#include "ACAP.h"
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>

#include "media.h"
#include "recording_store.h"
#include "storage.h"

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }

static char last_error[512];
static GMutex encode_mutex;
static char ffmpeg_path[512];
static int ffmpeg_path_resolved = 0;
static int ffmpeg_path_logged = 0;

#define REENCODE_EST_MBPS 2.5
// Batches are only flushed into the permanent export cache once genuinely quiet (no new
// capture for this long) OR once INCREMENTAL_MIN_PENDING_FRAMES have piled up - whichever
// comes first. Sized to land close to EXPORT_GOP_FRAMES so a batch boundary roughly coincides
// with where a periodic keyframe would land anyway, instead of forcing extra ones.
#define INCREMENTAL_QUIET_WINDOW_MS (2LL * 60LL * 60LL * 1000LL)
#define INCREMENTAL_MIN_PENDING_FRAMES 500

// GOP length in frames, fixed regardless of playback fps or capture interval. Sizing this
// off fps (fps * N seconds) made keyframe count vary with the export fps setting alone, even
// for the exact same source images (100-frame GOP at 10fps but 600-frame GOP at 60fps for the
// same clip collapses to a single keyframe). A frame-count is the one unit that's meaningful
// regardless of how far apart captures were in real time (1/day vs every 10s) or what fps the
// export is played back at (10fps review vs 30-60fps "nice timelapse").
#define EXPORT_GOP_FRAMES 100

static long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((long long)ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

typedef enum {
    MEDIA_PREVIEW,
    MEDIA_EXPORT,
    MEDIA_ARCHIVE
} MediaKind;

typedef struct {
    int frames;
    int fps;
    int width;
    int height;
    long long last;
    long long size_bytes;
} RecordingSnapshot;

static void set_last_error(const char* fmt, const char* detail) {
    snprintf(last_error, sizeof(last_error), fmt, detail ? detail : "");
}

static const char* resolve_ffmpeg_path(void) {
    if (ffmpeg_path_resolved) {
        return ffmpeg_path;
    }

    ffmpeg_path[0] = '\0';

    const char* env_path = getenv("TIMELAPSE_FFMPEG_PATH");
    if (env_path && env_path[0] && access(env_path, X_OK) == 0) {
        snprintf(ffmpeg_path, sizeof(ffmpeg_path), "%s", env_path);
    } else if (access(PACKAGED_FFMPEG_BIN, X_OK) == 0) {
        snprintf(ffmpeg_path, sizeof(ffmpeg_path), "%s", PACKAGED_FFMPEG_BIN);
    } else if (access(PACKAGED_FFMPEG_LIB, X_OK) == 0) {
        snprintf(ffmpeg_path, sizeof(ffmpeg_path), "%s", PACKAGED_FFMPEG_LIB);
    } else {
        ffmpeg_path[0] = '\0';
    }

    ffmpeg_path_resolved = 1;
    if (!ffmpeg_path_logged) {
        LOG("%s: using ffmpeg path=%s\n", __func__, ffmpeg_path);
        ffmpeg_path_logged = 1;
    }
    return ffmpeg_path;
}

static int clamp_fps(int fps) {
    if (fps < 1) return 1;
    if (fps > 60) return 60;
    return fps;
}

// Rolling average of real, observed encode+merge throughput (ms per frame), used to estimate
// "estimatedSeconds" for the mediaJob progress bar. This device's actual throughput varies a lot
// with what else is running (analytics, streaming, other ACAPs) - a fixed formula based on
// playback fps was frequently wrong by 5-10x, which is what made the progress bar rocket to its
// cap and then sit there for the rest of the job. Seeded with a conservative guess; every real
// job updates it, so the estimate self-corrects to this specific device's real-world behavior.
static double avg_encode_ms_per_frame = 150.0;

static void record_encode_throughput(int frame_count, long long elapsed_ms) {
    if (frame_count < 1 || elapsed_ms < 1) {
        return;
    }
    double sample_ms_per_frame = (double)elapsed_ms / (double)frame_count;
    // Exponential moving average: recent runs matter more than history, but one unusually
    // fast/slow job shouldn't swing the next estimate wildly either.
    avg_encode_ms_per_frame = (avg_encode_ms_per_frame * 0.7) + (sample_ms_per_frame * 0.3);
}

static double estimate_encode_seconds(int frame_count) {
    if (frame_count < 1) {
        return 2.0;
    }
    double estimate = (avg_encode_ms_per_frame * (double)frame_count) / 1000.0;
    return estimate < 1.0 ? 1.0 : estimate;
}

// Decides whether a batch smaller than INCREMENTAL_MIN_PENDING_FRAMES is still worth flushing
// now. Answer: only once capture has gone quiet for a while - if it's still actively capturing,
// waiting produces a better (larger, more GOP-aligned) batch; once quiet, there's nothing to be
// gained by waiting longer, so flush whatever's pending rather than leaving the export stale.
static int should_process_incremental_update(long long last_frame_ms, int pending_frames) {
    if (pending_frames < 1) {
        return 0;
    }

    if (pending_frames >= INCREMENTAL_MIN_PENDING_FRAMES) {
        return 1;
    }

    if (last_frame_ms <= 0) {
        return 1;
    }

    long long now_ms = (long long)time(NULL) * 1000LL;
    long long age_ms = now_ms - last_frame_ms;
    return age_ms >= INCREMENTAL_QUIET_WINDOW_MS;
}

static void set_reencode_status_active(const char* profile_id, int old_fps, int new_fps, long long input_bytes) {
    long long now = time(NULL);
    double estimate_seconds = 5.0;
    if (input_bytes > 0) {
        estimate_seconds = (double)input_bytes / (REENCODE_EST_MBPS * 1024.0 * 1024.0);
        if (estimate_seconds < 5.0) {
            estimate_seconds = 5.0;
        }
    }

    ACAP_STATUS_SetBool("mediaJob", "active", 1);
    ACAP_STATUS_SetString("mediaJob", "kind", "reencode_export");
    ACAP_STATUS_SetString("mediaJob", "profileId", profile_id ? profile_id : "");
    ACAP_STATUS_SetString("mediaJob", "stage", "Re-encoding export for FPS change");
    ACAP_STATUS_SetNumber("mediaJob", "oldFps", old_fps);
    ACAP_STATUS_SetNumber("mediaJob", "newFps", new_fps);
    ACAP_STATUS_SetNumber("mediaJob", "inputBytes", (double)input_bytes);
    ACAP_STATUS_SetNumber("mediaJob", "estimatedSeconds", estimate_seconds);
    ACAP_STATUS_SetNumber("mediaJob", "startedAt", (double)now);
    ACAP_STATUS_SetNumber("mediaJob", "progress", 1.0);
    ACAP_STATUS_SetString("mediaJob", "message", "Processing...");
}

static void set_reencode_status_done(int ok, const char* message) {
    ACAP_STATUS_SetBool("mediaJob", "active", 0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", ok ? 100.0 : 0.0);
    ACAP_STATUS_SetString("mediaJob", "message", message ? message : (ok ? "Completed" : "Failed"));
}

static void set_media_encode_status_active(const char* stage, const char* profile_id, int fps, int frame_count) {
    long long now = time(NULL);
    double estimate_seconds = estimate_encode_seconds(frame_count);

    ACAP_STATUS_SetBool("mediaJob", "active", 1);
    ACAP_STATUS_SetString("mediaJob", "kind", "media_encode");
    ACAP_STATUS_SetString("mediaJob", "profileId", profile_id ? profile_id : "");
    ACAP_STATUS_SetString("mediaJob", "stage", stage ? stage : "Preparing video");
    ACAP_STATUS_SetNumber("mediaJob", "fps", fps);
    ACAP_STATUS_SetNumber("mediaJob", "frameCount", frame_count);
    ACAP_STATUS_SetNumber("mediaJob", "estimatedSeconds", estimate_seconds);
    ACAP_STATUS_SetNumber("mediaJob", "startedAt", (double)now);
    ACAP_STATUS_SetNumber("mediaJob", "progress", 1.0);
    ACAP_STATUS_SetString("mediaJob", "message", "Processing...");
}

static void set_media_encode_status_done(int ok, const char* message) {
    ACAP_STATUS_SetBool("mediaJob", "active", 0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", ok ? 100.0 : 0.0);
    ACAP_STATUS_SetString("mediaJob", "message", message ? message : (ok ? "Video ready" : "Video failed"));
}

static const char* media_kind_name(MediaKind kind) {
    switch (kind) {
        case MEDIA_PREVIEW: return "preview";
        case MEDIA_EXPORT: return "export";
        case MEDIA_ARCHIVE: return "archive";
        default: return "unknown";
    }
}

static int file_exists_nonempty(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int build_metadata_path(char* out, size_t out_len, const char* output_path) {
    int written = snprintf(out, out_len, "%s.json", output_path);
    return written > 0 && (size_t)written < out_len;
}

static int build_incremental_state_path(char* out, size_t out_len, const char* output_path) {
    int written = snprintf(out, out_len, "%s.state.json", output_path);
    return written > 0 && (size_t)written < out_len;
}

static int has_prefix(const char* value, const char* prefix) {
    return value && prefix && strncmp(value, prefix, strlen(prefix)) == 0;
}

static int has_suffix(const char* value, const char* suffix) {
    if (!value || !suffix) {
        return 0;
    }
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len && strcmp(value + value_len - suffix_len, suffix) == 0;
}

static int is_v4l2m2m_encoder(const char* encoder_name) {
    return encoder_name && strstr(encoder_name, "_v4l2m2m") != NULL;
}

static cJSON* read_json_file(const char* path);

static int load_incremental_state(const char* path, int* finalized_frames, int* fps) {
    cJSON* root = read_json_file(path);
    if (!root) {
        return 0;
    }

    cJSON* finalized = cJSON_GetObjectItem(root, "finalizedFrames");
    cJSON* state_fps = cJSON_GetObjectItem(root, "fps");
    if (!finalized || !state_fps) {
        cJSON_Delete(root);
        return 0;
    }

    *finalized_frames = finalized->valueint;
    *fps = state_fps->valueint;
    cJSON_Delete(root);
    return 1;
}

static int save_incremental_state(const char* path, const char* profile_id, MediaKind kind, int fps, int finalized_frames) {
    char temp_path[1208];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
        return 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }

    cJSON_AddStringToObject(root, "profileId", profile_id);
    cJSON_AddStringToObject(root, "kind", media_kind_name(kind));
    cJSON_AddNumberToObject(root, "fps", fps);
    cJSON_AddNumberToObject(root, "finalizedFrames", finalized_frames);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return 0;
    }

    FILE* file = fopen(temp_path, "w");
    if (!file) {
        free(json);
        return 0;
    }

    size_t json_len = strlen(json);
    int ok = fwrite(json, 1, json_len, file) == json_len;
    fclose(file);
    free(json);

    if (!ok || rename(temp_path, path) == -1) {
        unlink(temp_path);
        return 0;
    }

    return 1;
}

static void delete_frame_range(const char* profile_id, int start_frame, int end_frame) {
    for (int frame = start_frame; frame <= end_frame; frame++) {
        char frame_path[1024];
        if (!storage_frame_path(frame_path, sizeof(frame_path), profile_id, (unsigned)frame)) {
            return;
        }
        unlink(frame_path);
    }
}

static const char* basename_ptr(const char* path) {
    const char* slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

static cJSON* read_json_file(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    fseek(file, 0, SEEK_SET);

    char* json = malloc((size_t)size + 1);
    if (!json) {
        fclose(file);
        return NULL;
    }

    size_t read_bytes = fread(json, 1, (size_t)size, file);
    fclose(file);
    json[read_bytes] = '\0';

    cJSON* root = cJSON_Parse(json);
    free(json);
    return root;
}

static int get_recording_snapshot(const char* profile_id, int fps, RecordingSnapshot* snapshot) {
    cJSON* recording = recording_store_get(profile_id);
    cJSON* frames = recording ? cJSON_GetObjectItem(recording, "frames") : NULL;
    if (!frames || frames->valueint < 1) {
        set_last_error("Recording has no frames for %s", profile_id);
        return 0;
    }

    cJSON* last = cJSON_GetObjectItem(recording, "last");
    cJSON* size = cJSON_GetObjectItem(recording, "sizeBytes") ?
        cJSON_GetObjectItem(recording, "sizeBytes") : cJSON_GetObjectItem(recording, "size");
    cJSON* width = cJSON_GetObjectItem(recording, "width");
    cJSON* height = cJSON_GetObjectItem(recording, "height");

    snapshot->frames = frames->valueint;
    snapshot->fps = fps;
    snapshot->width = width ? width->valueint : 0;
    snapshot->height = height ? height->valueint : 0;
    snapshot->last = last ? (long long)last->valuedouble : 0;
    snapshot->size_bytes = size ? (long long)size->valuedouble : 0;
    return 1;
}

static int detect_frame_bounds(const char* profile_id, int* first_frame, int* last_frame, int* file_count) {
    char frames_dir[1024];
    if (!storage_frames_dir(frames_dir, sizeof(frames_dir), profile_id)) {
        return 0;
    }

    DIR* dir = opendir(frames_dir);
    if (!dir) {
        return 0;
    }

    int min_idx = 0;
    int max_idx = 0;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        unsigned idx = 0;
        if (sscanf(entry->d_name, "%u.jpg", &idx) == 1 && idx > 0) {
            int iidx = (int)idx;
            if (count == 0 || iidx < min_idx) {
                min_idx = iidx;
            }
            if (count == 0 || iidx > max_idx) {
                max_idx = iidx;
            }
            count++;
        }
    }
    closedir(dir);

    if (count < 1) {
        return 0;
    }

    *first_frame = min_idx;
    *last_frame = max_idx;
    *file_count = count;
    return 1;
}

static int metadata_number_matches(cJSON* root, const char* name, long long expected) {
    cJSON* item = cJSON_GetObjectItem(root, name);
    return item && (long long)item->valuedouble == expected;
}

static int cache_is_valid(const char* profile_id, MediaKind kind, const char* output_path, const RecordingSnapshot* snapshot) {
    if (kind == MEDIA_ARCHIVE || !file_exists_nonempty(output_path)) {
        return 0;
    }

    char metadata_path[1200];
    if (!build_metadata_path(metadata_path, sizeof(metadata_path), output_path)) {
        return 0;
    }

    cJSON* root = read_json_file(metadata_path);
    if (!root) {
        return 0;
    }

    const char* cached_profile = cJSON_GetStringValue(cJSON_GetObjectItem(root, "profileId"));
    const char* cached_kind = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
    int valid = cached_profile && strcmp(cached_profile, profile_id) == 0 &&
        cached_kind && strcmp(cached_kind, media_kind_name(kind)) == 0 &&
        metadata_number_matches(root, "fps", snapshot->fps) &&
        metadata_number_matches(root, "frames", snapshot->frames) &&
        metadata_number_matches(root, "last", snapshot->last) &&
        metadata_number_matches(root, "sizeBytes", snapshot->size_bytes) &&
        metadata_number_matches(root, "width", snapshot->width) &&
        metadata_number_matches(root, "height", snapshot->height);

    cJSON_Delete(root);
    if (valid) {
        last_error[0] = '\0';
    }
    return valid;
}

static int write_cache_metadata(const char* profile_id, MediaKind kind, const char* output_path, const RecordingSnapshot* snapshot) {
    if (kind == MEDIA_ARCHIVE) {
        return 1;
    }

    char metadata_path[1200];
    char temp_path[1208];
    if (!build_metadata_path(metadata_path, sizeof(metadata_path), output_path)) {
        set_last_error("Cache metadata path is too long for %s", profile_id);
        return 0;
    }

    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", metadata_path);
    if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
        set_last_error("Cache metadata temp path is too long for %s", profile_id);
        return 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        set_last_error("Failed to create cache metadata for %s", profile_id);
        return 0;
    }

    cJSON_AddStringToObject(root, "profileId", profile_id);
    cJSON_AddStringToObject(root, "kind", media_kind_name(kind));
    cJSON_AddNumberToObject(root, "fps", snapshot->fps);
    cJSON_AddNumberToObject(root, "frames", snapshot->frames);
    cJSON_AddNumberToObject(root, "last", snapshot->last);
    cJSON_AddNumberToObject(root, "sizeBytes", snapshot->size_bytes);
    cJSON_AddNumberToObject(root, "width", snapshot->width);
    cJSON_AddNumberToObject(root, "height", snapshot->height);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        set_last_error("Failed to serialize cache metadata for %s", profile_id);
        return 0;
    }

    FILE* file = fopen(temp_path, "w");
    if (!file) {
        free(json);
        set_last_error("Failed to write cache metadata: %s", strerror(errno));
        return 0;
    }

    size_t json_len = strlen(json);
    int ok = fwrite(json, 1, json_len, file) == json_len;
    fclose(file);
    free(json);

    if (!ok || rename(temp_path, metadata_path) == -1) {
        unlink(temp_path);
        set_last_error("Failed to finalize cache metadata: %s", strerror(errno));
        return 0;
    }

    return 1;
}

static void prune_cache_variants(const char* profile_id, MediaKind kind, const char* output_path) {
    if (kind == MEDIA_ARCHIVE) {
        return;
    }

    char cache_dir[1024];
    char metadata_path[1200];
    if (!storage_cache_dir(cache_dir, sizeof(cache_dir), profile_id) ||
        !build_metadata_path(metadata_path, sizeof(metadata_path), output_path)) {
        return;
    }

    const char* prefix = kind == MEDIA_PREVIEW ? "preview_" : "export_";
    const char* current_mp4 = basename_ptr(output_path);
    const char* current_meta = basename_ptr(metadata_path);

    DIR* dir = opendir(cache_dir);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        int stale_temp = has_suffix(entry->d_name, ".tmp");
        int is_state_file = has_suffix(entry->d_name, ".state.json");
        int stale_variant = has_prefix(entry->d_name, prefix) &&
            strcmp(entry->d_name, current_mp4) != 0 &&
            strcmp(entry->d_name, current_meta) != 0;
        if (is_state_file || (!stale_temp && !stale_variant)) {
            continue;
        }

        char path[1200];
        if (storage_join(path, sizeof(path), cache_dir, entry->d_name)) {
            unlink(path);
        }
    }

    closedir(dir);
}

int media_ffmpeg_available(void) {
    const char* path = resolve_ffmpeg_path();
    if (path && path[0] && access(path, X_OK) == 0) {
        last_error[0] = '\0';
        return 1;
    }

    if (path && path[0]) {
        set_last_error("FFmpeg is not executable at %s", path);
    } else {
        set_last_error("Bundled FFmpeg missing: expected %s", PACKAGED_FFMPEG_BIN);
    }
    return 0;
}

const char* media_last_error(void) {
    return last_error[0] ? last_error : "Unknown media error";
}

static int ensure_cache_dir(const char* profile_id) {
    char error[256];
    char profiles_dir[1024];
    char profile_dir[1024];
    char cache_dir[1024];
    if (!storage_profiles_dir(profiles_dir, sizeof(profiles_dir)) ||
        !storage_profile_dir(profile_dir, sizeof(profile_dir), profile_id) ||
        !storage_cache_dir(cache_dir, sizeof(cache_dir), profile_id)) {
        set_last_error("Failed to build cache directory for %s", profile_id);
        return 0;
    }

    if (!storage_ensure_directory(profiles_dir, error, sizeof(error)) ||
        !storage_ensure_directory(profile_dir, error, sizeof(error)) ||
        !storage_ensure_directory(cache_dir, error, sizeof(error))) {
        set_last_error("%s", error);
        return 0;
    }

    return 1;
}

static int generate_mp4(const char* profile_id, int fps, const char* output_path, MediaKind kind, int force) {
    long long started_ms = monotonic_ms();
    RecordingSnapshot snapshot;
    if (!media_ffmpeg_available() || !get_recording_snapshot(profile_id, fps, &snapshot) || !ensure_cache_dir(profile_id)) {
        LOG_WARN("%s: precheck failed kind=%s profile=%s err=%s\n", __func__, media_kind_name(kind), profile_id ? profile_id : "(null)", media_last_error());
        return 0;
    }

    LOG("%s: start kind=%s profile=%s fps=%d frames=%d size_bytes=%lld\n",
        __func__, media_kind_name(kind), profile_id, snapshot.fps, snapshot.frames, snapshot.size_bytes);

    if (cache_is_valid(profile_id, kind, output_path, &snapshot)) {
        LOG("%s: cache hit kind=%s profile=%s path=%s elapsed_ms=%lld\n",
            __func__, media_kind_name(kind), profile_id, output_path, monotonic_ms() - started_ms);
        return 1;
    }

    if (!g_mutex_trylock(&encode_mutex)) {
        set_last_error("Media generation is already running for %s", profile_id);
        LOG_WARN("%s: encode lock busy kind=%s profile=%s\n", __func__, media_kind_name(kind), profile_id);
        return 0;
    }

    if (cache_is_valid(profile_id, kind, output_path, &snapshot)) {
        LOG("%s: cache hit after lock kind=%s profile=%s path=%s elapsed_ms=%lld\n",
            __func__, media_kind_name(kind), profile_id, output_path, monotonic_ms() - started_ms);
        g_mutex_unlock(&encode_mutex);
        return 1;
    }

    char frames_dir[1024];
    if (!storage_frames_dir(frames_dir, sizeof(frames_dir), profile_id)) {
        set_last_error("Failed to build frame directory for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    char input_pattern[1200];
    int written = snprintf(input_pattern, sizeof(input_pattern), "%s/%%08d.jpg", frames_dir);
    if (written <= 0 || (size_t)written >= sizeof(input_pattern)) {
        set_last_error("Frame input path is too long for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    char temp_path[1200];
    written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", output_path);
    if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
        set_last_error("Output path is too long for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    char fps_arg[16];
    snprintf(fps_arg, sizeof(fps_arg), "%d", clamp_fps(fps));

    // Populated below only if archiving needs to prepend an already-encoded export video
    // because the raw frames covering that range were purged by earlier export processing.
    char export_base_path[1200];
    export_base_path[0] = '\0';

    int start_number = 1;
    int frame_count = snapshot.frames;

    char state_path[1200];
    if (!build_incremental_state_path(state_path, sizeof(state_path), output_path)) {
        set_last_error("State path is too long for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    int finalized_frames = 0;
    int state_fps = 0;
    if (kind != MEDIA_ARCHIVE && load_incremental_state(state_path, &finalized_frames, &state_fps)) {
        if (state_fps != fps || finalized_frames < 0 || finalized_frames > snapshot.frames) {
            finalized_frames = 0;
            unlink(output_path);
        } else if (!file_exists_nonempty(output_path)) {
            finalized_frames = 0;
        }
    }

    if (kind != MEDIA_ARCHIVE) {
        if (snapshot.frames <= finalized_frames && file_exists_nonempty(output_path)) {
            if (!write_cache_metadata(profile_id, kind, output_path, &snapshot)) {
                g_mutex_unlock(&encode_mutex);
                return 0;
            }
            prune_cache_variants(profile_id, kind, output_path);
            last_error[0] = '\0';
            LOG("%s: incremental cache hit kind=%s profile=%s finalized=%d elapsed_ms=%lld\n",
                __func__, media_kind_name(kind), profile_id, finalized_frames, monotonic_ms() - started_ms);
            g_mutex_unlock(&encode_mutex);
            return 1;
        }

        start_number = finalized_frames + 1;
        frame_count = snapshot.frames - finalized_frames;
    }

    int media_job = 1;

    // Frame files can start at a higher index after incremental cleanup.
    char first_frame_path[1024];
    if (!storage_frame_path(first_frame_path, sizeof(first_frame_path), profile_id, (unsigned)start_number) ||
        access(first_frame_path, R_OK) != 0) {
        int first_available = 0;
        int last_available = 0;
        int frame_files = 0;
        if (detect_frame_bounds(profile_id, &first_available, &last_available, &frame_files)) {
            // If incremental state was lost and recording starts above 1 while export exists,
            // recover by appending available frames to the current export instead of replacing it.
            if (kind != MEDIA_ARCHIVE && start_number == 1 && first_available > 1 && file_exists_nonempty(output_path)) {
                finalized_frames = first_available - 1;
                LOG_WARN("%s: recovered incremental state kind=%s profile=%s finalized=%d\n",
                         __func__, media_kind_name(kind), profile_id, finalized_frames);
            }

            // Archiving reads raw frames directly. If earlier frames were already folded into
            // the export video and purged from disk, prepend that export video instead of
            // silently building an archive that is missing its earlier portion.
            if (kind == MEDIA_ARCHIVE && start_number == 1 && first_available > 1) {
                if (storage_export_path(export_base_path, sizeof(export_base_path), profile_id, fps) &&
                    file_exists_nonempty(export_base_path)) {
                    LOG_WARN("%s: archiving with export prefix kind=%s profile=%s prefix=%s purged_through=%d\n",
                             __func__, media_kind_name(kind), profile_id, export_base_path, first_available - 1);
                } else {
                    export_base_path[0] = '\0';
                }
            }

            // If old frame chunks are already purged and no base video exists to recover from,
            // only a tail clip can be built. Fail fast so playback/archives do not silently
            // start from the middle of the recording timeline.
            if (start_number == 1 && first_available > 1 &&
                !(kind != MEDIA_ARCHIVE && file_exists_nonempty(output_path)) &&
                !(kind == MEDIA_ARCHIVE && export_base_path[0])) {
                if (media_job) {
                    set_media_encode_status_done(0, "Full recording unavailable; base video is missing");
                }
                set_last_error("Full recording unavailable for %s", profile_id);
                LOG_WARN("%s: cannot rebuild full recording kind=%s profile=%s first_available=%d output=%s\n",
                         __func__, media_kind_name(kind), profile_id, first_available, output_path);
                g_mutex_unlock(&encode_mutex);
                return 0;
            }

            start_number = first_available;
            frame_count = last_available - first_available + 1;
            LOG_WARN("%s: adjusted frame window kind=%s profile=%s start=%d count=%d files=%d\n",
                     __func__, media_kind_name(kind), profile_id, start_number, frame_count, frame_files);
        } else {
            if (media_job) {
                set_media_encode_status_done(0, "No frame files available");
            }
            set_last_error("No frame files available for %s", profile_id);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
    }

    if (frame_count < 1) {
        if (media_job) {
            set_media_encode_status_done(0, "No frame files available");
        }
        set_last_error("No frame files available for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    if (kind != MEDIA_ARCHIVE && !force && finalized_frames > 0 && file_exists_nonempty(output_path) &&
        !should_process_incremental_update(snapshot.last, frame_count)) {
        last_error[0] = '\0';
        LOG("%s: deferred incremental update kind=%s profile=%s pending=%d last_ms=%lld elapsed_ms=%lld\n",
            __func__, media_kind_name(kind), profile_id, frame_count, snapshot.last, monotonic_ms() - started_ms);
        g_mutex_unlock(&encode_mutex);
        return 1;
    }

    if (media_job) {
        const char* stage = kind == MEDIA_PREVIEW ? "Preparing preview video" :
            (kind == MEDIA_ARCHIVE ? "Archiving recording" : "Updating recording video");
        set_media_encode_status_active(stage, profile_id, fps, frame_count);
    }

    char frame_count_arg[16];
    snprintf(frame_count_arg, sizeof(frame_count_arg), "%d", frame_count);

    char start_number_arg[16];
    snprintf(start_number_arg, sizeof(start_number_arg), "%d", start_number);

    const char* preset = kind == MEDIA_PREVIEW ? "ultrafast" : (kind == MEDIA_ARCHIVE ? "superfast" : "veryfast");
    const char* crf = kind == MEDIA_PREVIEW ? "30" : (kind == MEDIA_ARCHIVE ? "23" : "24");

    /* Fixed frame-count GOP (not fps-scaled); sc_threshold=0 stops libx264 inserting extra
       keyframes on scene cuts. Together these keep the keyframe cadence stable regardless of
       capture interval or playback fps, instead of short/irregular or fps-dependent GOPs. */
    char gop_arg[16];
    snprintf(gop_arg, sizeof(gop_arg), "%d", EXPORT_GOP_FRAMES);

    unlink(temp_path);
    const char* ffmpeg_exec = resolve_ffmpeg_path();

    const char* forced_encoder = getenv("TIMELAPSE_VIDEO_ENCODER");
    const char* encoder_candidates[4];
    int candidate_count = 0;
    if (forced_encoder && forced_encoder[0]) {
        encoder_candidates[candidate_count++] = forced_encoder;
    } else {
        if (kind != MEDIA_PREVIEW) {
            encoder_candidates[candidate_count++] = "h264_v4l2m2m";
        }
        encoder_candidates[candidate_count++] = "libx264";
    }

    int encode_ok = 0;
    char final_error[512];
    final_error[0] = '\0';

    for (int i = 0; i < candidate_count; i++) {
        const char* encoder = encoder_candidates[i];
        int use_hw = is_v4l2m2m_encoder(encoder);
        int use_faststart = kind != MEDIA_ARCHIVE;

        char* argv_sw_faststart[] = {
            (char*)ffmpeg_exec,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-framerate",
            fps_arg,
            "-start_number",
            start_number_arg,
            "-i",
            input_pattern,
            "-frames:v",
            frame_count_arg,
            "-c:v",
            (char*)encoder,
            "-preset",
            (char*)preset,
            "-crf",
            (char*)crf,
            "-g",
            gop_arg,
            "-keyint_min",
            gop_arg,
            "-sc_threshold",
            "0",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "+faststart",
            "-f",
            "mp4",
            temp_path,
            NULL
        };

        char* argv_sw_no_faststart[] = {
            (char*)ffmpeg_exec,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-framerate",
            fps_arg,
            "-start_number",
            start_number_arg,
            "-i",
            input_pattern,
            "-frames:v",
            frame_count_arg,
            "-c:v",
            (char*)encoder,
            "-preset",
            (char*)preset,
            "-crf",
            (char*)crf,
            "-g",
            gop_arg,
            "-keyint_min",
            gop_arg,
            "-sc_threshold",
            "0",
            "-pix_fmt",
            "yuv420p",
            "-f",
            "mp4",
            temp_path,
            NULL
        };

        char* argv_hw_faststart[] = {
            (char*)ffmpeg_exec,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-framerate",
            fps_arg,
            "-start_number",
            start_number_arg,
            "-i",
            input_pattern,
            "-frames:v",
            frame_count_arg,
            "-c:v",
            (char*)encoder,
            "-pix_fmt",
            "nv12",
            "-b:v",
            "4M",
            "-g",
            gop_arg,
            "-movflags",
            "+faststart",
            "-f",
            "mp4",
            temp_path,
            NULL
        };

        char* argv_hw_no_faststart[] = {
            (char*)ffmpeg_exec,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-framerate",
            fps_arg,
            "-start_number",
            start_number_arg,
            "-i",
            input_pattern,
            "-frames:v",
            frame_count_arg,
            "-c:v",
            (char*)encoder,
            "-pix_fmt",
            "nv12",
            "-b:v",
            "4M",
            "-g",
            gop_arg,
            "-f",
            "mp4",
            temp_path,
            NULL
        };

        char** argv = use_hw
            ? (use_faststart ? argv_hw_faststart : argv_hw_no_faststart)
            : (use_faststart ? argv_sw_faststart : argv_sw_no_faststart);

        unlink(temp_path);

        GError* error = NULL;
        gchar* stderr_text = NULL;
        gint wait_status = 0;
        long long ffmpeg_started_ms = monotonic_ms();
        LOG("%s: ffmpeg try kind=%s profile=%s encoder=%s input=%s output=%s fps=%s start=%s frames=%s preset=%s crf=%s gop=%s faststart=%s\n",
            __func__, media_kind_name(kind), profile_id, encoder, input_pattern, output_path,
            fps_arg, start_number_arg, frame_count_arg, preset, crf, gop_arg, use_faststart ? "on" : "off");

        gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                                   NULL, &stderr_text, &wait_status, &error);
        if (!ok) {
            snprintf(final_error, sizeof(final_error), "spawn failed: %s", error ? error->message : "unknown error");
            LOG_WARN("%s: ffmpeg spawn failed kind=%s profile=%s encoder=%s elapsed_ms=%lld err=%s\n",
                     __func__, media_kind_name(kind), profile_id, encoder,
                     monotonic_ms() - ffmpeg_started_ms, error ? error->message : "unknown error");
            if (error) {
                g_error_free(error);
            }
            g_free(stderr_text);
            continue;
        }

        if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
            snprintf(final_error, sizeof(final_error), "%s", stderr_text ? stderr_text : "unknown error");
            LOG_WARN("%s: ffmpeg failed kind=%s profile=%s encoder=%s wait_status=%d elapsed_ms=%lld err=%s\n",
                     __func__, media_kind_name(kind), profile_id, encoder, wait_status,
                     monotonic_ms() - ffmpeg_started_ms, stderr_text ? stderr_text : "unknown error");
            g_free(stderr_text);
            continue;
        }

        g_free(stderr_text);
        encode_ok = 1;
        LOG("%s: ffmpeg success kind=%s profile=%s encoder=%s elapsed_ms=%lld\n",
            __func__, media_kind_name(kind), profile_id, encoder, monotonic_ms() - ffmpeg_started_ms);
        break;
    }

    if (!encode_ok) {
        if (media_job) {
            set_media_encode_status_done(0, "Video encoding failed");
        }
        set_last_error("FFmpeg failed: %s", final_error[0] ? final_error : "all encoders failed");
        unlink(temp_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    // For export/preview, prepend the previously-finalized portion of the same output file.
    // For archive, prepend the export video only if raw frames were purged out from under it
    // (export_base_path was set above); otherwise archive always has the full raw sequence.
    const char* merge_base_path = NULL;
    if (kind != MEDIA_ARCHIVE) {
        if (finalized_frames > 0 && file_exists_nonempty(output_path)) {
            merge_base_path = output_path;
        }
    } else if (export_base_path[0] && file_exists_nonempty(export_base_path)) {
        merge_base_path = export_base_path;
    }

    if (merge_base_path) {
        char concat_list_path[1200];
        char concat_output_path[1200];
        int written_list = snprintf(concat_list_path, sizeof(concat_list_path), "%s.concat.txt", output_path);
        int written_concat = snprintf(concat_output_path, sizeof(concat_output_path), "%s.merge.tmp", output_path);
        if (written_list <= 0 || (size_t)written_list >= sizeof(concat_list_path) ||
            written_concat <= 0 || (size_t)written_concat >= sizeof(concat_output_path)) {
            if (media_job) {
                set_media_encode_status_done(0, "Video merge path failed");
            }
            set_last_error("Concat path is too long for %s", profile_id);
            unlink(temp_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }

        FILE* concat_list = fopen(concat_list_path, "w");
        if (!concat_list) {
            if (media_job) {
                set_media_encode_status_done(0, "Video merge list failed");
            }
            set_last_error("Failed to create concat list: %s", strerror(errno));
            unlink(temp_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
        fprintf(concat_list, "file '%s'\n", merge_base_path);
        fprintf(concat_list, "file '%s'\n", temp_path);
        fclose(concat_list);

        char* concat_argv[] = {
            (char*)ffmpeg_exec,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
            concat_list_path,
            "-c",
            "copy",
            "-movflags",
            "+faststart",
            "-f",
            "mp4",
            concat_output_path,
            NULL
        };

        GError* concat_error = NULL;
        gchar* concat_stderr = NULL;
        gint concat_wait_status = 0;
        gboolean concat_ok = g_spawn_sync(NULL, concat_argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                                          NULL, &concat_stderr, &concat_wait_status, &concat_error);
        unlink(concat_list_path);
        unlink(temp_path);

        if (!concat_ok || !WIFEXITED(concat_wait_status) || WEXITSTATUS(concat_wait_status) != 0) {
            if (media_job) {
                set_media_encode_status_done(0, "Video merge failed");
            }
            set_last_error("FFmpeg concat failed: %s", concat_stderr ? concat_stderr : (concat_error ? concat_error->message : "unknown error"));
            if (concat_error) {
                g_error_free(concat_error);
            }
            g_free(concat_stderr);
            unlink(concat_output_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }

        if (concat_error) {
            g_error_free(concat_error);
        }
        g_free(concat_stderr);

        if (rename(concat_output_path, output_path) == -1) {
            if (media_job) {
                set_media_encode_status_done(0, "Video finalize failed");
            }
            set_last_error("Failed to finalize merged MP4: %s", strerror(errno));
            unlink(concat_output_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
    } else if (rename(temp_path, output_path) == -1) {
        if (media_job) {
            set_media_encode_status_done(0, "Video finalize failed");
        }
        set_last_error("Failed to finalize MP4: %s", strerror(errno));
        LOG_WARN("%s: finalize failed kind=%s profile=%s output=%s err=%s\n",
                 __func__, media_kind_name(kind), profile_id, output_path, strerror(errno));
        unlink(temp_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    if (kind != MEDIA_ARCHIVE) {
        delete_frame_range(profile_id, start_number, start_number + frame_count - 1);
        if (!save_incremental_state(state_path, profile_id, kind, fps, snapshot.frames)) {
            if (media_job) {
                set_media_encode_status_done(0, "Video state update failed");
            }
            set_last_error("Failed to save incremental state for %s", profile_id);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
    }

    if (!write_cache_metadata(profile_id, kind, output_path, &snapshot)) {
        if (media_job) {
            set_media_encode_status_done(0, "Video metadata failed");
        }
        unlink(output_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }
    prune_cache_variants(profile_id, kind, output_path);

    last_error[0] = '\0';
    struct stat st;
    long long output_size = (stat(output_path, &st) == 0) ? (long long)st.st_size : -1;
    long long total_elapsed_ms = monotonic_ms() - started_ms;
    record_encode_throughput(frame_count, total_elapsed_ms);
    LOG("%s: success kind=%s profile=%s output=%s size=%lld elapsed_ms=%lld\n",
        __func__, media_kind_name(kind), profile_id, output_path, output_size, total_elapsed_ms);
    if (media_job) {
        set_media_encode_status_done(1, kind == MEDIA_PREVIEW ? "Preview ready" :
            (kind == MEDIA_ARCHIVE ? "Archive ready" : "Recording video updated"));
    }
    g_mutex_unlock(&encode_mutex);
    return 1;
}

// Builds a disposable "Play" preview: the existing export cache (stream-copied, free) plus a
// fast/cheap encode of whatever hasn't been folded into that cache yet. Unlike generate_mp4(),
// this NEVER purges raw frames, NEVER updates the export's incremental state, and NEVER writes
// cache metadata for its own output - it can be regenerated from scratch on every single call
// with zero lasting effect. That's deliberate: the permanent export cache's GOP quality must
// only ever be touched by the periodic sweep or an explicit Download (both use generate_mp4()'s
// proper -g/-sc_threshold-tuned encode), never by how many times someone clicks Play.
int media_generate_preview(const char* profile_id, int fps, char* out_path, size_t out_len, int allow_rebuild) {
    long long preview_started_ms = monotonic_ms();
    if (!media_ffmpeg_available()) {
        set_last_error("ffmpeg not available for %s", profile_id);
        return 0;
    }
    fps = clamp_fps(fps);

    RecordingSnapshot snapshot;
    if (!get_recording_snapshot(profile_id, fps, &snapshot)) {
        return 0;
    }

    char export_path[1024];
    if (!storage_export_path(export_path, sizeof(export_path), profile_id, fps)) {
        set_last_error("Failed to build export path for %s", profile_id);
        return 0;
    }

    int have_export = file_exists_nonempty(export_path);
    int encoded_frames = 0;
    if (have_export) {
        char metadata_path[1200];
        if (build_metadata_path(metadata_path, sizeof(metadata_path), export_path)) {
            cJSON* root = read_json_file(metadata_path);
            if (root) {
                cJSON* frames_item = cJSON_GetObjectItem(root, "frames");
                if (frames_item) {
                    encoded_frames = frames_item->valueint;
                }
                cJSON_Delete(root);
            }
        }
    }

    char preview_path[1024];
    if (!storage_preview_path(preview_path, sizeof(preview_path), profile_id, fps)) {
        set_last_error("Failed to build preview path for %s", profile_id);
        return 0;
    }

    // Nothing pending: the export cache already covers everything captured so far, so it's
    // both correct and free to just point at it directly instead of building a separate file.
    if (have_export && snapshot.frames <= encoded_frames) {
        if (snprintf(out_path, out_len, "%s", export_path) <= 0 || strlen(export_path) >= out_len) {
            set_last_error("Failed to return preview path for %s", profile_id);
            return 0;
        }
        return 1;
    }

    // How much does a previously-built preview already cover? Read this regardless of whether
    // it's fully current - it's also used below to decide the merge base, not just the
    // exact-match fast path.
    char preview_meta_path[1200];
    int preview_meta_written = snprintf(preview_meta_path, sizeof(preview_meta_path), "%s.frames", preview_path);
    int have_preview_meta = preview_meta_written > 0 && (size_t)preview_meta_written < sizeof(preview_meta_path);
    int have_preview_file = have_preview_meta && file_exists_nonempty(preview_path);
    int preview_frames = 0;
    if (have_preview_file) {
        FILE* meta_file = fopen(preview_meta_path, "r");
        if (meta_file) {
            char preview_frames_text[32];
            if (fgets(preview_frames_text, sizeof(preview_frames_text), meta_file)) {
                preview_frames = atoi(preview_frames_text);
            }
            fclose(meta_file);
        }
    }

    // Already fully current - reuse it instead of redoing the tail encode. This is what makes
    // a "prepare, then fetch" two-step Play flow (or repeated Play clicks with nothing new
    // captured) cheap.
    if (have_preview_file && snapshot.frames <= preview_frames) {
        if (snprintf(out_path, out_len, "%s", preview_path) <= 0 || strlen(preview_path) >= out_len) {
            set_last_error("Failed to return preview path for %s", profile_id);
            return 0;
        }
        return 1;
    }

    // Build on whichever of the export cache or the last-built preview already covers more.
    // This is what keeps repeated rebuilds cheap on an actively-capturing profile: each one
    // only needs to encode+merge the delta since the LAST rebuild (of either kind), instead of
    // re-copying the entire, ever-growing export cache from scratch every single time - which
    // is what previously made this take longer with every retry, on a profile that captures
    // faster than the rebuild + copy could complete.
    const char* base_path = NULL;
    int base_frames = 0;
    if (have_export && encoded_frames >= preview_frames) {
        base_path = export_path;
        base_frames = encoded_frames;
    } else if (have_preview_file) {
        base_path = preview_path;
        base_frames = preview_frames;
    }

    // Caller only wants whatever's already cached (typically a GET right after a PUT already
    // built/refreshed it) - report "not ready" rather than doing new work here. This is what
    // makes a fetch-only call immune to frames landing between the PUT finishing and this call:
    // it either serves what's already there or says so, it never chases a newer count itself.
    if (!allow_rebuild) {
        set_last_error("Preview not ready for %s", profile_id);
        return 0;
    }

    if (!g_mutex_trylock(&encode_mutex)) {
        set_last_error("Media generation is already running for %s", profile_id);
        return 0;
    }

    char frames_dir[1024];
    if (!storage_frames_dir(frames_dir, sizeof(frames_dir), profile_id)) {
        set_last_error("Failed to build frame directory for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    char input_pattern[1200];
    int written = snprintf(input_pattern, sizeof(input_pattern), "%s/%%08d.jpg", frames_dir);
    if (written <= 0 || (size_t)written >= sizeof(input_pattern)) {
        set_last_error("Frame input path is too long for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    int start_number = base_frames + 1;
    int frame_count = snapshot.frames - base_frames;

    // Frame files can start at a higher index than expected if older chunks were already
    // purged by a real export/archive pass; fall back to whatever's actually still on disk.
    char first_frame_path[1024];
    if (!storage_frame_path(first_frame_path, sizeof(first_frame_path), profile_id, (unsigned)start_number) ||
        access(first_frame_path, R_OK) != 0) {
        int first_available = 0, last_available = 0, frame_files = 0;
        if (!detect_frame_bounds(profile_id, &first_available, &last_available, &frame_files)) {
            set_last_error("No frame files available for %s", profile_id);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
        start_number = first_available;
        frame_count = last_available - first_available + 1;
    }

    if (frame_count < 1) {
        set_last_error("No frame files available for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    char fps_arg[16];
    snprintf(fps_arg, sizeof(fps_arg), "%d", fps);
    char start_number_arg[16];
    snprintf(start_number_arg, sizeof(start_number_arg), "%d", start_number);
    char frame_count_arg[16];
    snprintf(frame_count_arg, sizeof(frame_count_arg), "%d", frame_count);

    char tail_path[1200];
    written = snprintf(tail_path, sizeof(tail_path), "%s.tail.tmp", preview_path);
    if (written <= 0 || (size_t)written >= sizeof(tail_path)) {
        set_last_error("Preview temp path is too long for %s", profile_id);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }
    unlink(tail_path);

    const char* ffmpeg_exec = resolve_ffmpeg_path();
    char* tail_argv[] = {
        (char*)ffmpeg_exec,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-framerate",
        fps_arg,
        "-start_number",
        start_number_arg,
        "-i",
        input_pattern,
        "-frames:v",
        frame_count_arg,
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-crf",
        "30",
        "-pix_fmt",
        "yuv420p",
        "-f",
        "mp4",
        tail_path,
        NULL
    };

    GError* error = NULL;
    gchar* stderr_text = NULL;
    gint wait_status = 0;
    long long tail_started_ms = monotonic_ms();
    LOG("%s: tail encode profile=%s start=%s frames=%s\n", __func__, profile_id, start_number_arg, frame_count_arg);
    gboolean ok = g_spawn_sync(NULL, tail_argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                               NULL, &stderr_text, &wait_status, &error);
    if (!ok || !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        set_last_error("Preview encode failed: %s", stderr_text ? stderr_text : (error ? error->message : "unknown error"));
        LOG_WARN("%s: tail encode failed profile=%s err=%s\n", __func__, profile_id, last_error);
        if (error) {
            g_error_free(error);
        }
        g_free(stderr_text);
        unlink(tail_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }
    if (error) {
        g_error_free(error);
    }
    g_free(stderr_text);
    LOG("%s: tail encode done profile=%s elapsed_ms=%lld\n", __func__, profile_id, monotonic_ms() - tail_started_ms);

    if (!base_path) {
        // No base to build on yet (first-ever preview for this profile) - the tail is the
        // whole preview.
        unlink(preview_path);
        if (rename(tail_path, preview_path) == -1) {
            set_last_error("Failed to finalize preview: %s", strerror(errno));
            unlink(tail_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
    } else {
        // Prepend the base (stream copy, cheap) ahead of the fresh tail. This writes to a
        // separate temp file rather than preview_path directly, because base_path may *be*
        // preview_path (see the base_path selection above) - ffmpeg must finish reading it
        // before anything touches the real preview_path.
        char concat_list_path[1200];
        written = snprintf(concat_list_path, sizeof(concat_list_path), "%s.concat.txt", preview_path);
        if (written <= 0 || (size_t)written >= sizeof(concat_list_path)) {
            set_last_error("Preview concat path is too long for %s", profile_id);
            unlink(tail_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }

        char concat_output_path[1200];
        written = snprintf(concat_output_path, sizeof(concat_output_path), "%s.merge.tmp", preview_path);
        if (written <= 0 || (size_t)written >= sizeof(concat_output_path)) {
            set_last_error("Preview merge path is too long for %s", profile_id);
            unlink(tail_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }

        FILE* concat_list = fopen(concat_list_path, "w");
        if (!concat_list) {
            set_last_error("Failed to create preview concat list: %s", strerror(errno));
            unlink(tail_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
        fprintf(concat_list, "file '%s'\n", base_path);
        fprintf(concat_list, "file '%s'\n", tail_path);
        fclose(concat_list);

        unlink(concat_output_path);
        char* concat_argv[] = {
            (char*)ffmpeg_exec,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
            concat_list_path,
            "-c",
            "copy",
            "-movflags",
            "+faststart",
            "-f",
            "mp4",
            concat_output_path,
            NULL
        };

        GError* concat_error = NULL;
        gchar* concat_stderr = NULL;
        gint concat_wait_status = 0;
        gboolean concat_ok = g_spawn_sync(NULL, concat_argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                                          NULL, &concat_stderr, &concat_wait_status, &concat_error);
        unlink(concat_list_path);
        unlink(tail_path);

        if (!concat_ok || !WIFEXITED(concat_wait_status) || WEXITSTATUS(concat_wait_status) != 0) {
            set_last_error("Preview merge failed: %s", concat_stderr ? concat_stderr : (concat_error ? concat_error->message : "unknown error"));
            if (concat_error) {
                g_error_free(concat_error);
            }
            g_free(concat_stderr);
            unlink(concat_output_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
        if (concat_error) {
            g_error_free(concat_error);
        }
        g_free(concat_stderr);

        // Only now safe to replace the real preview_path - ffmpeg has finished reading
        // base_path, even in the case where that was the old preview_path itself.
        if (rename(concat_output_path, preview_path) == -1) {
            set_last_error("Failed to finalize merged preview: %s", strerror(errno));
            unlink(concat_output_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
    }

    g_mutex_unlock(&encode_mutex);

    // Record what this preview now covers so the next call (or a poll-driven "prepare" step
    // right before this one) can skip redoing the work if nothing new has been captured since.
    if (have_preview_meta) {
        FILE* meta_file = fopen(preview_meta_path, "w");
        if (meta_file) {
            fprintf(meta_file, "%d", snapshot.frames);
            fclose(meta_file);
        }
    }

    record_encode_throughput(frame_count, monotonic_ms() - preview_started_ms);

    if (snprintf(out_path, out_len, "%s", preview_path) <= 0 || strlen(preview_path) >= out_len) {
        set_last_error("Failed to return preview path for %s", profile_id);
        return 0;
    }
    return 1;
}

int media_generate_export(const char* profile_id, int fps, char* out_path, size_t out_len) {
    fps = clamp_fps(fps);
    if (!storage_export_path(out_path, out_len, profile_id, fps)) {
        set_last_error("Failed to build export path for %s", profile_id);
        return 0;
    }
    return generate_mp4(profile_id, fps, out_path, MEDIA_EXPORT, 0);
}

int media_generate_archive(const char* profile_id, int fps, const char* output_path) {
    if (!output_path) {
        set_last_error("Invalid archive output path for %s", profile_id);
        return 0;
    }
    return generate_mp4(profile_id, clamp_fps(fps), output_path, MEDIA_ARCHIVE, 0);
}

// Estimates the final export MP4 size from the currently-encoded video's own bytes-per-frame,
// extrapolated over frames captured since it was last processed. Returns -1 if no export video
// exists yet to sample a compression ratio from (raw JPEG size is the only signal available then).
long long media_estimate_export_size(const char* profile_id, int fps, int current_total_frames) {
    if (!profile_id || current_total_frames < 1) {
        return -1;
    }
    fps = clamp_fps(fps);

    char export_path[1024];
    if (!storage_export_path(export_path, sizeof(export_path), profile_id, fps)) {
        return -1;
    }

    struct stat st;
    if (stat(export_path, &st) != 0 || st.st_size <= 0) {
        return -1;
    }

    char metadata_path[1200];
    int encoded_frames = 0;
    if (build_metadata_path(metadata_path, sizeof(metadata_path), export_path)) {
        cJSON* root = read_json_file(metadata_path);
        if (root) {
            cJSON* frames_item = cJSON_GetObjectItem(root, "frames");
            if (frames_item) {
                encoded_frames = frames_item->valueint;
            }
            cJSON_Delete(root);
        }
    }

    if (encoded_frames <= 0 || current_total_frames <= encoded_frames) {
        return (long long)st.st_size;
    }

    double bytes_per_frame = (double)st.st_size / (double)encoded_frames;
    int pending_frames = current_total_frames - encoded_frames;
    return (long long)st.st_size + (long long)(bytes_per_frame * (double)pending_frames);
}

static gint64 parse_hms_us(const char* value) {
    int hours = 0;
    int minutes = 0;
    double seconds = 0.0;
    if (!value || sscanf(value, "%d:%d:%lf", &hours, &minutes, &seconds) != 3) {
        return 0;
    }
    double total = ((double)hours * 3600.0) + ((double)minutes * 60.0) + seconds;
    return (gint64)(total * 1000000.0);
}

static gint64 probe_media_duration_us(const char* input_path) {
    if (!input_path) {
        return 0;
    }

    const char* ffmpeg = resolve_ffmpeg_path();
    char* argv[] = {
        (char*)ffmpeg,
        "-hide_banner",
        "-i",
        (char*)input_path,
        "-f",
        "null",
        "-",
        NULL
    };

    GError* spawn_error = NULL;
    gchar* stderr_text = NULL;
    gint wait_status = 0;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                              NULL, &stderr_text, &wait_status, &spawn_error);
    if (spawn_error) {
        g_error_free(spawn_error);
    }

    gint64 duration_us = 0;
    if (ok && stderr_text) {
        char* duration = strstr(stderr_text, "Duration:");
        if (duration) {
            duration += strlen("Duration:");
            while (*duration == ' ') {
                duration++;
            }
            duration_us = parse_hms_us(duration);
        }
    }

    g_free(stderr_text);
    return duration_us;
}

int media_convert_avi_to_mp4(const char* input_path, const char* output_path, Media_Progress_Callback progress_callback, void* user_data) {
    if (!input_path || !output_path) {
        set_last_error("Invalid migration path for %s", "AVI conversion");
        return 0;
    }

    if (!media_ffmpeg_available()) {
        return 0;
    }

    char temp_path[1024];
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", output_path) <= 0 || strlen(output_path) + 4 >= sizeof(temp_path)) {
        set_last_error("Migration output path too long: %s", output_path);
        return 0;
    }

    gint64 duration_us = probe_media_duration_us(input_path);
    unlink(temp_path);
    g_mutex_lock(&encode_mutex);

    const char* ffmpeg = resolve_ffmpeg_path();
    if (progress_callback) {
        if (!progress_callback(1.0, "Starting conversion", user_data)) {
            set_last_error("%s", "Migration cancelled.");
            unlink(temp_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
    }

    char* argv[] = {
        (char*)ffmpeg,
        "-y",
        "-nostats",
        "-progress",
        "pipe:1",
        "-i",
        (char*)input_path,
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        "23",
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        "-f",
        "mp4",
        temp_path,
        NULL
    };

    GError* spawn_error = NULL;
    gchar* stderr_text = NULL;
    gint stdout_fd = -1;
    gint stderr_fd = -1;
    GPid child_pid = 0;
    gboolean ok = g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                          &child_pid, NULL, &stdout_fd, &stderr_fd, &spawn_error);
    if (!ok) {
        set_last_error("FFmpeg AVI migration failed: %s", spawn_error ? spawn_error->message : "unknown error");
        if (spawn_error) {
            g_error_free(spawn_error);
        }
        unlink(temp_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    GString* stderr_buffer = g_string_new(NULL);
    FILE* stdout_stream = fdopen(stdout_fd, "r");
    FILE* stderr_stream = fdopen(stderr_fd, "r");
    char line[512];
    gboolean cancelled = FALSE;
    while (stdout_stream && fgets(line, sizeof(line), stdout_stream)) {
        char* newline = strchr(line, '\n');
        if (newline) {
            *newline = '\0';
        }

        if (g_str_has_prefix(line, "out_time_ms=")) {
            gint64 out_time_us = g_ascii_strtoll(line + strlen("out_time_ms="), NULL, 10);
            double percent = duration_us > 0 ? ((double)out_time_us * 100.0) / (double)duration_us : 0.0;
            if (percent < 1.0) percent = 1.0;
            if (percent > 99.0) percent = 99.0;
            if (progress_callback) {
                if (!progress_callback(percent, "Converting AVI to MP4", user_data)) {
                    cancelled = TRUE;
                    kill(child_pid, SIGTERM);
                    break;
                }
            }
        } else if (g_str_has_prefix(line, "out_time_us=")) {
            gint64 out_time_us = g_ascii_strtoll(line + strlen("out_time_us="), NULL, 10);
            double percent = duration_us > 0 ? ((double)out_time_us * 100.0) / (double)duration_us : 0.0;
            if (percent < 1.0) percent = 1.0;
            if (percent > 99.0) percent = 99.0;
            if (progress_callback) {
                if (!progress_callback(percent, "Converting AVI to MP4", user_data)) {
                    cancelled = TRUE;
                    kill(child_pid, SIGTERM);
                    break;
                }
            }
        }
    }

    if (stdout_stream) {
        fclose(stdout_stream);
    }

    if (stderr_stream) {
        while (fgets(line, sizeof(line), stderr_stream)) {
            g_string_append(stderr_buffer, line);
        }
        fclose(stderr_stream);
    }

    int wait_status = 0;
    waitpid(child_pid, &wait_status, 0);
    g_spawn_close_pid(child_pid);
    stderr_text = g_string_free(stderr_buffer, FALSE);

    if (cancelled) {
        set_last_error("%s", "Migration cancelled.");
        if (spawn_error) {
            g_error_free(spawn_error);
        }
        g_free(stderr_text);
        unlink(temp_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    if (!ok || !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        set_last_error("FFmpeg AVI migration failed: %s", stderr_text ? stderr_text : (spawn_error ? spawn_error->message : "unknown error"));
        if (spawn_error) {
            g_error_free(spawn_error);
        }
        g_free(stderr_text);
        unlink(temp_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    if (spawn_error) {
        g_error_free(spawn_error);
    }
    g_free(stderr_text);

    if (rename(temp_path, output_path) == -1) {
        set_last_error("Failed to finalize migrated MP4: %s", strerror(errno));
        unlink(temp_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

    if (progress_callback) {
        progress_callback(100.0, "Conversion complete", user_data);
    }

    g_mutex_unlock(&encode_mutex);
    return 1;
}

int media_import_avi_to_live_mp4(const char* input_path, const char* profile_id, int fps, int finalized_frames, Media_Progress_Callback progress_callback, void* user_data) {
    if (!input_path || !profile_id) {
        set_last_error("Invalid migration path for %s", "live AVI import");
        return 0;
    }

    char output_path[1024];
    char state_path[1200];
    fps = clamp_fps(fps);
    if (!ensure_cache_dir(profile_id) ||
        !storage_export_path(output_path, sizeof(output_path), profile_id, fps) ||
        !build_incremental_state_path(state_path, sizeof(state_path), output_path)) {
        set_last_error("Failed to build live import paths for %s", profile_id);
        return 0;
    }

    unlink(output_path);
    if (!media_convert_avi_to_mp4(input_path, output_path, progress_callback, user_data)) {
        return 0;
    }

    if (finalized_frames < 0) {
        finalized_frames = 0;
    }
    if (!save_incremental_state(state_path, profile_id, MEDIA_EXPORT, fps, finalized_frames)) {
        set_last_error("Failed to save live import state for %s", profile_id);
        unlink(output_path);
        return 0;
    }

    return 1;
}

int media_process_pending(const char* profile_id, int fps) {
    char output_path[1024];
    fps = clamp_fps(fps);
    if (!storage_export_path(output_path, sizeof(output_path), profile_id, fps)) {
        set_last_error("Failed to build export path for %s", profile_id);
        return 0;
    }
    return generate_mp4(profile_id, fps, output_path, MEDIA_EXPORT, 0);
}

// Bypasses the incremental-batch deferral so every currently captured frame is
// folded into the export video immediately, regardless of batch size/recency.
int media_process_pending_force(const char* profile_id, int fps) {
    char output_path[1024];
    fps = clamp_fps(fps);
    if (!storage_export_path(output_path, sizeof(output_path), profile_id, fps)) {
        set_last_error("Failed to build export path for %s", profile_id);
        return 0;
    }
    return generate_mp4(profile_id, fps, output_path, MEDIA_EXPORT, 1);
}

int media_reencode_export(const char* profile_id, int old_fps, int new_fps) {
    if (!profile_id) {
        set_last_error("Invalid profile id for %s", "reencode");
        return 0;
    }

    old_fps = clamp_fps(old_fps);
    new_fps = clamp_fps(new_fps);
    if (old_fps == new_fps) {
        return 1;
    }

    if (!media_ffmpeg_available()) {
        return 0;
    }

    char old_path[1024];
    char new_path[1024];
    if (!storage_export_path(old_path, sizeof(old_path), profile_id, old_fps) ||
        !storage_export_path(new_path, sizeof(new_path), profile_id, new_fps)) {
        set_last_error("Failed to build export paths for %s", profile_id);
        return 0;
    }

    struct stat old_st;
    long long old_size_bytes = (stat(old_path, &old_st) == 0) ? (long long)old_st.st_size : 0;
    if (!file_exists_nonempty(old_path)) {
        return 1;
    }

    set_reencode_status_active(profile_id, old_fps, new_fps, old_size_bytes);

    char new_temp[1200];
    int written = snprintf(new_temp, sizeof(new_temp), "%s.reencode.tmp", new_path);
    if (written <= 0 || (size_t)written >= sizeof(new_temp)) {
        set_last_error("Re-encode temp path too long for %s", profile_id);
        return 0;
    }

    char fps_arg[16];
    snprintf(fps_arg, sizeof(fps_arg), "%d", new_fps);
    char gop_arg[16];
    snprintf(gop_arg, sizeof(gop_arg), "%d", EXPORT_GOP_FRAMES);
    const char* ffmpeg_exec = resolve_ffmpeg_path();

    char* argv[] = {
        (char*)ffmpeg_exec,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        old_path,
        "-r",
        fps_arg,
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        "24",
        "-g",
        gop_arg,
        "-keyint_min",
        gop_arg,
        "-sc_threshold",
        "0",
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        "-f",
        "mp4",
        new_temp,
        NULL
    };

    GError* error = NULL;
    gchar* stderr_text = NULL;
    gint wait_status = 0;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                               NULL, &stderr_text, &wait_status, &error);

    if (!ok || !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        set_reencode_status_done(0, "Re-encode failed");
        set_last_error("Failed to re-encode export: %s",
                       stderr_text ? stderr_text : (error ? error->message : "unknown error"));
        if (error) {
            g_error_free(error);
        }
        g_free(stderr_text);
        unlink(new_temp);
        return 0;
    }

    if (error) {
        g_error_free(error);
    }
    g_free(stderr_text);

    if (rename(new_temp, new_path) == -1) {
        set_reencode_status_done(0, "Failed finalizing output");
        set_last_error("Failed to finalize re-encode: %s", strerror(errno));
        unlink(new_temp);
        return 0;
    }

    char old_state_path[1200];
    char new_state_path[1200];
    if (build_incremental_state_path(old_state_path, sizeof(old_state_path), old_path) &&
        build_incremental_state_path(new_state_path, sizeof(new_state_path), new_path)) {
        int finalized_frames = 0;
        int state_fps = 0;
        if (load_incremental_state(old_state_path, &finalized_frames, &state_fps)) {
            save_incremental_state(new_state_path, profile_id, MEDIA_EXPORT, new_fps, finalized_frames);
        }
        unlink(old_state_path);
    }

    char old_meta_path[1200];
    if (build_metadata_path(old_meta_path, sizeof(old_meta_path), old_path)) {
        unlink(old_meta_path);
    }
    unlink(old_path);
    set_reencode_status_done(1, "Re-encode completed");
    return 1;
}