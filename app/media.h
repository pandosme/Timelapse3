#ifndef MEDIA_H
#define MEDIA_H

#include <stddef.h>

#define PACKAGED_FFMPEG_BIN "/usr/local/packages/timelapse2/bin/ffmpeg"
#define PACKAGED_FFMPEG_LIB "/usr/local/packages/timelapse2/lib/ffmpeg"

int media_ffmpeg_available(void);
const char* media_last_error(void);
int media_job_try_admit(void);
void media_job_release(void);
int media_job_is_active(void);
void media_exclusive_lock(void);
void media_exclusive_unlock(void);
void media_storage_transaction_lock(void);
void media_storage_transaction_unlock(void);
int media_generate_preview(const char* profile_id, int fps, char* out_path, size_t out_len, int allow_rebuild);
int media_generate_export(const char* profile_id, int fps, char* out_path, size_t out_len);
int media_generate_archive(const char* profile_id, int fps, const char* output_path);
long long media_estimate_export_size(const char* profile_id, int fps, int current_total_frames);
typedef int (*Media_Progress_Callback)(double percent, const char* detail, void* user_data);
int media_convert_avi_to_mp4(const char* input_path, const char* output_path, Media_Progress_Callback progress_callback, void* user_data);
int media_import_avi_to_live_mp4(const char* input_path, const char* profile_id, int fps, int finalized_frames, Media_Progress_Callback progress_callback, void* user_data);
int media_process_pending(const char* profile_id, int fps);
int media_process_pending_force(const char* profile_id, int fps);
int media_reencode_export(const char* profile_id, int old_fps, int new_fps);

#endif