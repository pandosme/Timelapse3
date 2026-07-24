#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

#define STORAGE_ROOT "/var/spool/storage/SD_DISK/timelapse2"
#define STORAGE_SD_BASE "/var/spool/storage/SD_DISK"
#define STORAGE_FALLBACK_ROOT "/usr/local/packages/timelapse2/localdata/timelapse2"

const char* storage_root(void);
int storage_ensure_root(char* error, size_t error_len);
int storage_ensure_directory(const char* path, char* error, size_t error_len);
int storage_reset(char* error, size_t error_len);
int storage_remove_tree(const char* path);
int storage_join(char* out, size_t out_len, const char* first, const char* second);
int storage_profiles_dir(char* out, size_t out_len);
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