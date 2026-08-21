#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "ACAP.h"
#include "capture.h"
#include "recording_store.h"
#include "storage.h"

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }

static cJSON* recording_state = NULL;
/* Recursive: capture takes the lock and then calls helpers that take it too. */
static GRecMutex recording_mutex;

static void set_number(cJSON* object, const char* name, double value) {
    cJSON* item = cJSON_GetObjectItem(object, name);
    if (item) {
        cJSON_SetNumberValue(item, value);
    } else {
        cJSON_AddNumberToObject(object, name, value);
    }
}

static cJSON* load_state(void) {
    char path[1024];
    if (!storage_recordings_path(path, sizeof(path))) {
        LOG_WARN("%s: Failed to build recordings path\n", __func__);
        return cJSON_CreateObject();
    }

    FILE* file = fopen(path, "r");
    if (!file) {
        return cJSON_CreateObject();
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* json = malloc(size + 1);
    if (!json) {
        fclose(file);
        return cJSON_CreateObject();
    }

    fread(json, 1, size, file);
    json[size] = '\0';
    fclose(file);

    cJSON* state = cJSON_Parse(json);
    free(json);
    return state ? state : cJSON_CreateObject();
}

static int save_state(void) {
    char error[256];
    char path[1024];
    if (!storage_ensure_root(error, sizeof(error))) {
        LOG_WARN("%s: %s\n", __func__, error);
        return 0;
    }

    if (!storage_recordings_path(path, sizeof(path))) {
        LOG_WARN("%s: Failed to build recordings path\n", __func__);
        return 0;
    }

    char* json = cJSON_PrintUnformatted(recording_state);
    if (!json) {
        return 0;
    }

    FILE* file = fopen(path, "w");
    if (!file) {
        free(json);
        return 0;
    }

    size_t written = fwrite(json, strlen(json), 1, file);
    fclose(file);
    free(json);
    return written == 1;
}

static int ensure_profile_frame_dirs(const char* profile_id) {
    char error[256];
    char profiles_dir[1024];
    char profile_dir[1024];
    char frames_dir[1024];

    if (!storage_profiles_dir(profiles_dir, sizeof(profiles_dir)) ||
        !storage_profile_dir(profile_dir, sizeof(profile_dir), profile_id) ||
        !storage_frames_dir(frames_dir, sizeof(frames_dir), profile_id)) {
        LOG_WARN("%s: Failed to build frame directories for %s\n", __func__, profile_id);
        return 0;
    }

    if (!storage_ensure_directory(profiles_dir, error, sizeof(error)) ||
        !storage_ensure_directory(profile_dir, error, sizeof(error)) ||
        !storage_ensure_directory(frames_dir, error, sizeof(error))) {
        LOG_WARN("%s: %s\n", __func__, error);
        return 0;
    }

    return 1;
}

int recording_store_init(void) {
    g_rec_mutex_lock(&recording_mutex);
    if (recording_state) {
        cJSON_Delete(recording_state);
    }
    recording_state = load_state();
    int ok = recording_state != NULL;
    g_rec_mutex_unlock(&recording_mutex);
    return ok;
}

/* Caller must hold recording_mutex. */
static cJSON* recording_state_locked(void) {
    if (!recording_state) {
        recording_state = load_state();
    }
    return recording_state;
}

cJSON* recording_store_snapshot(void) {
    g_rec_mutex_lock(&recording_mutex);
    cJSON* copy = cJSON_Duplicate(recording_state_locked(), 1);
    g_rec_mutex_unlock(&recording_mutex);
    return copy ? copy : cJSON_CreateObject();
}

cJSON* recording_store_get_copy(const char* profile_id) {
    if (!profile_id) {
        return NULL;
    }
    g_rec_mutex_lock(&recording_mutex);
    cJSON* recording = cJSON_GetObjectItem(recording_state_locked(), profile_id);
    cJSON* copy = recording ? cJSON_Duplicate(recording, 1) : NULL;
    g_rec_mutex_unlock(&recording_mutex);
    return copy;
}

int recording_store_capture_profile(cJSON* profile) {
    if (!profile) {
        return 0;
    }

    const char* profile_id = cJSON_GetStringValue(cJSON_GetObjectItem(profile, "id"));
    if (!profile_id) {
        LOG_WARN("%s: Profile is missing id\n", __func__);
        return 0;
    }

    g_rec_mutex_lock(&recording_mutex);

    if (!ensure_profile_frame_dirs(profile_id)) {
        g_rec_mutex_unlock(&recording_mutex);
        return 0;
    }

    JpegFrame frame;
    if (!capture_snapshot(profile, &frame)) {
        g_rec_mutex_unlock(&recording_mutex);
        return 0;
    }

    cJSON* state = recording_state_locked();
    cJSON* recording = cJSON_GetObjectItem(state, profile_id);
    if (!recording) {
        recording = cJSON_CreateObject();
        cJSON_AddItemToObject(state, profile_id, recording);
        set_number(recording, "first", frame.timestamp_ms);
        set_number(recording, "frames", 0);
        set_number(recording, "images", 0);
        set_number(recording, "sizeBytes", 0);
        set_number(recording, "size", 0);
        set_number(recording, "nextFrame", 1);
    }

    unsigned next_frame = cJSON_GetObjectItem(recording, "nextFrame") ?
        (unsigned)cJSON_GetObjectItem(recording, "nextFrame")->valueint :
        (unsigned)(cJSON_GetObjectItem(recording, "frames") ? cJSON_GetObjectItem(recording, "frames")->valueint + 1 : 1);

    char frame_path[1024];
    if (!storage_frame_path(frame_path, sizeof(frame_path), profile_id, next_frame)) {
        LOG_WARN("%s: Failed to build frame path for %s\n", __func__, profile_id);
        capture_frame_free(&frame);
        g_rec_mutex_unlock(&recording_mutex);
        return 0;
    }

    FILE* file = fopen(frame_path, "wb");
    if (!file) {
        LOG_WARN("%s: Failed to open frame path %s\n", __func__, frame_path);
        capture_frame_free(&frame);
        g_rec_mutex_unlock(&recording_mutex);
        return 0;
    }

    size_t written = fwrite(frame.data, 1, frame.size, file);
    fclose(file);
    if (written != frame.size) {
        LOG_WARN("%s: Failed to write complete frame %s\n", __func__, frame_path);
        capture_frame_free(&frame);
        g_rec_mutex_unlock(&recording_mutex);
        return 0;
    }

    int frames = cJSON_GetObjectItem(recording, "frames") ? cJSON_GetObjectItem(recording, "frames")->valueint : 0;
    double size_bytes = cJSON_GetObjectItem(recording, "sizeBytes") ? cJSON_GetObjectItem(recording, "sizeBytes")->valuedouble : 0;
    int fps = cJSON_GetObjectItem(profile, "fps") ? cJSON_GetObjectItem(profile, "fps")->valueint : 10;

    frames++;
    size_bytes += frame.size;
    set_number(recording, "frames", frames);
    set_number(recording, "images", frames);
    set_number(recording, "sizeBytes", size_bytes);
    set_number(recording, "size", size_bytes);
    set_number(recording, "last", frame.timestamp_ms);
    set_number(recording, "fps", fps);
    set_number(recording, "width", frame.width);
    set_number(recording, "height", frame.height);
    set_number(recording, "nextFrame", next_frame + 1);

    capture_frame_free(&frame);
    int saved = save_state();
    g_rec_mutex_unlock(&recording_mutex);
    return saved;
}

int recording_store_clear(const char* profile_id) {
    if (!profile_id) {
        return 0;
    }

    g_rec_mutex_lock(&recording_mutex);

    char profile_dir[1024];
    if (storage_profile_dir(profile_dir, sizeof(profile_dir), profile_id)) {
        if (!storage_remove_tree(profile_dir)) {
            LOG_WARN("%s: Failed to purge profile directory %s\n", __func__, profile_dir);
            g_rec_mutex_unlock(&recording_mutex);
            return 0;
        }
    }

    cJSON_DeleteItemFromObject(recording_state_locked(), profile_id);
    int saved = save_state();
    g_rec_mutex_unlock(&recording_mutex);
    return saved;
}

int recording_store_clear_if_unchanged(const char* profile_id, int expected_frames, double expected_last) {
    if (!profile_id) {
        return -1;
    }

    g_rec_mutex_lock(&recording_mutex);
    cJSON* recording = cJSON_GetObjectItem(recording_state_locked(), profile_id);
    cJSON* frames = recording ? cJSON_GetObjectItem(recording, "frames") : NULL;
    cJSON* last = recording ? cJSON_GetObjectItem(recording, "last") : NULL;
    if (!frames || !last || frames->valueint != expected_frames || last->valuedouble != expected_last) {
        g_rec_mutex_unlock(&recording_mutex);
        return 0;
    }

    char profile_dir[1024];
    if (storage_profile_dir(profile_dir, sizeof(profile_dir), profile_id) &&
        !storage_remove_tree(profile_dir)) {
        LOG_WARN("%s: Failed to purge profile directory %s\n", __func__, profile_dir);
        g_rec_mutex_unlock(&recording_mutex);
        return -1;
    }

    cJSON_DeleteItemFromObject(recording_state_locked(), profile_id);
    int saved = save_state();
    g_rec_mutex_unlock(&recording_mutex);
    return saved ? 1 : -1;
}

int recording_store_reset(void) {
    g_rec_mutex_lock(&recording_mutex);
    if (recording_state) {
        cJSON_Delete(recording_state);
    }
    recording_state = cJSON_CreateObject();
    int saved = save_state();
    g_rec_mutex_unlock(&recording_mutex);
    return saved;
}