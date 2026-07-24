#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ACAP.h"
#include "cJSON.h"
#include "media.h"
#include "migration.h"
#include "storage.h"

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
#define PATH_MAX_LEN 1024

typedef enum {
    MIGRATION_TASK_ACTIVE,
    MIGRATION_TASK_ARCHIVE
} MigrationTaskKind;

typedef struct {
    MigrationTaskKind kind;
    char id[128];
    char name[256];
    char input_path[PATH_MAX_LEN];
    char output_filename[PATH_MAX_LEN];
    int frames;
    int fps;
    double first;
    double last;
    double source_size;
} MigrationTask;

static GMutex migration_mutex;
static GPtrArray* migration_tasks = NULL;
static cJSON* migration_state = NULL;
static Migration_Complete_Callback complete_callback = NULL;
static int services_callback_queued = 0;
static int migration_cancel_requested = 0;

typedef struct {
    guint index;
    guint total;
    const char* current;
} MigrationProgressContext;

static int file_exists_nonempty(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int has_extension(const char* name, const char* extension) {
    if (!name || !extension) {
        return 0;
    }
    size_t name_len = strlen(name);
    size_t extension_len = strlen(extension);
    return name_len >= extension_len && g_ascii_strcasecmp(name + name_len - extension_len, extension) == 0;
}

static int is_legacy_media_file(const char* name) {
    return has_extension(name, ".avi") || has_extension(name, ".idx");
}

static int remove_file_if_exists(const char* path) {
    if (!path || unlink(path) == 0 || errno == ENOENT) {
        return 1;
    }
    LOG_WARN("%s: Failed to remove %s: %s\n", __func__, path, strerror(errno));
    return 0;
}

static int path_join3(char* out, size_t out_len, const char* a, const char* b, const char* c) {
    char temp[PATH_MAX_LEN];
    if (!storage_join(temp, sizeof(temp), a, b)) {
        return 0;
    }
    return storage_join(out, out_len, temp, c);
}

static int replace_extension(char* out, size_t out_len, const char* filename, const char* extension) {
    if (!out || !filename || !extension) {
        return 0;
    }

    const char* slash = strrchr(filename, '/');
    const char* base = slash ? slash + 1 : filename;
    char copy[PATH_MAX_LEN];
    if (snprintf(copy, sizeof(copy), "%s", base) <= 0 || strlen(base) >= sizeof(copy)) {
        return 0;
    }

    char* dot = strrchr(copy, '.');
    if (dot) {
        *dot = '\0';
    }

    int written = snprintf(out, out_len, "%s%s", copy, extension);
    return written > 0 && (size_t)written < out_len;
}

static char* read_file_text(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length < 0) {
        fclose(file);
        return NULL;
    }

    char* data = malloc((size_t)length + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }

    size_t read = fread(data, 1, (size_t)length, file);
    fclose(file);
    data[read] = '\0';
    return data;
}

static cJSON* read_json_file(const char* path) {
    char* text = read_file_text(path);
    if (!text) {
        return NULL;
    }
    cJSON* json = cJSON_Parse(text);
    free(text);
    return json;
}

static int write_json_file(const char* path, cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    if (!text) {
        return 0;
    }

    FILE* file = fopen(path, "w");
    if (!file) {
        free(text);
        return 0;
    }

    size_t written = fwrite(text, strlen(text), 1, file);
    fclose(file);
    free(text);
    return written == 1;
}

static int migration_state_path(char* out, size_t out_len) {
    return storage_join(out, out_len, storage_root(), "migration.json");
}

static int legacy_profiles_path(char* out, size_t out_len) {
    return storage_join(out, out_len, storage_root(), "timelapse.json");
}

static int legacy_recordings_path(char* out, size_t out_len) {
    return storage_join(out, out_len, storage_root(), "recordings.json");
}

static int directory_contains_legacy_media(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        return 0;
    }

    int found = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (is_legacy_media_file(entry->d_name)) {
            found = 1;
            break;
        }
    }

    closedir(dir);
    return found;
}

static int purge_legacy_profile_dirs(void) {
    const char* root = storage_root();
    DIR* dir = opendir(root);
    if (!dir) {
        return errno == ENOENT;
    }

    int ok = 1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, "archive") == 0 || strcmp(entry->d_name, "profiles") == 0) {
            continue;
        }

        char path[PATH_MAX_LEN];
        struct stat st;
        if (!storage_join(path, sizeof(path), root, entry->d_name) || lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) ||
            !directory_contains_legacy_media(path)) {
            continue;
        }

        if (!storage_remove_tree(path)) {
            LOG_WARN("%s: Failed to remove legacy recording directory %s\n", __func__, path);
            ok = 0;
        }
    }

    closedir(dir);
    return ok;
}

static int purge_legacy_archive_files(void) {
    char archive_dir[PATH_MAX_LEN];
    if (!storage_archive_dir(archive_dir, sizeof(archive_dir))) {
        return 0;
    }

    DIR* dir = opendir(archive_dir);
    if (!dir) {
        return errno == ENOENT;
    }

    int ok = 1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_legacy_media_file(entry->d_name)) {
            continue;
        }

        char path[PATH_MAX_LEN];
        if (!storage_join(path, sizeof(path), archive_dir, entry->d_name) || !remove_file_if_exists(path)) {
            ok = 0;
        }
    }

    closedir(dir);
    return ok;
}

static int purge_legacy_artifacts(void) {
    char profiles_path[PATH_MAX_LEN];
    int profile_dirs_ok = purge_legacy_profile_dirs();
    int archive_files_ok = purge_legacy_archive_files();
    int ok = profile_dirs_ok && archive_files_ok;
    if (legacy_profiles_path(profiles_path, sizeof(profiles_path)) && !remove_file_if_exists(profiles_path)) {
        ok = 0;
    }
    return ok;
}

static void set_state_string(const char* name, const char* value) {
    cJSON* item = cJSON_GetObjectItem(migration_state, name);
    if (item) {
        cJSON_SetValuestring(item, value ? value : "");
    } else {
        cJSON_AddStringToObject(migration_state, name, value ? value : "");
    }
}

static void set_state_number(const char* name, double value) {
    cJSON* item = cJSON_GetObjectItem(migration_state, name);
    if (item) {
        cJSON_SetNumberValue(item, value);
    } else {
        cJSON_AddNumberToObject(migration_state, name, value);
    }
}

static void set_state_bool(const char* name, int value) {
    cJSON* item = cJSON_GetObjectItem(migration_state, name);
    if (item) {
        item->type = value ? cJSON_True : cJSON_False;
    } else {
        cJSON_AddBoolToObject(migration_state, name, value);
    }
}

static void publish_state(void) {
    const char* status = cJSON_GetStringValue(cJSON_GetObjectItem(migration_state, "status"));
    const char* current = cJSON_GetStringValue(cJSON_GetObjectItem(migration_state, "current"));
    const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(migration_state, "message"));
    cJSON* required = cJSON_GetObjectItem(migration_state, "required");
    cJSON* total = cJSON_GetObjectItem(migration_state, "total");
    cJSON* completed = cJSON_GetObjectItem(migration_state, "completed");
    cJSON* current_index = cJSON_GetObjectItem(migration_state, "currentIndex");
    cJSON* current_bytes = cJSON_GetObjectItem(migration_state, "currentBytes");
    cJSON* file_progress = cJSON_GetObjectItem(migration_state, "fileProgress");

    ACAP_STATUS_SetBool("migration", "required", cJSON_IsTrue(required));
    ACAP_STATUS_SetString("migration", "status", status ? status : "unknown");
    ACAP_STATUS_SetString("migration", "current", current ? current : "");
    ACAP_STATUS_SetString("migration", "message", message ? message : "");
    ACAP_STATUS_SetNumber("migration", "total", total ? total->valuedouble : 0);
    ACAP_STATUS_SetNumber("migration", "completed", completed ? completed->valuedouble : 0);
    ACAP_STATUS_SetNumber("migration", "currentIndex", current_index ? current_index->valuedouble : 0);
    ACAP_STATUS_SetNumber("migration", "currentBytes", current_bytes ? current_bytes->valuedouble : 0);
    ACAP_STATUS_SetNumber("migration", "fileProgress", file_progress ? file_progress->valuedouble : 0);
}

static void save_state(void) {
    char path[PATH_MAX_LEN];
    if (migration_state_path(path, sizeof(path))) {
        write_json_file(path, migration_state);
    }
    publish_state();
}

static void set_state_defaults(const char* status, int required, int total, const char* message) {
    if (migration_state) {
        cJSON_Delete(migration_state);
    }
    migration_state = cJSON_CreateObject();
    cJSON_AddBoolToObject(migration_state, "required", required);
    cJSON_AddStringToObject(migration_state, "status", status ? status : "complete");
    cJSON_AddNumberToObject(migration_state, "total", total);
    cJSON_AddNumberToObject(migration_state, "completed", 0);
    cJSON_AddNumberToObject(migration_state, "currentIndex", 0);
    cJSON_AddNumberToObject(migration_state, "currentBytes", 0);
    cJSON_AddNumberToObject(migration_state, "fileProgress", 0);
    cJSON_AddNumberToObject(migration_state, "startedAt", 0);
    cJSON_AddStringToObject(migration_state, "current", "");
    cJSON_AddStringToObject(migration_state, "message", message ? message : "");
}

static void free_task(gpointer data) {
    g_free(data);
}

static void clear_tasks(void) {
    if (migration_tasks) {
        g_ptr_array_free(migration_tasks, TRUE);
    }
    migration_tasks = g_ptr_array_new_with_free_func(free_task);
}

static void add_task(MigrationTask* task) {
    if (!migration_tasks) {
        clear_tasks();
    }
    g_ptr_array_add(migration_tasks, task);
}

static const char* profile_name_for_id(cJSON* profiles, const char* profile_id) {
    cJSON* profile = NULL;
    cJSON_ArrayForEach(profile, profiles) {
        const char* id = cJSON_GetStringValue(cJSON_GetObjectItem(profile, "id"));
        if (id && profile_id && strcmp(id, profile_id) == 0) {
            const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(profile, "name"));
            return name ? name : profile_id;
        }
    }
    return profile_id ? profile_id : "Recording";
}

static void sanitize_filename(char* value) {
    for (char* ptr = value; ptr && *ptr; ++ptr) {
        if (*ptr == ' ' || *ptr == '/' || *ptr == '\\') {
            *ptr = '_';
        }
    }
}

static int task_filename_exists(const char* filename);

static void format_timestamp_suffix(double timestamp_ms, char* out, size_t out_len) {
    time_t timestamp = timestamp_ms > 0 ? (time_t)(timestamp_ms / 1000.0) : time(NULL);
    struct tm local_time;
    if (localtime_r(&timestamp, &local_time)) {
        strftime(out, out_len, "%Y_%m_%d_%H%M", &local_time);
    } else {
        snprintf(out, out_len, "unknown_date");
    }
}

static void build_active_output_filename(MigrationTask* task) {
    char safe_name[PATH_MAX_LEN];
    char date_suffix[64];
    snprintf(safe_name, sizeof(safe_name), "%s", task->name[0] ? task->name : "Recording");
    sanitize_filename(safe_name);
    format_timestamp_suffix(task->last > 0 ? task->last : task->first, date_suffix, sizeof(date_suffix));

    snprintf(task->output_filename, sizeof(task->output_filename), "%s_%s.mp4", safe_name, date_suffix);
    for (int suffix = 2; task_filename_exists(task->output_filename); ++suffix) {
        snprintf(task->output_filename, sizeof(task->output_filename), "%s_%s_%d.mp4", safe_name, date_suffix, suffix);
    }
}

static void add_active_tasks(cJSON* legacy_recordings, cJSON* legacy_profiles) {
    if (!legacy_recordings || !cJSON_IsObject(legacy_recordings)) {
        return;
    }

    cJSON* recording = legacy_recordings->child;
    while (recording) {
        const char* profile_id = recording->string;
        char avi_path[PATH_MAX_LEN];
        if (path_join3(avi_path, sizeof(avi_path), storage_root(), profile_id, "timelapse.avi") && file_exists_nonempty(avi_path)) {
            MigrationTask* task = g_new0(MigrationTask, 1);
            task->kind = MIGRATION_TASK_ACTIVE;
            snprintf(task->id, sizeof(task->id), "%s", profile_id);
            snprintf(task->name, sizeof(task->name), "%s", profile_name_for_id(legacy_profiles, profile_id));
            snprintf(task->input_path, sizeof(task->input_path), "%s", avi_path);
            task->frames = cJSON_GetObjectItem(recording, "images") ? cJSON_GetObjectItem(recording, "images")->valueint : 0;
            task->fps = cJSON_GetObjectItem(recording, "fps") ? cJSON_GetObjectItem(recording, "fps")->valueint : 10;
            task->first = cJSON_GetObjectItem(recording, "first") ? cJSON_GetObjectItem(recording, "first")->valuedouble : 0;
            task->last = cJSON_GetObjectItem(recording, "last") ? cJSON_GetObjectItem(recording, "last")->valuedouble : 0;
            task->source_size = cJSON_GetObjectItem(recording, "size") ? cJSON_GetObjectItem(recording, "size")->valuedouble : 0;
            build_active_output_filename(task);
            add_task(task);
        }
        recording = recording->next;
    }
}

static int task_filename_exists(const char* filename) {
    if (!migration_tasks || !filename) {
        return 0;
    }
    for (guint index = 0; index < migration_tasks->len; ++index) {
        MigrationTask* task = g_ptr_array_index(migration_tasks, index);
        if (task && strcmp(task->output_filename, filename) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_archive_task_from_entry(cJSON* entry) {
    const char* filename = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "filename"));
    if (!filename || !strstr(filename, ".avi")) {
        return;
    }

    char archive_dir[PATH_MAX_LEN];
    char avi_path[PATH_MAX_LEN];
    char output_filename[PATH_MAX_LEN];
    if (!storage_archive_dir(archive_dir, sizeof(archive_dir)) ||
        !storage_join(avi_path, sizeof(avi_path), archive_dir, filename) ||
        !file_exists_nonempty(avi_path) ||
        !replace_extension(output_filename, sizeof(output_filename), filename, ".mp4") ||
        task_filename_exists(output_filename)) {
        return;
    }

    MigrationTask* task = g_new0(MigrationTask, 1);
    task->kind = MIGRATION_TASK_ARCHIVE;
    snprintf(task->id, sizeof(task->id), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(entry, "id")) ? cJSON_GetStringValue(cJSON_GetObjectItem(entry, "id")) : "legacy");
    snprintf(task->name, sizeof(task->name), "%s", filename);
    snprintf(task->input_path, sizeof(task->input_path), "%s", avi_path);
    snprintf(task->output_filename, sizeof(task->output_filename), "%s", output_filename);
    task->frames = cJSON_GetObjectItem(entry, "frames") ? cJSON_GetObjectItem(entry, "frames")->valueint : 0;
    task->fps = cJSON_GetObjectItem(entry, "fps") ? cJSON_GetObjectItem(entry, "fps")->valueint : 10;
    task->first = cJSON_GetObjectItem(entry, "first") ? cJSON_GetObjectItem(entry, "first")->valuedouble : 0;
    task->last = cJSON_GetObjectItem(entry, "last") ? cJSON_GetObjectItem(entry, "last")->valuedouble : 0;
    task->source_size = cJSON_GetObjectItem(entry, "size") ? cJSON_GetObjectItem(entry, "size")->valuedouble : 0;
    add_task(task);
}

static void scan_unindexed_archives(void) {
    char archive_dir[PATH_MAX_LEN];
    if (!storage_archive_dir(archive_dir, sizeof(archive_dir))) {
        return;
    }

    DIR* dir = opendir(archive_dir);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strstr(entry->d_name, ".avi")) {
            continue;
        }

        char output_filename[PATH_MAX_LEN];
        if (!replace_extension(output_filename, sizeof(output_filename), entry->d_name, ".mp4") || task_filename_exists(output_filename)) {
            continue;
        }

        char avi_path[PATH_MAX_LEN];
        if (!storage_join(avi_path, sizeof(avi_path), archive_dir, entry->d_name) || !file_exists_nonempty(avi_path)) {
            continue;
        }

        MigrationTask* task = g_new0(MigrationTask, 1);
        task->kind = MIGRATION_TASK_ARCHIVE;
        snprintf(task->id, sizeof(task->id), "%s", "legacy");
        snprintf(task->name, sizeof(task->name), "%s", entry->d_name);
        snprintf(task->input_path, sizeof(task->input_path), "%s", avi_path);
        snprintf(task->output_filename, sizeof(task->output_filename), "%s", output_filename);
        task->fps = 10;
        add_task(task);
    }

    closedir(dir);
}

static void detect_legacy_tasks(void) {
    clear_tasks();

    char profiles_path[PATH_MAX_LEN];
    char recordings_path[PATH_MAX_LEN];
    char archive_index_path[PATH_MAX_LEN];
    cJSON* profiles = NULL;
    cJSON* recordings = NULL;
    cJSON* archives = NULL;

    if (legacy_profiles_path(profiles_path, sizeof(profiles_path))) {
        profiles = read_json_file(profiles_path);
    }
    if (legacy_recordings_path(recordings_path, sizeof(recordings_path))) {
        recordings = read_json_file(recordings_path);
    }
    if (storage_archive_index_path(archive_index_path, sizeof(archive_index_path))) {
        archives = read_json_file(archive_index_path);
    }

    add_active_tasks(recordings, profiles);

    if (archives && cJSON_IsArray(archives)) {
        cJSON* archive = NULL;
        cJSON_ArrayForEach(archive, archives) {
            add_archive_task_from_entry(archive);
        }
    }
    scan_unindexed_archives();

    if (profiles) {
        cJSON_Delete(profiles);
    }
    if (recordings) {
        cJSON_Delete(recordings);
    }
    if (archives) {
        cJSON_Delete(archives);
    }
}

static int import_profiles_if_needed(void) {
    char new_profiles_path[PATH_MAX_LEN];
    char old_profiles_path[PATH_MAX_LEN];
    if (!storage_profiles_path(new_profiles_path, sizeof(new_profiles_path)) ||
        !legacy_profiles_path(old_profiles_path, sizeof(old_profiles_path)) ||
        file_exists_nonempty(new_profiles_path) ||
        !file_exists_nonempty(old_profiles_path)) {
        return 1;
    }

    cJSON* profiles = read_json_file(old_profiles_path);
    if (!profiles) {
        return 0;
    }
    int ok = write_json_file(new_profiles_path, profiles);
    cJSON_Delete(profiles);
    return ok;
}

static int write_recording_list(cJSON* recordings) {
    char path[PATH_MAX_LEN];
    if (!storage_recordings_path(path, sizeof(path))) {
        return 0;
    }
    return write_json_file(path, recordings);
}

static cJSON* archive_entry_from_task(MigrationTask* task, const char* output_path) {
    struct stat st;
    long long output_size = stat(output_path, &st) == 0 ? (long long)st.st_size : 0;
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "id", task->id[0] ? task->id : "legacy");
    cJSON_AddStringToObject(entry, "filename", task->output_filename);
    cJSON_AddNumberToObject(entry, "size", (double)output_size);
    cJSON_AddNumberToObject(entry, "sourceSize", task->source_size);
    cJSON_AddNumberToObject(entry, "frames", task->frames);
    cJSON_AddNumberToObject(entry, "fps", task->fps > 0 ? task->fps : 10);
    cJSON_AddNumberToObject(entry, "first", task->first);
    cJSON_AddNumberToObject(entry, "last", task->last);
    cJSON_AddStringToObject(entry, "container", "mp4");
    cJSON_AddStringToObject(entry, "codec", "h264");
    cJSON_AddBoolToObject(entry, "migrated", 1);
    cJSON_AddStringToObject(entry, "sourceContainer", "avi");
    return entry;
}

static cJSON* recording_entry_from_task(MigrationTask* task) {
    cJSON* entry = cJSON_CreateObject();
    int frames = task->frames > 0 ? task->frames : 0;
    cJSON_AddNumberToObject(entry, "first", task->first);
    cJSON_AddNumberToObject(entry, "last", task->last > 0 ? task->last : task->first);
    cJSON_AddNumberToObject(entry, "frames", frames);
    cJSON_AddNumberToObject(entry, "images", frames);
    cJSON_AddNumberToObject(entry, "sizeBytes", task->source_size);
    cJSON_AddNumberToObject(entry, "size", task->source_size);
    cJSON_AddNumberToObject(entry, "fps", task->fps > 0 ? task->fps : 10);
    cJSON_AddNumberToObject(entry, "nextFrame", frames + 1);
    cJSON_AddBoolToObject(entry, "migrated", 1);
    cJSON_AddStringToObject(entry, "sourceContainer", "avi");
    return entry;
}

static gboolean run_complete_callback(gpointer user_data) {
    services_callback_queued = 0;
    if (complete_callback) {
        complete_callback();
    }
    return G_SOURCE_REMOVE;
}

static void queue_complete_callback(void) {
    if (!services_callback_queued) {
        services_callback_queued = 1;
        g_idle_add(run_complete_callback, NULL);
    }
}

static int migration_is_cancel_requested(void) {
    int requested;
    g_mutex_lock(&migration_mutex);
    requested = migration_cancel_requested;
    g_mutex_unlock(&migration_mutex);
    return requested;
}

static int migration_progress_callback(double percent, const char* detail, void* user_data) {
    MigrationProgressContext* context = (MigrationProgressContext*)user_data;
    if (!context) {
        return 1;
    }

    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;

    g_mutex_lock(&migration_mutex);
    set_state_number("completed", (double)context->index);
    set_state_number("currentIndex", (double)context->index + 1.0);
    set_state_number("fileProgress", percent);
    set_state_string("current", context->current ? context->current : "");
    set_state_string("message", migration_cancel_requested ? "Cancelling migration..." : (detail ? detail : "Converting AVI to MP4..."));
    save_state();
    int keep_running = !migration_cancel_requested;
    g_mutex_unlock(&migration_mutex);
    return keep_running;
}

static void set_migration_cancelled_state(void) {
    migration_cancel_requested = 0;
    set_state_string("status", "cancelled");
    set_state_bool("required", 1);
    set_state_number("currentIndex", 0);
    set_state_number("currentBytes", 0);
    set_state_number("fileProgress", 0.0);
    set_state_string("current", "");
    set_state_string("message", "Migration cancelled. AVI files were left unchanged. Reinstall version 2.x.x to continue using AVI recordings, or start conversion again.");
}

static gpointer migration_thread(gpointer user_data) {
    cJSON* new_archive_list = cJSON_CreateArray();
    cJSON* new_recording_list = cJSON_CreateObject();
    char archive_dir[PATH_MAX_LEN];
    char archive_index_path[PATH_MAX_LEN];
    char error[256];

    if (!storage_archive_dir(archive_dir, sizeof(archive_dir)) ||
        !storage_archive_index_path(archive_index_path, sizeof(archive_index_path)) ||
        !storage_ensure_directory(archive_dir, error, sizeof(error))) {
        g_mutex_lock(&migration_mutex);
        set_state_string("status", "failed");
        set_state_string("message", "Unable to prepare archive storage for migration.");
        save_state();
        g_mutex_unlock(&migration_mutex);
        cJSON_Delete(new_archive_list);
        cJSON_Delete(new_recording_list);
        return NULL;
    }

    if (!import_profiles_if_needed()) {
        g_mutex_lock(&migration_mutex);
        set_state_string("status", "failed");
        set_state_string("message", "Unable to migrate recording settings.");
        save_state();
        g_mutex_unlock(&migration_mutex);
        cJSON_Delete(new_archive_list);
        cJSON_Delete(new_recording_list);
        return NULL;
    }

    for (guint index = 0; migration_tasks && index < migration_tasks->len; ++index) {
        MigrationTask* task = g_ptr_array_index(migration_tasks, index);
        char output_path[PATH_MAX_LEN];
        if (!task) {
            continue;
        }

        int output_ok = task->kind == MIGRATION_TASK_ACTIVE
            ? storage_export_path(output_path, sizeof(output_path), task->id, task->fps > 0 ? task->fps : 10)
            : storage_join(output_path, sizeof(output_path), archive_dir, task->output_filename);
        if (!output_ok) {
            continue;
        }

        if (migration_is_cancel_requested()) {
            g_mutex_lock(&migration_mutex);
            set_migration_cancelled_state();
            save_state();
            g_mutex_unlock(&migration_mutex);
            cJSON_Delete(new_archive_list);
            cJSON_Delete(new_recording_list);
            return NULL;
        }

        g_mutex_lock(&migration_mutex);
        set_state_string("current", task->name[0] ? task->name : task->input_path);
        set_state_number("completed", (double)index);
        set_state_number("currentIndex", (double)index + 1.0);
        set_state_number("currentBytes", task->source_size);
        set_state_number("fileProgress", 0.0);
        set_state_number("startedAt", (double)time(NULL));
        set_state_string("message", "Converting AVI to MP4...");
        save_state();
        g_mutex_unlock(&migration_mutex);

        unlink(output_path);
        MigrationProgressContext progress_context = {
            .index = index,
            .total = migration_tasks ? migration_tasks->len : 0,
            .current = task->name[0] ? task->name : task->input_path
        };
        int convert_ok = task->kind == MIGRATION_TASK_ACTIVE
            ? media_import_avi_to_live_mp4(task->input_path, task->id, task->fps > 0 ? task->fps : 10, task->frames, migration_progress_callback, &progress_context)
            : media_convert_avi_to_mp4(task->input_path, output_path, migration_progress_callback, &progress_context);
        if (!convert_ok) {
            g_mutex_lock(&migration_mutex);
            if (migration_cancel_requested) {
                unlink(output_path);
                set_migration_cancelled_state();
            } else {
                set_state_string("status", "failed");
                set_state_string("message", media_last_error());
            }
            save_state();
            g_mutex_unlock(&migration_mutex);
            cJSON_Delete(new_archive_list);
            cJSON_Delete(new_recording_list);
            return NULL;
        }

        if (task->kind == MIGRATION_TASK_ACTIVE) {
            cJSON_AddItemToObject(new_recording_list, task->id, recording_entry_from_task(task));
        } else {
            cJSON_AddItemToArray(new_archive_list, archive_entry_from_task(task, output_path));
        }
        g_mutex_lock(&migration_mutex);
        set_state_number("completed", (double)index + 1.0);
        set_state_number("fileProgress", 100.0);
        set_state_string("message", "Converted AVI to MP4.");
        save_state();
        g_mutex_unlock(&migration_mutex);
    }

    if (!write_json_file(archive_index_path, new_archive_list) || !write_recording_list(new_recording_list)) {
        g_mutex_lock(&migration_mutex);
        set_state_string("status", "failed");
        set_state_string("message", "Unable to save migrated recording index.");
        save_state();
        g_mutex_unlock(&migration_mutex);
        cJSON_Delete(new_archive_list);
        cJSON_Delete(new_recording_list);
        return NULL;
    }

    cJSON_Delete(new_archive_list);
    cJSON_Delete(new_recording_list);

    if (!purge_legacy_artifacts()) {
        g_mutex_lock(&migration_mutex);
        set_state_string("status", "failed");
        set_state_string("message", "MP4 migration completed, but legacy AVI files could not be purged.");
        save_state();
        g_mutex_unlock(&migration_mutex);
        return NULL;
    }

    g_mutex_lock(&migration_mutex);
    set_state_defaults("complete", 0, 0, "");
    save_state();
    g_mutex_unlock(&migration_mutex);

    queue_complete_callback();
    return NULL;
}

static void http_migration(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method) {
        ACAP_HTTP_Respond_Error(response, 400, "Invalid Request Method");
        return;
    }

    if (strcmp(method, "GET") == 0) {
        cJSON* state = Migration_State_JSON();
        ACAP_HTTP_Respond_JSON(response, state);
        cJSON_Delete(state);
        return;
    }

    if (strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
        return;
    }

    cJSON* body = request->postData ? cJSON_Parse(request->postData) : NULL;
    const char* action = cJSON_GetStringValue(body ? cJSON_GetObjectItem(body, "action") : NULL);
    if (!action) {
        if (body) {
            cJSON_Delete(body);
        }
        ACAP_HTTP_Respond_Error(response, 400, "Missing migration action");
        return;
    }

    if (strcmp(action, "decline") == 0) {
        g_mutex_lock(&migration_mutex);
        const char* status = cJSON_GetStringValue(cJSON_GetObjectItem(migration_state, "status"));
        if (status && strcmp(status, "running") == 0) {
            migration_cancel_requested = 1;
            set_state_string("status", "cancelling");
            set_state_string("message", "Cancelling migration...");
        } else {
            set_migration_cancelled_state();
        }
        save_state();
        g_mutex_unlock(&migration_mutex);
        cJSON_Delete(body);
        ACAP_HTTP_Respond_Text(response, "Migration cancellation requested");
        return;
    }

    if (strcmp(action, "start") == 0) {
        g_mutex_lock(&migration_mutex);
        const char* status = cJSON_GetStringValue(cJSON_GetObjectItem(migration_state, "status"));
        if (status && strcmp(status, "running") == 0) {
            g_mutex_unlock(&migration_mutex);
            cJSON_Delete(body);
            ACAP_HTTP_Respond_Text(response, "Migration already running");
            return;
        }

        detect_legacy_tasks();
        migration_cancel_requested = 0;
        set_state_string("status", "running");
        set_state_bool("required", 1);
        set_state_number("total", migration_tasks ? (double)migration_tasks->len : 0);
        set_state_number("completed", 0);
        set_state_string("message", "Starting migration...");
        save_state();
        g_mutex_unlock(&migration_mutex);

        GThread* thread = g_thread_new("avi-migration", migration_thread, NULL);
        if (!thread) {
            g_mutex_lock(&migration_mutex);
            set_state_string("status", "failed");
            set_state_string("message", "Unable to start migration worker.");
            save_state();
            g_mutex_unlock(&migration_mutex);
            cJSON_Delete(body);
            ACAP_HTTP_Respond_Error(response, 500, "Unable to start migration worker");
            return;
        }
        g_thread_unref(thread);
        cJSON_Delete(body);
        ACAP_HTTP_Respond_Text(response, "Migration started");
        return;
    }

    cJSON_Delete(body);
    ACAP_HTTP_Respond_Error(response, 400, "Unsupported migration action");
}

int Migration_Init(Migration_Complete_Callback callback) {
    complete_callback = callback;
    ACAP_HTTP_Node("migration", http_migration);

    char path[PATH_MAX_LEN];
    if (migration_state_path(path, sizeof(path))) {
        migration_state = read_json_file(path);
    }

    if (migration_state) {
        const char* status = cJSON_GetStringValue(cJSON_GetObjectItem(migration_state, "status"));
        if (status && strcmp(status, "complete") == 0) {
            if (!purge_legacy_artifacts()) {
                set_state_defaults("failed", 1, 0, "Legacy AVI files could not be purged.");
                save_state();
                return 1;
            }
            set_state_defaults("complete", 0, 0, "");
            save_state();
            return 0;
        }
        if (status && (strcmp(status, "cancelled") == 0 || strcmp(status, "declined") == 0 || strcmp(status, "failed") == 0)) {
            publish_state();
            return 1;
        }
    }

    detect_legacy_tasks();
    int total = migration_tasks ? (int)migration_tasks->len : 0;
    if (total == 0) {
        if (!import_profiles_if_needed()) {
            set_state_defaults("failed", 1, 0, "Unable to migrate recording settings.");
        } else {
            set_state_defaults("complete", 0, 0, "No AVI recordings found. No migration needed.");
        }
        save_state();
        return 0;
    }

    set_state_defaults("pending", 1, total, "AVI recordings must be migrated before Timelapse can continue.");
    save_state();
    return 1;
}

int Migration_Is_Pending(void) {
    if (!migration_state) {
        return 0;
    }
    const char* status = cJSON_GetStringValue(cJSON_GetObjectItem(migration_state, "status"));
    return status && (strcmp(status, "pending") == 0 || strcmp(status, "running") == 0 || strcmp(status, "cancelling") == 0 || strcmp(status, "cancelled") == 0 || strcmp(status, "declined") == 0 || strcmp(status, "failed") == 0);
}

cJSON* Migration_State_JSON(void) {
    g_mutex_lock(&migration_mutex);
    cJSON* copy = migration_state ? cJSON_Duplicate(migration_state, 1) : cJSON_CreateObject();
    g_mutex_unlock(&migration_mutex);
    return copy;
}
