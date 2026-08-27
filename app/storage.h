#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

typedef void (*StorageInitCallback)(int available, const char* message, void* user_data);

int storage_initialize_async(StorageInitCallback callback, void* user_data);
void storage_cleanup(void);
const char* storage_base(void);
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