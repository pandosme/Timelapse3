#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

// Candidate SD card locations, newest AXIS OS layout first. Older firmware only bind-mounts
// the SD card at STORAGE_SD_BASE; newer firmware (and some devices even on the same OS version)
// only mount it under the "storage areas" path, leaving STORAGE_SD_BASE present but empty and
// unwritable. storage_ensure_root() searches all of these for an existing "timelapse2" directory
// first (so upgrades keep using wherever a camera's data already lives), and only creates a new
// one - preferring this list's order - if none of them has one yet. There is deliberately no
// internal-flash fallback: recordings only ever belong on the SD card, so if none of these is
// usable, storage_ensure_root() fails rather than silently writing somewhere else.
#define STORAGE_SD_AREAS_BASE "/var/spool/storage/areas/SD_DISK/root"
#define STORAGE_SD_BASE "/var/spool/storage/SD_DISK"
#define STORAGE_ROOT "/var/spool/storage/SD_DISK/timelapse2"

const char* storage_root(void);
int storage_ensure_root(char* error, size_t error_len);
int storage_ensure_directory(const char* path, char* error, size_t error_len);
int storage_reset(char* error, size_t error_len);
int storage_remove_tree(const char* path);
int storage_join(char* out, size_t out_len, const char* first, const char* second);
int storage_profiles_dir(char* out, size_t out_len);
/* Non-zero when name is a single, safe path component - see storage.c. */
int storage_name_is_safe(const char* name);
int storage_profile_dir(char* out, size_t out_len, const char* profile_id);
int storage_frames_dir(char* out, size_t out_len, const char* profile_id);
int storage_frame_path(char* out, size_t out_len, const char* profile_id, unsigned frame_number);
int storage_cache_dir(char* out, size_t out_len, const char* profile_id);
int storage_preview_path(char* out, size_t out_len, const char* profile_id, int fps);
int storage_export_path(char* out, size_t out_len, const char* profile_id, int fps);
int storage_recordings_path(char* out, size_t out_len);
int storage_profiles_path(char* out, size_t out_len);
int storage_archive_dir(char* out, size_t out_len);
int storage_archive_index_path(char* out, size_t out_len);

#endif