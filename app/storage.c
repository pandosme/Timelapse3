#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "storage.h"

static char active_storage_root[1024];
static int active_storage_root_initialized = 0;

static int build_uid_storage_root(char* out, size_t out_len) {
    uid_t uid = geteuid();
    int written = snprintf(out, out_len, "%s/timelapse2-u%u", STORAGE_SD_BASE, (unsigned)uid);
    return written > 0 && (size_t)written < out_len;
}

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
    if (!active_storage_root_initialized) {
        snprintf(active_storage_root, sizeof(active_storage_root), "%s", STORAGE_ROOT);
    }
    return active_storage_root;
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

int storage_ensure_root(char* error, size_t error_len) {
    const char* env_root = getenv("STORAGE_ROOT");
    if (env_root && env_root[0]) {
        char env_error[256];
        if (ensure_root_candidate(env_root, env_error, sizeof(env_error))) {
            snprintf(active_storage_root, sizeof(active_storage_root), "%s", env_root);
            active_storage_root_initialized = 1;
            if (error && error_len > 0) {
                error[0] = '\0';
            }
            return 1;
        }
        if (error && error_len > 0) {
            snprintf(error, error_len, "STORAGE_ROOT override unusable: %s (%s)", env_root, env_error);
        }
        return 0;
    }

    char sd_areas_root[1024];
    char uid_root[1024];
    int sd_areas_ready = storage_join(sd_areas_root, sizeof(sd_areas_root), STORAGE_SD_AREAS_BASE, "timelapse2");
    int uid_root_ready = build_uid_storage_root(uid_root, sizeof(uid_root));

    // Deliberately no internal-flash fallback: recordings only ever belong on the SD card. If
    // neither SD card path is usable, storage_ensure_root() must fail so the app reports "no SD
    // card" instead of silently recording somewhere small, non-persistent, and wiped on reinstall.
    struct { const char* path; int ready; } candidates[] = {
        { sd_areas_root, sd_areas_ready },   // modern AXIS OS "storage areas" path - the real SD card
                                              // on devices where the legacy path below isn't usable
        { STORAGE_ROOT, 1 },                 // legacy path - the real SD card on older/other devices
        { uid_root, uid_root_ready },        // legacy path, uid-scoped (pre-existing edge case)
    };
    const int candidate_count = (int)(sizeof(candidates) / sizeof(candidates[0]));

    // First pass: if a "timelapse2" directory already exists in any candidate, keep using it.
    // A camera's data should never get stranded just because this priority order changed, or
    // because its firmware happens to mount the SD card at a different one of these paths.
    for (int i = 0; i < candidate_count; i++) {
        if (candidates[i].ready && dir_exists(candidates[i].path) &&
            access(candidates[i].path, R_OK | W_OK | X_OK) == 0) {
            snprintf(active_storage_root, sizeof(active_storage_root), "%s", candidates[i].path);
            active_storage_root_initialized = 1;
            if (error && error_len > 0) {
                error[0] = '\0';
            }
            return 1;
        }
    }

    // Second pass: nothing exists yet (fresh install) - create the first candidate that's
    // actually writable, in priority order.
    char attempt_errors[3][256];
    for (int i = 0; i < candidate_count; i++) {
        attempt_errors[i][0] = '\0';
        if (!candidates[i].ready) {
            snprintf(attempt_errors[i], sizeof(attempt_errors[i]), "path unavailable");
            continue;
        }
        if (ensure_root_candidate(candidates[i].path, attempt_errors[i], sizeof(attempt_errors[i]))) {
            snprintf(active_storage_root, sizeof(active_storage_root), "%s", candidates[i].path);
            active_storage_root_initialized = 1;
            if (error && error_len > 0) {
                error[0] = '\0';
            }
            return 1;
        }
    }

    if (error && error_len > 0) {
        snprintf(error, error_len,
                 "SD card unavailable. areas='%s' (%s); legacy='%s' (%s); uid='%s' (%s)",
                 candidates[0].path, attempt_errors[0],
                 candidates[1].path, attempt_errors[1],
                 candidates[2].ready ? candidates[2].path : "(unavailable)", attempt_errors[2]);
    }
    return 0;
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
    const char* root = storage_root();
    if (!storage_remove_tree(root)) {
        return set_error(error, error_len, "Failed to remove storage tree: %s (%s)", root);
    }
    return storage_ensure_root(error, error_len);
}

int storage_profile_dir(char* out, size_t out_len, const char* profile_id) {
    char profiles_dir[1024];
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