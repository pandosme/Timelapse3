#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <pthread.h>

#include <axsdk/axstorage.h>

#include "storage.h"

#define STORAGE_ID "SD_DISK"
#define STORAGE_DIRECTORY "timelapse2"
#define EXISTING_STORAGE_DIRECTORY "root/timelapse2"

static char active_storage_base[1024];
static char active_storage_root[1024];
static int active_storage_root_initialized = 0;
static int active_storage_usable = 0;
static pthread_mutex_t storage_root_mutex = PTHREAD_MUTEX_INITIALIZER;
static AXStorage* active_storage = NULL;
static guint storage_subscription_id = 0;
static int storage_setup_pending = 0;
static int storage_available = 0;
static int storage_writable = 0;
static int storage_full = 0;
static int storage_exiting = 0;
static StorageInitCallback storage_init_callback = NULL;
static void* storage_init_user_data = NULL;
static _Thread_local char storage_base_snapshot[1024];
static _Thread_local char storage_root_snapshot[1024];

static int dir_exists(const char* path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int set_error(char* error, size_t error_len, const char* fmt, const char* path) {
    if (error && error_len > 0) {
        snprintf(error, error_len, fmt, path, strerror(errno));
    }
    return 0;
}

const char* storage_root(void) {
    pthread_mutex_lock(&storage_root_mutex);
    snprintf(storage_root_snapshot, sizeof(storage_root_snapshot), "%s", active_storage_root);
    pthread_mutex_unlock(&storage_root_mutex);
    return storage_root_snapshot;
}

const char* storage_base(void) {
    pthread_mutex_lock(&storage_root_mutex);
    snprintf(storage_base_snapshot, sizeof(storage_base_snapshot), "%s", active_storage_base);
    pthread_mutex_unlock(&storage_root_mutex);
    return storage_base_snapshot;
}

static int ensure_root_candidate(const char* candidate, char* error, size_t error_len) {
    struct stat st;

    if (stat(candidate, &st) == -1) {
        if (errno != ENOENT) {
            return set_error(error, error_len, "Directory check failed: %s (%s)", candidate);
        }
        if (mkdir(candidate, 0755) == -1) {
            return set_error(error, error_len, "Creation failed: %s (%s)", candidate);
        }
    } else if (!S_ISDIR(st.st_mode)) {
        if (error && error_len > 0) {
            snprintf(error, error_len, "Storage path is not a directory: %s", candidate);
        }
        return 0;
    }

    if (access(candidate, R_OK | W_OK | X_OK) == -1) {
        return set_error(error, error_len, "Directory inaccessible: %s (%s)", candidate);
    }

    if (error && error_len > 0) {
        error[0] = '\0';
    }
    return 1;
}

int storage_join(char* out, size_t out_len, const char* first, const char* second) {
    if (!out || !out_len || !first || !second) {
        return 0;
    }

    int written = snprintf(out, out_len, "%s/%s", first, second);
    return written > 0 && (size_t)written < out_len;
}

static void notify_storage_state(int available, const char* message) {
    StorageInitCallback callback;
    void* user_data;
    pthread_mutex_lock(&storage_root_mutex);
    callback = storage_init_callback;
    user_data = storage_init_user_data;
    pthread_mutex_unlock(&storage_root_mutex);
    if (callback) {
        callback(available, message, user_data);
    }
}

static void storage_release_cb(gpointer user_data, GError* error) {
    const char* storage_id = user_data;
    if (error) {
        syslog(LOG_WARNING, "Failed to release %s: %s", storage_id, error->message);
        g_error_free(error);
    } else {
        syslog(LOG_INFO, "Release of %s was successful", storage_id);
    }
}

static void release_storage_handle(AXStorage* storage) {
    if (!storage) {
        return;
    }
    GError* error = NULL;
    if (!ax_storage_release_async(storage, storage_release_cb, (gpointer)STORAGE_ID, &error) && error) {
        syslog(LOG_WARNING, "Failed to release Axis storage: %s", error->message);
        g_error_free(error);
    }
}

static void release_active_storage(void) {
    AXStorage* storage = NULL;
    pthread_mutex_lock(&storage_root_mutex);
    storage = active_storage;
    active_storage = NULL;
    active_storage_base[0] = '\0';
    active_storage_root[0] = '\0';
    active_storage_root_initialized = 0;
    active_storage_usable = 0;
    pthread_mutex_unlock(&storage_root_mutex);

    release_storage_handle(storage);
}

static void storage_setup_cb(AXStorage* storage, gpointer user_data, GError* error) {
    (void)user_data;
    pthread_mutex_lock(&storage_root_mutex);
    storage_setup_pending = 0;
    pthread_mutex_unlock(&storage_root_mutex);

    if (!storage || error) {
        char message[256];
        snprintf(message, sizeof(message), "Axis Storage API setup failed: %s",
                 error ? error->message : "no storage handle");
        if (error) {
            g_error_free(error);
        }
        notify_storage_state(0, message);
        return;
    }

    GError* id_error = NULL;
    gchar* storage_id = ax_storage_get_storage_id(storage, &id_error);
    if (!storage_id || id_error || strcmp(storage_id, STORAGE_ID) != 0) {
        char message[256];
        snprintf(message, sizeof(message), "Axis Storage API returned an invalid storage ID: %s",
                 id_error ? id_error->message : (storage_id ? storage_id : "empty ID"));
        if (id_error) {
            g_error_free(id_error);
        }
        g_free(storage_id);
        release_storage_handle(storage);
        notify_storage_state(0, message);
        return;
    }
    g_free(storage_id);

    GError* path_error = NULL;
    gchar* api_path = ax_storage_get_path(storage, &path_error);
    if (!api_path || path_error) {
        char message[256];
        snprintf(message, sizeof(message), "Axis Storage API path failed: %s",
                 path_error ? path_error->message : "empty path");
        if (path_error) {
            g_error_free(path_error);
        }
        g_free(api_path);
        release_storage_handle(storage);
        notify_storage_state(0, message);
        return;
    }

    size_t path_len = strlen(api_path);
    while (path_len > 1 && api_path[path_len - 1] == '/') {
        api_path[--path_len] = '\0';
    }
    char* slash = strrchr(api_path, '/');
    char base[1024];
    char root[1024];
    if (!slash || slash == api_path || (size_t)(slash - api_path) >= sizeof(base)) {
        notify_storage_state(0, "Axis Storage API returned an invalid path");
        g_free(api_path);
        release_storage_handle(storage);
        return;
    }
    memcpy(base, api_path, (size_t)(slash - api_path));
    base[slash - api_path] = '\0';
    if (!storage_join(root, sizeof(root), base, STORAGE_DIRECTORY) || strcmp(root, api_path) != 0) {
        notify_storage_state(0, "Axis Storage API path does not match the application directory");
        g_free(api_path);
        release_storage_handle(storage);
        return;
    }

    char existing_root[1024];
    if (!storage_join(existing_root, sizeof(existing_root), base, EXISTING_STORAGE_DIRECTORY)) {
        notify_storage_state(0, "Existing storage path is too long");
        g_free(api_path);
        release_storage_handle(storage);
        return;
    }
    if (dir_exists(existing_root)) {
        snprintf(root, sizeof(root), "%s", existing_root);
        syslog(LOG_INFO, "Using existing Timelapse storage at %s", root);
    }

    char setup_error[256];
    if (!ensure_root_candidate(root, setup_error, sizeof(setup_error))) {
        notify_storage_state(0, setup_error);
        g_free(api_path);
        release_storage_handle(storage);
        return;
    }

    pthread_mutex_lock(&storage_root_mutex);
    int storage_is_usable = storage_available && storage_writable && !storage_full && !storage_exiting;
    if (!storage_is_usable) {
        pthread_mutex_unlock(&storage_root_mutex);
        g_free(api_path);
        release_storage_handle(storage);
        notify_storage_state(0, "SD card became unavailable during setup");
        return;
    }
    active_storage = storage;
    snprintf(active_storage_base, sizeof(active_storage_base), "%s", base);
    snprintf(active_storage_root, sizeof(active_storage_root), "%s", root);
    active_storage_root_initialized = 1;
    active_storage_usable = 1;
    pthread_mutex_unlock(&storage_root_mutex);
    g_free(api_path);
    notify_storage_state(1, "");
}

static int get_storage_status(AXStorageStatusEventId event, int* value,
                              char* error_message, size_t error_len) {
    GError* error = NULL;
    *value = ax_storage_get_status((gchar*)STORAGE_ID, event, &error);
    if (!error) {
        return 1;
    }
    snprintf(error_message, error_len, "Axis Storage API status failed: %s", error->message);
    g_error_free(error);
    return 0;
}

static void storage_status_cb(gchar* storage_id, gpointer user_data, GError* error) {
    (void)user_data;
    if (strcmp(storage_id, STORAGE_ID) != 0) {
        if (error) g_error_free(error);
        return;
    }
    if (error) {
        char message[256];
        snprintf(message, sizeof(message), "Axis Storage API subscription failed: %s", error->message);
        g_error_free(error);
        pthread_mutex_lock(&storage_root_mutex);
        active_storage_usable = 0;
        pthread_mutex_unlock(&storage_root_mutex);
        notify_storage_state(0, message);
        return;
    }

    int available = 0;
    int writable = 0;
    int full = 0;
    int exiting = 0;
    char status_error[256];
    if (!get_storage_status(AX_STORAGE_AVAILABLE_EVENT, &available, status_error, sizeof(status_error)) ||
        !get_storage_status(AX_STORAGE_WRITABLE_EVENT, &writable, status_error, sizeof(status_error)) ||
        !get_storage_status(AX_STORAGE_FULL_EVENT, &full, status_error, sizeof(status_error)) ||
        !get_storage_status(AX_STORAGE_EXITING_EVENT, &exiting, status_error, sizeof(status_error))) {
        pthread_mutex_lock(&storage_root_mutex);
        active_storage_usable = 0;
        pthread_mutex_unlock(&storage_root_mutex);
        notify_storage_state(0, status_error);
        return;
    }

    pthread_mutex_lock(&storage_root_mutex);
    storage_available = available;
    storage_writable = writable;
    storage_full = full;
    storage_exiting = exiting;
    pthread_mutex_unlock(&storage_root_mutex);
    syslog(LOG_INFO, "Status of events for %s: %swritable, %savailable, %sexiting, %sfull",
           storage_id, writable ? "" : "not ", available ? "" : "not ",
           exiting ? "" : "not ", full ? "" : "not ");

    if (!available || !writable || full || exiting) {
        pthread_mutex_lock(&storage_root_mutex);
        active_storage_usable = 0;
        pthread_mutex_unlock(&storage_root_mutex);
        if (exiting) {
            release_active_storage();
        }
        const char* message = exiting ? "SD card is being removed" :
            (!available ? "SD card is unavailable" : (!writable ? "SD card is read-only" : "SD card is full"));
        notify_storage_state(0, message);
        return;
    }

    pthread_mutex_lock(&storage_root_mutex);
    int should_setup = !active_storage && !storage_setup_pending;
    active_storage_usable = active_storage != NULL;
    if (should_setup) {
        storage_setup_pending = 1;
    }
    pthread_mutex_unlock(&storage_root_mutex);
    if (!should_setup) {
        notify_storage_state(1, "");
        return;
    }

    syslog(LOG_INFO, "Setup %s", storage_id);
    GError* setup_error = NULL;
    if (!ax_storage_setup_async((gchar*)STORAGE_ID, storage_setup_cb, NULL, &setup_error)) {
        pthread_mutex_lock(&storage_root_mutex);
        storage_setup_pending = 0;
        pthread_mutex_unlock(&storage_root_mutex);
        char message[256];
        snprintf(message, sizeof(message), "Axis Storage API setup could not start: %s",
                 setup_error ? setup_error->message : "unknown error");
        if (setup_error) g_error_free(setup_error);
        notify_storage_state(0, message);
    }
}

int storage_initialize_async(StorageInitCallback callback, void* user_data) {
    pthread_mutex_lock(&storage_root_mutex);
    storage_init_callback = callback;
    storage_init_user_data = user_data;
    pthread_mutex_unlock(&storage_root_mutex);

    GError* list_error = NULL;
    GList* storage_list = ax_storage_list(&list_error);
    if (list_error) {
        char message[256];
        snprintf(message, sizeof(message), "Axis Storage API list failed: %s", list_error->message);
        g_error_free(list_error);
        notify_storage_state(0, message);
        return 0;
    }
    int found = 0;
    for (GList* node = storage_list; node; node = node->next) {
        if (strcmp((const char*)node->data, STORAGE_ID) == 0) {
            found = 1;
        }
        g_free(node->data);
    }
    g_list_free(storage_list);
    if (!found) {
        notify_storage_state(0, "Axis Storage API did not find SD_DISK");
        return 0;
    }

    GError* subscribe_error = NULL;
    guint subscription_id = ax_storage_subscribe((gchar*)STORAGE_ID, storage_status_cb, NULL, &subscribe_error);
    if (!subscription_id || subscribe_error) {
        char message[256];
        snprintf(message, sizeof(message), "Axis Storage API subscription failed: %s",
                 subscribe_error ? subscribe_error->message : "unknown error");
        if (subscribe_error) g_error_free(subscribe_error);
        notify_storage_state(0, message);
        return 0;
    }
    pthread_mutex_lock(&storage_root_mutex);
    storage_subscription_id = subscription_id;
    pthread_mutex_unlock(&storage_root_mutex);
    syslog(LOG_INFO, "Subscribed to events of %s", STORAGE_ID);
    return 1;
}

void storage_cleanup(void) {
    guint subscription_id;
    pthread_mutex_lock(&storage_root_mutex);
    subscription_id = storage_subscription_id;
    storage_subscription_id = 0;
    storage_init_callback = NULL;
    storage_init_user_data = NULL;
    pthread_mutex_unlock(&storage_root_mutex);

    release_active_storage();
    if (subscription_id) {
        GError* error = NULL;
        ax_storage_unsubscribe(subscription_id, &error);
        if (error) {
            syslog(LOG_WARNING, "Axis Storage API unsubscribe failed: %s", error->message);
            g_error_free(error);
        } else {
            syslog(LOG_INFO, "Unsubscribed events of %s", STORAGE_ID);
        }
    }
}

static int storage_ensure_root_unlocked(char* error, size_t error_len) {
    if (active_storage_root_initialized && active_storage_usable) {
        return ensure_root_candidate(active_storage_root, error, error_len);
    }
    if (error && error_len > 0) {
        snprintf(error, error_len, "SD card is not ready through the Axis Storage API");
    }
    return 0;
}

int storage_ensure_root(char* error, size_t error_len) {
    pthread_mutex_lock(&storage_root_mutex);
    int result = storage_ensure_root_unlocked(error, error_len);
    pthread_mutex_unlock(&storage_root_mutex);
    return result;
}

int storage_ensure_directory(const char* path, char* error, size_t error_len) {
    struct stat st;

    if (!path) {
        if (error && error_len > 0) {
            snprintf(error, error_len, "Invalid directory path");
        }
        return 0;
    }

    if (stat(path, &st) == -1) {
        if (errno != ENOENT) {
            return set_error(error, error_len, "Directory check failed: %s (%s)", path);
        }
        if (mkdir(path, 0755) == -1) {
            return set_error(error, error_len, "Creation failed: %s (%s)", path);
        }
    } else if (!S_ISDIR(st.st_mode)) {
        if (error && error_len > 0) {
            snprintf(error, error_len, "Path is not a directory: %s", path);
        }
        return 0;
    }

    if (error && error_len > 0) {
        error[0] = '\0';
    }
    return 1;
}

int storage_remove_tree(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        return errno == ENOENT;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[1024];
        if (!storage_join(child, sizeof(child), path, entry->d_name)) {
            closedir(dir);
            return 0;
        }

        struct stat st;
        if (lstat(child, &st) == -1) {
            closedir(dir);
            return 0;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!storage_remove_tree(child)) {
                closedir(dir);
                return 0;
            }
        } else if (unlink(child) == -1) {
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return rmdir(path) == 0;
}

int storage_reset(char* error, size_t error_len) {
    if (!storage_ensure_root(error, error_len)) {
        return 0;
    }
    const char* root = storage_root();
    if (!storage_remove_tree(root)) {
        return set_error(error, error_len, "Failed to remove storage tree: %s (%s)", root);
    }
    return storage_ensure_root(error, error_len);
}

/* Anything that arrives from an HTTP parameter and ends up as a path component has to
   pass here first. A profile id or a filename is one name, never a path: "../.." walked
   out of the storage tree entirely, which for the delete paths means removing a directory
   that has nothing to do with this app. */
int storage_name_is_safe(const char* name) {
    if (!name || !name[0]) {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (const unsigned char* c = (const unsigned char*)name; *c; c++) {
        if (*c == '/' || *c == '\\' || *c < 0x20) {
            return 0;
        }
    }
    return 1;
}

int storage_profile_dir(char* out, size_t out_len, const char* profile_id) {
    char profiles_dir[1024];
    if (!storage_name_is_safe(profile_id)) {
        return 0;
    }
    if (!storage_profiles_dir(profiles_dir, sizeof(profiles_dir))) {
        return 0;
    }
    return storage_join(out, out_len, profiles_dir, profile_id);
}

int storage_profiles_dir(char* out, size_t out_len) {
    return storage_join(out, out_len, storage_root(), "profiles");
}

int storage_frames_dir(char* out, size_t out_len, const char* profile_id) {
    char profile_dir[1024];
    if (!storage_profile_dir(profile_dir, sizeof(profile_dir), profile_id)) {
        return 0;
    }
    return storage_join(out, out_len, profile_dir, "frames");
}

int storage_frame_path(char* out, size_t out_len, const char* profile_id, unsigned frame_number) {
    char frames_dir[1024];
    if (!storage_frames_dir(frames_dir, sizeof(frames_dir), profile_id)) {
        return 0;
    }

    char filename[32];
    int written = snprintf(filename, sizeof(filename), "%08u.jpg", frame_number);
    if (written <= 0 || (size_t)written >= sizeof(filename)) {
        return 0;
    }

    return storage_join(out, out_len, frames_dir, filename);
}

int storage_cache_dir(char* out, size_t out_len, const char* profile_id) {
    char profile_dir[1024];
    if (!storage_profile_dir(profile_dir, sizeof(profile_dir), profile_id)) {
        return 0;
    }
    return storage_join(out, out_len, profile_dir, "cache");
}

int storage_preview_path(char* out, size_t out_len, const char* profile_id, int fps) {
    char cache_dir[1024];
    if (!storage_cache_dir(cache_dir, sizeof(cache_dir), profile_id)) {
        return 0;
    }

    char filename[64];
    int written = snprintf(filename, sizeof(filename), "preview_%dfps.mp4", fps);
    if (written <= 0 || (size_t)written >= sizeof(filename)) {
        return 0;
    }
    return storage_join(out, out_len, cache_dir, filename);
}

int storage_export_path(char* out, size_t out_len, const char* profile_id, int fps) {
    char cache_dir[1024];
    if (!storage_cache_dir(cache_dir, sizeof(cache_dir), profile_id)) {
        return 0;
    }

    char filename[64];
    int written = snprintf(filename, sizeof(filename), "export_%dfps.mp4", fps);
    if (written <= 0 || (size_t)written >= sizeof(filename)) {
        return 0;
    }
    return storage_join(out, out_len, cache_dir, filename);
}

int storage_recordings_path(char* out, size_t out_len) {
    return storage_join(out, out_len, storage_root(), "recordings.json");
}

int storage_profiles_path(char* out, size_t out_len) {
    return storage_join(out, out_len, storage_root(), "profiles.json");
}

int storage_archive_dir(char* out, size_t out_len) {
    return storage_join(out, out_len, storage_root(), "archive");
}

int storage_archive_index_path(char* out, size_t out_len) {
    char archive_dir[1024];
    if (!storage_archive_dir(archive_dir, sizeof(archive_dir))) {
        return 0;
    }
    return storage_join(out, out_len, archive_dir, "archives.json");
}