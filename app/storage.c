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
    const char* preferred_root = (env_root && env_root[0]) ? env_root : STORAGE_ROOT;
    char uid_root[1024];
    int uid_root_ready = build_uid_storage_root(uid_root, sizeof(uid_root));
    char preferred_error[256];
    char uid_error[256];
    char fallback_error[256];

    if (ensure_root_candidate(preferred_root, preferred_error, sizeof(preferred_error))) {
        snprintf(active_storage_root, sizeof(active_storage_root), "%s", preferred_root);
        active_storage_root_initialized = 1;
        if (error && error_len > 0) {
            error[0] = '\0';
        }
        return 1;
    }

    if (uid_root_ready && ensure_root_candidate(uid_root, uid_error, sizeof(uid_error))) {
        snprintf(active_storage_root, sizeof(active_storage_root), "%s", uid_root);
        active_storage_root_initialized = 1;
        if (error && error_len > 0) {
            error[0] = '\0';
        }
        return 1;
    }

    if (ensure_root_candidate(STORAGE_FALLBACK_ROOT, fallback_error, sizeof(fallback_error))) {
        snprintf(active_storage_root, sizeof(active_storage_root), "%s", STORAGE_FALLBACK_ROOT);
        active_storage_root_initialized = 1;
        if (error && error_len > 0) {
            error[0] = '\0';
        }
        return 1;
    }

    if (error && error_len > 0) {
        snprintf(error, error_len,
                 "Storage unavailable. preferred='%s' error='%s'; uidRoot='%s' error='%s'; fallback='%s' error='%s'",
                 preferred_root,
                 preferred_error,
                 uid_root_ready ? uid_root : "(unavailable)",
                 uid_root_ready ? uid_error : "unable to build uid root",
                 STORAGE_FALLBACK_ROOT,
                 fallback_error);
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