#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <syslog.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include "vdo-stream.h"
#include "vdo-frame.h"
#include "vdo-types.h"
#include "ACAP.h"
#include "cJSON.h"
#include "recordings.h"
#include "timelapse.h"
#include "media.h"
#include "recording_store.h"
#include "storage.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define PATH_MAX_LEN 1024
#define RIFF_HEADER_SIZE 44
#define AVI_ALIGN_SIZE 2048

typedef unsigned int DWORD;

#if __BYTE_ORDER == __BIG_ENDIAN
    #define LILEND4(a) SWAP4((a))
#else
    #define LILEND4(a) (a)
#endif

#define SWAP4(x) (((x>>24) & 0x000000ff) | \
                  ((x>>8)  & 0x0000ff00) | \
                  ((x<<8)  & 0x00ff0000) | \
                  ((x<<24) & 0xff000000))

#define AVIF_HASINDEX 0x00000010

struct AVI_HEADER_STRUCT {
    DWORD LIST_RIFF;      // "RIFF"
    DWORD RIFF_size;      //
    DWORD RIFF_FOURCC;    // "AVI "
    DWORD LIST_HDRL;      // "LIST"
    DWORD hdrl_size;      // 208
    DWORD hdrl_name;      // "hdrl"
    DWORD avih;           // "avih"
    DWORD avih_size;      // 56
    DWORD AVIH_MicroSecPerFrame;
    DWORD AVIH_MaxBytesPerSec;
    DWORD AVIH_PaddingGranularity;
    DWORD AVIH_Flags;
    DWORD AVIH_TotalFrames;
    DWORD AVIH_InitialFrames;
    DWORD AVIH_Streams;
    DWORD AVIH_SugestedBufferSize;
    DWORD AVIH_Width;
    DWORD AVIH_Height;
    DWORD AVIH_Reserved1;
    DWORD AVIH_Reserved2;
    DWORD AVIH_Reserved3;
    DWORD AVIH_Reserved4;
    DWORD LIST_strl;      // "LIST"
    DWORD LIST_strl_size; // 132
    DWORD LIST_strl_name; // "strl"
    DWORD STRH_name;      // "strh"
    DWORD STRH_size;      // 48
    DWORD strh_fccType;
    DWORD strh_fccHandler;
    DWORD strh_flags;
    DWORD strh_priority;
    DWORD strh_init_frames;
    DWORD strh_scale;
    DWORD strh_rate;
    DWORD strh_start;
    DWORD strh_length;
    DWORD strh_sugg_buff_sz;
    DWORD strh_quality;
    DWORD strh_sample_sz;
    DWORD LIST_strf;      // "strf"
    DWORD strf_size_list; // 40
    DWORD strf_size;      // 40
    DWORD strf_width;
    DWORD strf_height;
    DWORD strf_planes_bit_cnt;
    DWORD strf_compression;
    DWORD strf_image_size;
    DWORD strf_xpels_meter;
    DWORD strf_ypels_meter;
    DWORD strf_num_colors;
    DWORD strf_imp_colors;
    DWORD LIST_ODML;      // "LIST"
    DWORD LIST_ODML_Size; // 16
    DWORD LIST_ODML_type; // "odml"
    DWORD odml_fourCC;    // "dmlh"
    DWORD odml_size;      // 4
    DWORD odml_frames;
    DWORD LIST_movi;      // "LIST"
    DWORD LIST_movi_size; // SUM(JPEG data size) + (8 * frames) + 4
    DWORD LIST_movi_name; // "movi"
};
typedef struct AVI_HEADER_STRUCT AVI_HEADER;

struct AVI_INDEX_ENTRY_STRUCT {
    DWORD fourCC;    // "00dc"
    DWORD flags;     // Usually 0
    DWORD offset;    // Offset from movi start
    DWORD size;      // Size of frame
};
typedef struct AVI_INDEX_ENTRY_STRUCT AVI_INDEX_ENTRY;

struct LIST_INDEX_STRUCT {
    DWORD fourCC;
    DWORD size;
};
typedef struct LIST_INDEX_STRUCT LIST_INDEX;

struct AVIOLDINDEX_STRUCT {
    DWORD fourCC;    // 'idx1'
    DWORD cb;        // Size not including first 8 bytes
};
typedef struct AVIOLDINDEX_STRUCT AVIOLDINDEX;

static cJSON* Recordings_Container = NULL;

static DWORD FOURCC(const char* str) {
    DWORD value = 0;
    value = str[3];
    value <<= 8;
    value += str[2];
    value <<= 8;
    value += str[1];
    value <<= 8;
    value += str[0];
    return value;
}

static cJSON *ArchiveList = NULL;
// Guards ArchiveList. It's touched from the http-thread (manual archive/delete actions, the
// GET archive endpoint) and from the GLib main-loop thread (automatic midnight archiving via
// check_midnight -> Recordings_Archive, and now the background archive-media thread below).
// Recursive because Retention_Cleanup() calls Recordings_Delete_Archive() while already holding it.
static GRecMutex archive_list_mutex;

typedef struct {
    char profile_id[128];
} ResetMediaTask;

static void Set_Reset_Media_Status_Active(const char* profile_id) {
    ACAP_STATUS_SetBool("mediaJob", "active", 1);
    ACAP_STATUS_SetString("mediaJob", "kind", "reset_media");
    ACAP_STATUS_SetString("mediaJob", "profileId", profile_id ? profile_id : "");
    ACAP_STATUS_SetString("mediaJob", "stage", "Resetting recording media");
    ACAP_STATUS_SetNumber("mediaJob", "estimatedSeconds", 10.0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", 5.0);
    ACAP_STATUS_SetString("mediaJob", "message", "Deleting captured frames and generated videos...");
}

static void Set_Reset_Media_Status_Done(int ok, const char* message) {
    ACAP_STATUS_SetBool("mediaJob", "active", 0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", ok ? 100.0 : 0.0);
    ACAP_STATUS_SetString("mediaJob", "message", message ? message : (ok ? "Recording media reset" : "Reset failed"));
}

static gpointer Reset_Media_Thread(gpointer user_data) {
    ResetMediaTask* task = (ResetMediaTask*)user_data;
    if (!task) {
        ACAP_Background_Job_End();
        return NULL;
    }

    LOG("%s: start profile=%s\n", __func__, task->profile_id);
    ACAP_STATUS_SetNumber("mediaJob", "progress", 10.0);
    if (Recordings_Clear(task->profile_id) == 0) {
        Set_Reset_Media_Status_Done(1, "Recording media reset");
        LOG("%s: done profile=%s\n", __func__, task->profile_id);
    } else {
        Set_Reset_Media_Status_Done(0, "Failed to reset recording media");
        LOG_WARN("%s: failed profile=%s\n", __func__, task->profile_id);
    }

    g_free(task);
    ACAP_Background_Job_End();
    return NULL;
}

static int Queue_Reset_Media(const char* profile_id) {
    if (!profile_id || !profile_id[0]) {
        return 0;
    }

    ResetMediaTask* task = g_new0(ResetMediaTask, 1);
    if (!task) {
        return 0;
    }

    snprintf(task->profile_id, sizeof(task->profile_id), "%s", profile_id);
    Set_Reset_Media_Status_Active(profile_id);

    ACAP_Background_Job_Begin();
    GThread* thread = g_thread_new("reset-media", Reset_Media_Thread, task);
    if (!thread) {
        ACAP_Background_Job_End();
        g_free(task);
        Set_Reset_Media_Status_Done(0, "Failed to start media reset");
        return 0;
    }
    g_thread_unref(thread);
    return 1;
}

static void write_avi_header(FILE* f, DWORD frames, DWORD totalJPEGSize, DWORD width, DWORD height, unsigned int fps);
static size_t write_avi_frame(FILE* f, const unsigned char* data, size_t size);
static int avi_add_index_entry(FILE* file, unsigned int frame, unsigned int jpeg_size);
static void ensure_profile_directory(const char* profileId);
static cJSON* load_recordings(void);
static void save_recordings(void);
int Recordings_Archive(const char *profileID);
static void ensure_directory(const char *path);
static void replace_spaces_with_underscores(char *str);
static void load_archive_list();
static void save_archive_list();
static void update_avi_fps(FILE* f, unsigned int fps);
int Recordings_Delete_Archive(const char* filename);

static int stream_file_response(const ACAP_HTTP_Response response, const char* path, const char* content_type, const char* disposition, const char* filename);
static void normalize_mp4_filename(char* out, size_t out_len, const char* filename);
static void build_timestamped_export_filename(char* out, size_t out_len, const char* profile_name);

// Background job wrappers for anything that can trigger a slow ffmpeg pass (generate_mp4() in
// media.c). The app's HTTP handling is a single thread processing one FastCGI request at a time
// (see ACAP_HTTP_Process() in ACAP.c) - a handler that blocks on ffmpeg blocks every other
// request, including the "mediaJob" status polls the progress bar depends on. Running the real
// work on a GThread and returning "started" immediately keeps the status endpoint responsive,
// mirroring the pattern Reset Media already used above.

typedef struct {
    char profile_id[128];
    int fps;
} RefreshTask;

static void Set_Refresh_Status_Active(const char* profile_id) {
    ACAP_STATUS_SetBool("mediaJob", "active", 1);
    ACAP_STATUS_SetString("mediaJob", "kind", "refresh");
    ACAP_STATUS_SetString("mediaJob", "profileId", profile_id ? profile_id : "");
    ACAP_STATUS_SetString("mediaJob", "stage", "Refreshing recording");
    ACAP_STATUS_SetNumber("mediaJob", "estimatedSeconds", 5.0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", 2.0);
    ACAP_STATUS_SetString("mediaJob", "message", "Processing...");
}

// Always called at the end of the background thread, regardless of which internal path
// generate_mp4() took - it may not touch "mediaJob" itself at all (e.g. a no-op cache hit), so
// this is what guarantees "active" always flips back to 0 exactly once per job.
static void Set_Refresh_Status_Done(int ok, const char* message) {
    ACAP_STATUS_SetBool("mediaJob", "active", 0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", ok ? 100.0 : 0.0);
    ACAP_STATUS_SetString("mediaJob", "message", message ? message : (ok ? "Recording refreshed" : "Refresh failed"));
}

static gpointer Refresh_Media_Thread(gpointer user_data) {
    RefreshTask* task = (RefreshTask*)user_data;
    if (!task) {
        ACAP_Background_Job_End();
        return NULL;
    }

    LOG("%s: start profile=%s fps=%d\n", __func__, task->profile_id, task->fps);
    int ok = media_process_pending_force(task->profile_id, task->fps);
    if (ok) {
        LOG("%s: done profile=%s\n", __func__, task->profile_id);
        Set_Refresh_Status_Done(1, "Recording refreshed");
    } else {
        LOG_WARN("%s: failed profile=%s err=%s\n", __func__, task->profile_id, media_last_error());
        Set_Refresh_Status_Done(0, media_last_error());
    }

    g_free(task);
    ACAP_Background_Job_End();
    return NULL;
}

static int Queue_Refresh_Media(const char* profile_id, int fps) {
    if (!profile_id || !profile_id[0]) {
        return 0;
    }

    RefreshTask* task = g_new0(RefreshTask, 1);
    if (!task) {
        return 0;
    }

    snprintf(task->profile_id, sizeof(task->profile_id), "%s", profile_id);
    task->fps = fps;
    Set_Refresh_Status_Active(profile_id);

    ACAP_Background_Job_Begin();
    GThread* thread = g_thread_new("refresh-media", Refresh_Media_Thread, task);
    if (!thread) {
        ACAP_Background_Job_End();
        g_free(task);
        Set_Refresh_Status_Done(0, "Failed to start refresh");
        return 0;
    }
    g_thread_unref(thread);
    return 1;
}

typedef struct {
    char profile_id[128];
} ArchiveTask;

static void Set_Archive_Status_Active(const char* profile_id) {
    ACAP_STATUS_SetBool("mediaJob", "active", 1);
    ACAP_STATUS_SetString("mediaJob", "kind", "archive");
    ACAP_STATUS_SetString("mediaJob", "profileId", profile_id ? profile_id : "");
    ACAP_STATUS_SetString("mediaJob", "stage", "Archiving recording");
    ACAP_STATUS_SetNumber("mediaJob", "estimatedSeconds", 10.0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", 2.0);
    ACAP_STATUS_SetString("mediaJob", "message", "Processing...");
}

static void Set_Archive_Status_Done(int ok, const char* message) {
    ACAP_STATUS_SetBool("mediaJob", "active", 0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", ok ? 100.0 : 0.0);
    ACAP_STATUS_SetString("mediaJob", "message", message ? message : (ok ? "Recording archived" : "Archive failed"));
}

static gpointer Archive_Media_Thread(gpointer user_data) {
    ArchiveTask* task = (ArchiveTask*)user_data;
    if (!task) {
        ACAP_Background_Job_End();
        return NULL;
    }

    LOG("%s: start profile=%s\n", __func__, task->profile_id);
    int result = Recordings_Archive(task->profile_id);
    if (result == 0) {
        LOG("%s: done profile=%s\n", __func__, task->profile_id);
        Set_Archive_Status_Done(1, "Recording archived");
    } else {
        LOG_WARN("%s: failed profile=%s err=%s\n", __func__, task->profile_id, media_last_error());
        Set_Archive_Status_Done(0, "Failed to archive recording");
    }

    g_free(task);
    ACAP_Background_Job_End();
    return NULL;
}

static int Queue_Archive_Media(const char* profile_id) {
    if (!profile_id || !profile_id[0]) {
        return 0;
    }

    ArchiveTask* task = g_new0(ArchiveTask, 1);
    if (!task) {
        return 0;
    }

    snprintf(task->profile_id, sizeof(task->profile_id), "%s", profile_id);
    Set_Archive_Status_Active(profile_id);

    ACAP_Background_Job_Begin();
    GThread* thread = g_thread_new("archive-media", Archive_Media_Thread, task);
    if (!thread) {
        ACAP_Background_Job_End();
        g_free(task);
        Set_Archive_Status_Done(0, "Failed to start archive");
        return 0;
    }
    g_thread_unref(thread);
    return 1;
}

typedef struct {
    char profile_id[128];
    int fps;
} PreviewTask;

static void Set_Preview_Status_Active(const char* profile_id) {
    ACAP_STATUS_SetBool("mediaJob", "active", 1);
    ACAP_STATUS_SetString("mediaJob", "kind", "preview");
    ACAP_STATUS_SetString("mediaJob", "profileId", profile_id ? profile_id : "");
    ACAP_STATUS_SetString("mediaJob", "stage", "Preparing video");
    ACAP_STATUS_SetNumber("mediaJob", "estimatedSeconds", 3.0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", 2.0);
    ACAP_STATUS_SetString("mediaJob", "message", "Processing...");
}

static void Set_Preview_Status_Done(int ok, const char* message) {
    ACAP_STATUS_SetBool("mediaJob", "active", 0);
    ACAP_STATUS_SetNumber("mediaJob", "progress", ok ? 100.0 : 0.0);
    ACAP_STATUS_SetString("mediaJob", "message", message ? message : (ok ? "Preview ready" : "Preview failed"));
}

static gpointer Preview_Media_Thread(gpointer user_data) {
    PreviewTask* task = (PreviewTask*)user_data;
    if (!task) {
        ACAP_Background_Job_End();
        return NULL;
    }

    LOG("%s: start profile=%s fps=%d\n", __func__, task->profile_id, task->fps);
    char out_path[PATH_MAX_LEN];
    int ok = media_generate_preview(task->profile_id, task->fps, out_path, sizeof(out_path), 1);
    if (ok) {
        LOG("%s: done profile=%s\n", __func__, task->profile_id);
        Set_Preview_Status_Done(1, "Preview ready");
    } else {
        LOG_WARN("%s: failed profile=%s err=%s\n", __func__, task->profile_id, media_last_error());
        Set_Preview_Status_Done(0, media_last_error());
    }

    g_free(task);
    ACAP_Background_Job_End();
    return NULL;
}

// Unlike Refresh/Archive, this never touches the permanent export cache or purges raw frames
// (see media_generate_preview()'s own comment) - it just builds/refreshes the disposable
// preview.mp4 that HTTP_Endpoint_Video's GET then streams. Still backgrounded because the tail
// encode it may need to run is not instant and must not block the request-handling thread.
static int Queue_Preview_Media(const char* profile_id, int fps) {
    if (!profile_id || !profile_id[0]) {
        return 0;
    }

    PreviewTask* task = g_new0(PreviewTask, 1);
    if (!task) {
        return 0;
    }

    snprintf(task->profile_id, sizeof(task->profile_id), "%s", profile_id);
    task->fps = fps;
    Set_Preview_Status_Active(profile_id);

    ACAP_Background_Job_Begin();
    GThread* thread = g_thread_new("preview-media", Preview_Media_Thread, task);
    if (!thread) {
        ACAP_Background_Job_End();
        g_free(task);
        Set_Preview_Status_Done(0, "Failed to start preview");
        return 0;
    }
    g_thread_unref(thread);
    return 1;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((long long)ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

//pthread_mutex_t recordings_mutex = //pthread_mutex_INITIALIZER;

static void write_avi_header(FILE* f, DWORD frames, DWORD totalJPEGSize, DWORD width, DWORD height, unsigned int fps) {
    AVI_HEADER header;
    DWORD riffsize;

    if (!fps) fps = 30;

    header.LIST_RIFF = FOURCC("RIFF");
    riffsize = sizeof(AVI_HEADER);  // Instead of 220
    riffsize += sizeof(LIST_INDEX) + totalJPEGSize + (sizeof(LIST_INDEX) * frames); // movi
    riffsize += sizeof(LIST_INDEX) + (sizeof(AVI_INDEX_ENTRY) * frames); // index
    header.RIFF_size = LILEND4(riffsize);
    header.RIFF_FOURCC = FOURCC("AVI ");
    header.LIST_HDRL = FOURCC("LIST");
    header.hdrl_size = LILEND4(208);
    header.hdrl_name = FOURCC("hdrl");
    header.avih = FOURCC("avih");
    header.avih_size = LILEND4(56);
    header.AVIH_MicroSecPerFrame = LILEND4(1000000/fps);
    header.AVIH_MaxBytesPerSec = LILEND4(width * height * 3 * fps);
    header.AVIH_PaddingGranularity = LILEND4(0);
    header.AVIH_Flags = LILEND4(AVIF_HASINDEX);
    header.AVIH_TotalFrames = LILEND4(frames);
    header.AVIH_InitialFrames = LILEND4(0);
    header.AVIH_Streams = LILEND4(1);
    header.AVIH_SugestedBufferSize = LILEND4(width * height * 3);
    header.AVIH_Width = LILEND4(width);
    header.AVIH_Height = LILEND4(height);
    header.AVIH_Reserved1 = LILEND4(0);
    header.AVIH_Reserved2 = LILEND4(0);
    header.AVIH_Reserved3 = LILEND4(0);
    header.AVIH_Reserved4 = LILEND4(0);

    // Stream LIST
    header.LIST_strl = FOURCC("LIST");
    header.LIST_strl_size = LILEND4(132);
    header.LIST_strl_name = FOURCC("strl");
    header.STRH_name = FOURCC("strh");
    header.STRH_size = LILEND4(48);
    header.strh_fccType = FOURCC("vids");
    header.strh_fccHandler = FOURCC("MJPG");
    header.strh_flags = LILEND4(0);
    header.strh_priority = LILEND4(0);
    header.strh_init_frames = LILEND4(0);
    header.strh_scale = LILEND4(1);
    header.strh_rate = LILEND4(fps);
    header.strh_start = LILEND4(0);
    header.strh_length = LILEND4(frames);
    header.strh_sugg_buff_sz = LILEND4(width * height * 3);
    header.strh_quality = LILEND4(0);
    header.strh_sample_sz = LILEND4(0);

    // Stream format
    header.LIST_strf = FOURCC("strf");
    header.strf_size_list = LILEND4(40);
    header.strf_size = LILEND4(40);
    header.strf_width = LILEND4(width);
    header.strf_height = LILEND4(height);
    header.strf_planes_bit_cnt = LILEND4(1 | (24<<16));  // 1 plane, 24 bits
    header.strf_compression = FOURCC("MJPG");
    header.strf_image_size = LILEND4(width * height * 3);
    header.strf_xpels_meter = LILEND4(0);
    header.strf_ypels_meter = LILEND4(0);
    header.strf_num_colors = LILEND4(0);
    header.strf_imp_colors = LILEND4(0);

    // ODML
    header.LIST_ODML = FOURCC("LIST");
    header.LIST_ODML_Size = LILEND4(16);
    header.LIST_ODML_type = FOURCC("odml");
    header.odml_fourCC = FOURCC("dmlh");
    header.odml_size = LILEND4(4);
    header.odml_frames = LILEND4(frames);

    // Movie data
	header.LIST_movi = FOURCC("LIST");
	header.LIST_movi_size = LILEND4(4 + totalJPEGSize + (frames * sizeof(LIST_INDEX)));
    header.LIST_movi_name = FOURCC("movi");
	fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(AVI_HEADER), 1, f);
}

static int avi_initialize_index(FILE* file) {
    AVIOLDINDEX header;

    if (!file) return 0;

    header.fourCC = FOURCC("idx1");
    header.cb = LILEND4(0);  // Initial size is 0
	fseek(file, 0, SEEK_SET);
    return fwrite(&header, sizeof(AVIOLDINDEX), 1, file) == 1;
}

static int avi_add_index_entry(FILE* file, unsigned int frames, unsigned int jpeg_size) {
    AVI_INDEX_ENTRY index_entry;
    AVI_INDEX_ENTRY prev_entry;
    AVIOLDINDEX header;
    size_t offset;
	LOG_TRACE("%s: Frames = %d \n",__func__, frames);
    if (!file) return 0;

    // Update index header with correct size
    fseek(file, 0, SEEK_SET);
    header.fourCC = FOURCC("idx1");
    header.cb = LILEND4(frames * sizeof(AVI_INDEX_ENTRY));
    fwrite(&header, sizeof(AVIOLDINDEX), 1, file);

    // Calculate offset based on previous frame
    if (frames > 1) {
        fseek(file, -sizeof(AVI_INDEX_ENTRY), SEEK_END);
        fread(&prev_entry, sizeof(AVI_INDEX_ENTRY), 1, file);
        offset = LILEND4(prev_entry.offset) + LILEND4(prev_entry.size) + sizeof(LIST_INDEX);
    } else {
        offset = 4;  // First frame starts after "movi" tag
    }

    // Create index entry
    index_entry.fourCC = FOURCC("00db");
    index_entry.flags = LILEND4(0);
    index_entry.offset = LILEND4(offset);
    index_entry.size = LILEND4(jpeg_size);

    fseek(file, 0, SEEK_END);
    return fwrite(&index_entry, sizeof(AVI_INDEX_ENTRY), 1, file) == 1;
}

static size_t write_avi_frame(FILE* f, const unsigned char* data, size_t size) {
    // Calculate padding needed for 4-byte alignment
	fseek(f, 0, SEEK_END);
    unsigned int padding = (4-(size%4)) % 4;
    size_t total_size = size + padding;

	LOG_TRACE("%s:\n",__func__);


    LIST_INDEX lindex;
    lindex.fourCC = FOURCC("00db");
    lindex.size = LILEND4(size);

    fwrite(&lindex, sizeof(LIST_INDEX), 1, f);
    fwrite((void*)data, 1, size, f);

    // Write padding if needed
    if (padding > 0) {
        char pad[4] = {0};
        fwrite(pad, 1, padding, f);
    }

    return total_size;
}

// Helper function to ensure a directory exists
static void ensure_profile_directory(const char* profileId) {

    char path[PATH_MAX_LEN];
    char error[256];
    if (storage_profile_dir(path, sizeof(path), profileId)) {
        storage_ensure_directory(path, error, sizeof(error));
    }
}

static void ensure_directory(const char *path) {
	LOG_TRACE("%s: %s\n",__func__,path);
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0755);
    }
}

static cJSON* load_recordings(void) {
	//pthread_mutex_lock(&recordings_mutex);
    char path[PATH_MAX_LEN];
    if (!storage_recordings_path(path, sizeof(path))) {
        return cJSON_CreateObject();
    }

    FILE* file = fopen(path, "r");
    if (!file) {
		//pthread_mutex_unlock(&recordings_mutex);
        return cJSON_CreateObject();
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* json = malloc(size + 1);
    if (!json) {
        fclose(file);
		//pthread_mutex_unlock(&recordings_mutex);
        return cJSON_CreateObject();
    }

    fread(json, 1, size, file);
    json[size] = 0;
    fclose(file);

    cJSON* recordings = cJSON_Parse(json);
    free(json);
	//pthread_mutex_unlock(&recordings_mutex);
    return recordings ? recordings : cJSON_CreateObject();
}

static void save_recordings(void) {
	LOG_TRACE("%s: Entry",__func__);
	//pthread_mutex_lock(&recordings_mutex);
    char path[PATH_MAX_LEN];
    if (!storage_recordings_path(path, sizeof(path))) {
        return;
    }

    char* json = cJSON_PrintUnformatted(Recordings_Container);
    if (!json) return;

    FILE* file = fopen(path, "w");
    if (file) {
        fwrite(json, strlen(json), 1, file);
        fclose(file);
    }
    free(json);
	//pthread_mutex_unlock(&recordings_mutex);
}

static int append_file(const char *source, const char *destination) {
    FILE *src = fopen(source, "rb");
    FILE *dest = fopen(destination, "rb+");  // Open in read/write mode
    if (!src || !dest) {
        if (src) fclose(src);
        if (dest) fclose(dest);
        return -1;
    }

    // Get file sizes
    fseek(dest, 0, SEEK_END);
    long aviSize = ftell(dest);
    fseek(src, 0, SEEK_END);
    long idxSize = ftell(src);

    // Update RIFF size in header
    fseek(dest, 4, SEEK_SET);
    DWORD newSize = LILEND4(aviSize + idxSize - 8);
    fwrite(&newSize, sizeof(DWORD), 1, dest);

    // Append index data at end
    fseek(dest, aviSize, SEEK_SET);
    fseek(src, 0, SEEK_SET);

    char buffer[65536];
    size_t bytesRead;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytesRead, dest);
    }

    fclose(src);
    fclose(dest);
    return 0;
}

// Helper function to replace spaces with underscores in a string
static void replace_spaces_with_underscores(char *str) {
    for (char *p = str; *p; ++p) {
        if (*p == ' ') {
            *p = '_';
        }
    }
}

static void load_archive_list() {
	LOG_TRACE("%s: Entry",__func__);
    g_rec_mutex_lock(&archive_list_mutex);

    char path[PATH_MAX_LEN];
    if (!storage_archive_index_path(path, sizeof(path))) {
        LOG_WARN("%s: Failed to build archive index path\n", __func__);
        ArchiveList = cJSON_CreateArray();
        g_rec_mutex_unlock(&archive_list_mutex);
        return;
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        ArchiveList = cJSON_CreateArray();
        g_rec_mutex_unlock(&archive_list_mutex);
        return;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *json = malloc(size + 1);
    if (!json) {
        fclose(file);
        ArchiveList = cJSON_CreateArray();
        g_rec_mutex_unlock(&archive_list_mutex);
        return;
    }

    fread(json, 1, size, file);
    json[size] = '\0';
    fclose(file);

    ArchiveList = cJSON_Parse(json);
	if(!ArchiveList)
		LOG_WARN("Error parsing archive index");
    free(json);

    if (!ArchiveList) {
        ArchiveList = cJSON_CreateArray();
    }
    g_rec_mutex_unlock(&archive_list_mutex);
}

// Function to save the archive list to recordings.json
static void save_archive_list() {
	LOG_TRACE("%s: Entry",__func__);
    char archive_dir[PATH_MAX_LEN];
    char path[PATH_MAX_LEN];
    char error[256];
    if (!storage_archive_dir(archive_dir, sizeof(archive_dir)) ||
        !storage_archive_index_path(path, sizeof(path))) {
        LOG_WARN("%s: Failed to build archive index path\n", __func__);
        return;
    }

    if (!storage_ensure_directory(archive_dir, error, sizeof(error))) {
        LOG_WARN("%s: Failed to prepare archive directory: %s\n", __func__, error);
        return;
    }

    g_rec_mutex_lock(&archive_list_mutex);
    char *jsonString = cJSON_PrintUnformatted(ArchiveList);
    g_rec_mutex_unlock(&archive_list_mutex);
    if (!jsonString) {
        return;
    }

    FILE *file = fopen(path, "w");
    if (file) {
        fwrite(jsonString, strlen(jsonString), 1, file);
        fclose(file);
    }

    free(jsonString);
}

static void Retention_Cleanup() {
    // Get retention period from settings
    int retentionMonths = 12;
    cJSON* settings = ACAP_Get_Config("settings");
    if (settings && cJSON_GetObjectItem(settings, "retentionMonths")) {
        retentionMonths = cJSON_GetObjectItem(settings, "retentionMonths")->valueint;
    } else {
		LOG_WARN("%s: Invalid settings retentionMonths configuration\n",__func__);
	}


    // Get current time
    time_t now = time(NULL);

    g_rec_mutex_lock(&archive_list_mutex);

    // Load archive list if not loaded
    if (!ArchiveList) {
        load_archive_list();
    }

    if (!ArchiveList) {
        g_rec_mutex_unlock(&archive_list_mutex);
        return;
    }

    // Check each archive
    cJSON* archive = ArchiveList->child;
    while(archive) {
		double archive_timestamp = cJSON_GetObjectItem(archive, "last")?cJSON_GetObjectItem(archive, "last")->valuedouble:0;
		if( archive_timestamp ) {
			double age_in_days = (ACAP_DEVICE_Timestamp() - archive_timestamp) / (24 * 3600 * 1000);
			if (age_in_days >= retentionMonths * 31) {
				const char* filename = cJSON_GetObjectItem(archive, "filename")->valuestring;
				LOG("Removing archived recording %s\n",filename);
				Recordings_Delete_Archive(filename);
				archive = ArchiveList->child;
			} else {
				archive = archive->next;
			}
		} else {
			archive = archive->next;
		}
    }

    g_rec_mutex_unlock(&archive_list_mutex);
}

static int days_in_month(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1];
}

static const char* profile_archive_schedule(cJSON* profile) {
    const char* schedule = cJSON_GetStringValue(profile ? cJSON_GetObjectItem(profile, "archiveSchedule") : NULL);
    if (!schedule) {
        return "monthly";
    }
    if (strcmp(schedule, "none") == 0 || strcmp(schedule, "daily") == 0 || strcmp(schedule, "weekly") == 0 || strcmp(schedule, "monthly") == 0) {
        return schedule;
    }
    return "monthly";
}

static int recording_due_for_archive(const char* profile_id, cJSON* recording, GDateTime* now) {
    cJSON* first = recording ? cJSON_GetObjectItem(recording, "first") : NULL;
    if (!profile_id || !first || first->valuedouble <= 0 || !now) {
        return 0;
    }

    cJSON* profile = Timelapse_Find_Profile_By_Id(profile_id);
    if (!profile) {
        LOG_WARN("%s: No profile found for active recording %s\n", __func__, profile_id);
        return 0;
    }

    const char* schedule = profile_archive_schedule(profile);
    if (strcmp(schedule, "none") == 0) {
        return 0;
    }

    if (strcmp(schedule, "daily") == 0) {
        return 1;
    }

    if (strcmp(schedule, "weekly") == 0) {
        return g_date_time_get_day_of_week(now) == 7;
    }

    int year = g_date_time_get_year(now);
    int month = g_date_time_get_month(now);
    int day = g_date_time_get_day_of_month(now);
    return day == days_in_month(year, month);
}

int Recordings_Clear(const char* profileId) {
    int result = recording_store_clear(profileId);
    Recordings_Container = recording_store_list();
    return result ? 0 : -1;
}

static void update_avi_fps(FILE* f, unsigned int fps) {
	LOG_TRACE("%s: Updating FPS=%d\n",__func__,fps);
    // Update microseconds per frame
    fseek(f, offsetof(AVI_HEADER, AVIH_MicroSecPerFrame), SEEK_SET);
    DWORD usec = LILEND4(1000000/fps);
    fwrite(&usec, sizeof(DWORD), 1, f);

    // Update rate in stream header
    fseek(f, offsetof(AVI_HEADER, strh_rate), SEEK_SET);
    DWORD rate = LILEND4(fps);
    fwrite(&rate, sizeof(DWORD), 1, f);
}

static void normalize_mp4_filename(char* out, size_t out_len, const char* filename) {
    if (!out || out_len == 0) return;

    // Copy the source out first: callers may pass out==filename, and snprintf's
    // behavior when source and destination overlap is undefined (seen in practice
    // to silently truncate to an empty string).
    char source_copy[512];
    const char* source = filename && filename[0] ? filename : "timelapse.mp4";
    snprintf(source_copy, sizeof(source_copy), "%s", source);
    snprintf(out, out_len, "%s", source_copy);

    char* extension = strrchr(out, '.');
    if (extension) {
        snprintf(extension, out_len - (size_t)(extension - out), ".mp4");
    } else {
        size_t len = strlen(out);
        if (len + 4 < out_len) {
            strcat(out, ".mp4");
        }
    }
}

static void build_timestamped_export_filename(char* out, size_t out_len, const char* profile_name) {
    if (!out || out_len == 0) {
        return;
    }

    const char* source_name = (profile_name && profile_name[0]) ? profile_name : "timelapse";

    char safe_name[256];
    size_t src_i = 0;
    size_t dst_i = 0;
    while (source_name[src_i] != '\0' && dst_i + 1 < sizeof(safe_name)) {
        unsigned char ch = (unsigned char)source_name[src_i++];
        if (isalnum(ch) || ch == '-' || ch == '_') {
            safe_name[dst_i++] = (char)ch;
        } else if (ch == ' ' || ch == '.') {
            safe_name[dst_i++] = '_';
        }
    }
    safe_name[dst_i] = '\0';

    if (safe_name[0] == '\0') {
        snprintf(safe_name, sizeof(safe_name), "timelapse");
    }

    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);

    char date_part[16];
    char time_part[16];
    strftime(date_part, sizeof(date_part), "%Y%m%d", &local_time);
    strftime(time_part, sizeof(time_part), "%H%M%S", &local_time);

    snprintf(out, out_len, "%s-%s-%s.mp4", safe_name, date_part, time_part);
    normalize_mp4_filename(out, out_len, out);
}

static int stream_file_response(const ACAP_HTTP_Response response, const char* path, const char* content_type, const char* disposition, const char* filename) {
    long long started_ms = monotonic_ms();
    LOG("%s: start path=%s disposition=%s filename=%s\n", __func__, path ? path : "(null)", disposition ? disposition : "(null)", filename ? filename : "(null)");

    FILE* file = fopen(path, "rb");
    if (!file) {
        LOG_WARN("%s: open failed path=%s err=%s\n", __func__, path ? path : "(null)", strerror(errno));
        ACAP_HTTP_Respond_Error(response, 404, "File not found");
        return 0;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    ACAP_HTTP_Respond_String(response, "Status: 200 OK\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Type: %s\r\n", content_type);
    ACAP_HTTP_Respond_String(response, "Content-Disposition: %s; filename=\"%s\"; filename*=UTF-8''%s\r\n", disposition, filename, filename);
    ACAP_HTTP_Respond_String(response, "Content-Length: %ld\r\n", file_size);
    ACAP_HTTP_Respond_String(response, "\r\n");

    char* buffer = malloc(65536);
    if (!buffer) {
        fclose(file);
        ACAP_HTTP_Respond_Error(response, 500, "Memory allocation failed");
        return 0;
    }

    size_t bytes_read;
    long long bytes_sent = 0;
    while ((bytes_read = fread(buffer, 1, 65536, file)) > 0) {
        if (ACAP_HTTP_Respond_Data(response, bytes_read, buffer) != 1) {
            LOG_WARN("%s: short send path=%s bytes_sent=%lld\n", __func__, path ? path : "(null)", bytes_sent);
            break;
        }
        bytes_sent += (long long)bytes_read;
    }

    free(buffer);
    fclose(file);
    LOG("%s: done path=%s size=%ld bytes_sent=%lld elapsed_ms=%lld\n",
        __func__, path ? path : "(null)", file_size, bytes_sent, monotonic_ms() - started_ms);
    return 1;
}

cJSON* Recordings_Get_List(void) {
    Recordings_Container = recording_store_list();
    return Recordings_Container;
}

cJSON* Recordings_Get_Metadata(const char* profileId) {
    return recording_store_get(profileId);
}

int Recordings_Capture(cJSON* profile) {
    int result = recording_store_capture_profile(profile);
    Recordings_Container = recording_store_list();
    return result ? 0 : -1;
}

int Recordings_Archive(const char *profileID) {
    char archivePath[PATH_MAX_LEN];
    char archiveFilename[PATH_MAX_LEN];
    char archiveFullPath[PATH_MAX_LEN];
    char error[256];

    long long started_ms = monotonic_ms();

    // Validate input
    if (!profileID) {
        LOG_WARN("Invalid profile ID\n");
        return -1;
    }

	LOG_TRACE("%s: Entry %s",__func__,profileID);
    if (!storage_archive_dir(archivePath, sizeof(archivePath)) ||
        !storage_ensure_directory(archivePath, error, sizeof(error))) {
        LOG_WARN("%s: Failed to prepare archive directory: %s\n", __func__, error);
        return -1;
    }

    // Get metadata before clearing anything
    cJSON *recordingMetadata = Recordings_Get_Metadata(profileID);
    if (!recordingMetadata) {
        LOG_WARN("No metadata found for profile: %s\n", profileID);
		//pthread_mutex_unlock(&recordings_mutex);
        return -1;
    }

    // Get profile information
    cJSON *profile = Timelapse_Find_Profile_By_Id(profileID);
    if (!profile) {
        LOG_WARN("Profile not found for ID: %s\n", profileID);
		//pthread_mutex_unlock(&recordings_mutex);
        return -1;
    }

    // Create archive filename
    cJSON *nameItem = cJSON_GetObjectItem(profile, "name");
    if (!nameItem || !nameItem->valuestring) {
        LOG_WARN("%s: Profile missing 'name' field: %s\n", __func__, profileID);
        return -1;
    }
    const char *profileName = nameItem->valuestring;
    char sanitizedProfileName[PATH_MAX_LEN];
    strncpy(sanitizedProfileName, profileName, PATH_MAX_LEN - 1);
    sanitizedProfileName[PATH_MAX_LEN - 1] = '\0';
    replace_spaces_with_underscores(sanitizedProfileName);

    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    snprintf(archiveFilename, sizeof(archiveFilename),
             "%s_%04d_%02d_%02d_%02d%02d.mp4",
             sanitizedProfileName,
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
             timeinfo->tm_mday, timeinfo->tm_hour, timeinfo->tm_min);

    if (!storage_join(archiveFullPath, sizeof(archiveFullPath), archivePath, archiveFilename)) {
        LOG_WARN("%s: Archive filename is too long\n", __func__);
        return -1;
    }

    cJSON *fpsItem = cJSON_GetObjectItem(recordingMetadata, "fps");
    int fps = fpsItem ? fpsItem->valueint : 10;
    LOG("%s: profile=%s fps=%d output=%s\n", __func__, profileID, fps, archiveFullPath);
    if (!media_generate_archive(profileID, fps, archiveFullPath)) {
        LOG_WARN("%s: Failed to create archive MP4 profile=%s fps=%d output=%s err=%s\n",
                 __func__, profileID, fps, archiveFullPath, media_last_error());
        unlink(archiveFullPath);
        return -1;
    }

    struct stat archiveStat;
    if (stat(archiveFullPath, &archiveStat) == -1) {
        LOG_WARN("%s: Failed to stat archive MP4 %s\n", __func__, archiveFullPath);
        unlink(archiveFullPath);
        return -1;
    }

    // Validate metadata fields before creating archive entry
    cJSON *sizeItem   = cJSON_GetObjectItem(recordingMetadata, "sizeBytes") ? cJSON_GetObjectItem(recordingMetadata, "sizeBytes") : cJSON_GetObjectItem(recordingMetadata, "size");
    cJSON *imagesItem = cJSON_GetObjectItem(recordingMetadata, "images");
    cJSON *firstItem  = cJSON_GetObjectItem(recordingMetadata, "first");
    cJSON *lastItem   = cJSON_GetObjectItem(recordingMetadata, "last");
    if (!sizeItem || !imagesItem || !fpsItem || !firstItem || !lastItem) {
        LOG_WARN("%s: Incomplete metadata for profile %s, removing archive\n", __func__, profileID);
        unlink(archiveFullPath);
        return -1;
    }

    // Create archive entry
    cJSON *recordingInfo = cJSON_CreateObject();
    cJSON_AddStringToObject(recordingInfo, "id", profileID);
    cJSON_AddStringToObject(recordingInfo, "filename", archiveFilename);
    cJSON_AddNumberToObject(recordingInfo, "size",   archiveStat.st_size);
    cJSON_AddNumberToObject(recordingInfo, "sourceSize", sizeItem->valuedouble);
    cJSON_AddNumberToObject(recordingInfo, "frames", imagesItem->valueint);
    cJSON_AddNumberToObject(recordingInfo, "fps",    fpsItem->valueint);
    cJSON_AddNumberToObject(recordingInfo, "first",  firstItem->valuedouble);
    cJSON_AddNumberToObject(recordingInfo, "last",   lastItem->valuedouble);
    cJSON_AddStringToObject(recordingInfo, "container", "mp4");
    cJSON_AddStringToObject(recordingInfo, "codec", "h264");

    // Add to archive list and save
    g_rec_mutex_lock(&archive_list_mutex);
    if (!ArchiveList) {
        load_archive_list();
    }
    cJSON_AddItemToArray(ArchiveList, recordingInfo);
    save_archive_list();
    g_rec_mutex_unlock(&archive_list_mutex);

    // Update profile archived timestamp
    if (!cJSON_GetObjectItem(profile, "archived")) {
        cJSON_AddNumberToObject(profile, "archived", ACAP_DEVICE_Timestamp());
    } else {
        cJSON_SetNumberValue(cJSON_GetObjectItem(profile, "archived"),
                            ACAP_DEVICE_Timestamp());
    }
    Timelapse_Save_Profiles();

    // Clear original recording
    Recordings_Clear(profileID);

    LOG("%s: success profile=%s file=%s size=%lld elapsed_ms=%lld\n",
        __func__, profileID, archiveFilename, (long long)archiveStat.st_size, monotonic_ms() - started_ms);
	//pthread_mutex_unlock(&recordings_mutex);
    return 0;
}

int Recordings_Delete_Archive(const char* filename) {
    if (!filename) {
        LOG_WARN("%s: Missing filename\n", __func__);
        return 0;
    }

    g_rec_mutex_lock(&archive_list_mutex);

    // Load archive list if not loaded
    if (!ArchiveList) {
        load_archive_list();
    }

    // Find and remove the entry in ArchiveList
    int found = 0;
    cJSON *newArchiveList = cJSON_CreateArray();
    cJSON *item = ArchiveList->child;
    while(item) {
        const char *id = cJSON_GetObjectItem(item, "filename")->valuestring;
        if (strcmp(id, filename) == 0) {
            found = 1;
            char filepath[PATH_MAX_LEN];
            char archive_dir[PATH_MAX_LEN];
            if (storage_archive_dir(archive_dir, sizeof(archive_dir)) &&
                storage_join(filepath, sizeof(filepath), archive_dir, filename)) {
                unlink(filepath);
            }
        } else {
            cJSON_AddItemToArray(newArchiveList, cJSON_Duplicate(item, 1));
        }
		item = item->next;
    }


    // Update and save archive list
    if (found) {
        cJSON_Delete(ArchiveList);
        ArchiveList = newArchiveList;
        save_archive_list();
    } else {
        cJSON_Delete(newArchiveList);
	}

    g_rec_mutex_unlock(&archive_list_mutex);

    return found;
}

int Recordings_Delete_Profile_Media(const char* profileId) {
    if (!profileId) {
        return 0;
    }

    g_rec_mutex_lock(&archive_list_mutex);

    if (!ArchiveList) {
        load_archive_list();
    }

    int removed_archives = 0;
    cJSON* newArchiveList = cJSON_CreateArray();
    if (!newArchiveList) {
        g_rec_mutex_unlock(&archive_list_mutex);
        return 0;
    }

    cJSON* item = ArchiveList ? ArchiveList->child : NULL;
    while (item) {
        const char* source_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char* filename = cJSON_GetStringValue(cJSON_GetObjectItem(item, "filename"));
        if (source_id && strcmp(source_id, profileId) == 0) {
            if (filename) {
                char filepath[PATH_MAX_LEN];
                char archive_dir[PATH_MAX_LEN];
                if (storage_archive_dir(archive_dir, sizeof(archive_dir)) &&
                    storage_join(filepath, sizeof(filepath), archive_dir, filename)) {
                    unlink(filepath);
                }
            }
            removed_archives++;
        } else {
            cJSON_AddItemToArray(newArchiveList, cJSON_Duplicate(item, 1));
        }
        item = item->next;
    }

    if (removed_archives > 0) {
        cJSON_Delete(ArchiveList);
        ArchiveList = newArchiveList;
        save_archive_list();
    } else {
        cJSON_Delete(newArchiveList);
    }

    g_rec_mutex_unlock(&archive_list_mutex);

    int clear_ok = Recordings_Clear(profileId) == 0;
    LOG("%s: profile=%s clear=%s removed_archives=%d\n",
        __func__, profileId, clear_ok ? "ok" : "failed", removed_archives);
    return clear_ok;
}

static void
HTTP_Endpoint_Image(const ACAP_HTTP_Response response,
                              const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    LOG_TRACE("%s: %s\n", __func__, method);

    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
        return;
    }

    const char* profileId = ACAP_HTTP_Request_Param(request, "id");
    const char* indexStr = ACAP_HTTP_Request_Param(request, "index");

    LOG_TRACE("%s: %s %s\n", __func__, profileId, indexStr);

    if (!profileId || !indexStr) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing parameters");
        return;
    }

    int index = atoi(indexStr);

    // Read frame index
    char framepath[PATH_MAX_LEN];
    if (!storage_frame_path(framepath, sizeof(framepath), profileId, (unsigned)index)) {
        ACAP_HTTP_Respond_Error(response, 500, "Failed to build frame path");
        return;
    }

    FILE* framefile = fopen(framepath, "rb");
    if (!framefile) {
        ACAP_HTTP_Respond_Error(response, 404, "Frame not found");
        return;
    }

    fseek(framefile, 0, SEEK_END);
    long frame_size = ftell(framefile);
    fseek(framefile, 0, SEEK_SET);

	char* buffer = malloc(frame_size);
	if (!buffer) {
		fclose(framefile);
		ACAP_HTTP_Respond_Error(response, 500, "Memory allocation failed");
		return;
	}

	fread(buffer, 1, frame_size, framefile);
    fclose(framefile);

    ACAP_HTTP_Respond_String(response, "Status: 200 OK\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Type: image/jpeg\r\n");
    ACAP_HTTP_Respond_String(response, "Content-Length: %ld\r\n", frame_size);
    ACAP_HTTP_Respond_String(response, "\r\n");

    int result = ACAP_HTTP_Respond_Data(response, (size_t)frame_size, buffer);
    if (result != 1) {
        LOG_WARN("%s: Failed to send image data\n", __func__);
    }

    free(buffer);
}

static void HTTP_Endpoint_Export(const ACAP_HTTP_Response response,
                               const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
	long long started_ms = monotonic_ms();
	LOG_TRACE("%s: %s\n", __func__,method);
    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
        return;
    }

    const char* profileId = ACAP_HTTP_Request_Param(request, "id");
    if (!profileId) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing parameters");
        return;
    }

    cJSON* profile = Timelapse_Find_Profile_By_Id(profileId);
    int fps = 10;
    const char* profile_name = profileId;
    if (profile) {
        cJSON* fps_item = cJSON_GetObjectItem(profile, "fps");
        cJSON* name_item = cJSON_GetObjectItem(profile, "name");
        if (fps_item && fps_item->type == cJSON_Number) {
            fps = fps_item->valueint;
        }
        if (name_item && name_item->valuestring && name_item->valuestring[0]) {
            profile_name = name_item->valuestring;
        }
    }

	if( fps < 1 ) fps = 1;
	if (fps > 60) fps = 60;
	LOG_TRACE("%s: %s fps=%d\n", __func__, profileId, fps );

    char output_path[PATH_MAX_LEN];
    if (!storage_export_path(output_path, sizeof(output_path), profileId, fps)) {
        ACAP_HTTP_Respond_Error(response, 500, "Failed to build export path");
        return;
    }

    // The frontend always does a background PUT (forces a full catch-up) + poll before this
    // GET, so the export is normally already exactly what's needed - just stream it. Only build
    // synchronously here as a fallback for a direct GET with no preceding PUT (bookmark, API
    // caller, old page). Deliberately NOT re-forcing a catch-up when the file already exists:
    // doing so would chase whatever's been captured in the meantime, which is exactly the
    // "images keep landing while the job runs" churn this avoids, per how this app's meant to
    // be used - a click either prepares (PUT) or fetches (GET), never both in one request.
    struct stat export_stat;
    int export_exists = stat(output_path, &export_stat) == 0 && S_ISREG(export_stat.st_mode) && export_stat.st_size > 0;
    if (!export_exists && !media_process_pending_force(profileId, fps)) {
        int status = media_ffmpeg_available() ? 500 : 503;
        LOG_WARN("%s: failed profile=%s fps=%d status=%d err=%s\n", __func__, profileId, fps, status, media_last_error());
        ACAP_HTTP_Respond_Error(response, status, media_last_error());
        return;
    }

    LOG("%s: generated profile=%s fps=%d path=%s elapsed_ms=%lld\n", __func__, profileId, fps, output_path, monotonic_ms() - started_ms);

    char download_name[PATH_MAX_LEN];
    build_timestamped_export_filename(download_name, sizeof(download_name), profile_name);
    stream_file_response(response, output_path, "video/mp4", "attachment", download_name);
}

static void HTTP_Endpoint_Video(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    long long started_ms = monotonic_ms();

    const char* profileId = ACAP_HTTP_Request_Param(request, "id");
    const char* fpsString = ACAP_HTTP_Request_Param(request, "fps");
    if (!profileId) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing profile id");
        return;
    }

    int fps = fpsString ? atoi(fpsString) : 10;
    if( fps < 1 ) fps = 1;
    if (fps > 60) fps = 60;

    // Queue (or refresh) the disposable preview in the background - see Queue_Preview_Media's
    // comment for why this must not block. The follow-up GET below picks up whatever it built.
    if (strcmp(method, "PUT") == 0) {
        LOG("%s: queueing preview profile=%s fps=%d\n", __func__, profileId, fps);
        if (Queue_Preview_Media(profileId, fps)) {
            ACAP_HTTP_Respond_Text(response, "Preview started");
        } else {
            ACAP_HTTP_Respond_Error(response, 500, "Failed to start preview");
        }
        return;
    }

    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
        return;
    }

    LOG("%s: request profile=%s fps=%d\n", __func__, profileId, fps);

    // Never touches the permanent export cache - see media_generate_preview()'s own comment.
    // Try fetch-only first: the frontend always does a background PUT + poll before this GET,
    // so the preview is normally already exactly what that PUT built - serve it as-is rather
    // than re-checking against a possibly-newer frame count (which is exactly the "images land
    // while the job runs" churn this avoids). Only fall back to building here for a direct GET
    // with no preceding PUT (bookmark, API caller, old page).
    char output_path[PATH_MAX_LEN];
    if (!media_generate_preview(profileId, fps, output_path, sizeof(output_path), 0) &&
        !media_generate_preview(profileId, fps, output_path, sizeof(output_path), 1)) {
        int status = media_ffmpeg_available() ? 500 : 503;
        LOG_WARN("%s: failed profile=%s fps=%d status=%d err=%s\n", __func__, profileId, fps, status, media_last_error());
        ACAP_HTTP_Respond_Error(response, status, media_last_error());
        return;
    }

    LOG("%s: generated profile=%s fps=%d path=%s elapsed_ms=%lld\n", __func__, profileId, fps, output_path, monotonic_ms() - started_ms);

    stream_file_response(response, output_path, "video/mp4", "inline", "preview.mp4");
}

// Adds an "estimatedSize" field (projected final export MP4 size) to a *duplicated*
// recording object for API responses. Never mutates the shared Recordings_Container,
// since that object gets persisted verbatim by recording_store.c on the next capture.
static void Attach_Estimated_Video_Size(cJSON* recording, const char* profileId) {
    if (!recording || !profileId) {
        return;
    }
    cJSON* framesItem = cJSON_GetObjectItem(recording, "frames");
    cJSON* fpsItem = cJSON_GetObjectItem(recording, "fps");
    int frames = framesItem ? framesItem->valueint : 0;
    int fps = (fpsItem && fpsItem->valueint > 0) ? fpsItem->valueint : 10;
    if (frames < 1) {
        return;
    }

    long long estimate = media_estimate_export_size(profileId, fps, frames);
    if (estimate < 0) {
        return;
    }

    cJSON_DeleteItemFromObject(recording, "estimatedSize");
    cJSON_AddNumberToObject(recording, "estimatedSize", (double)estimate);
}

static void
HTTP_Endpoint_Recordings(const ACAP_HTTP_Response response,
                                     const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);

	LOG_TRACE("%s: %s\n",__func__,method);

    if (strcmp(method, "GET") == 0) {
        if (!Recordings_Container) {
            load_recordings();
        }

        const char* profileId = ACAP_HTTP_Request_Param(request, "id");
        if (profileId) {
            cJSON* recording = cJSON_GetObjectItem(Recordings_Container, profileId);
            if (!recording) {
                ACAP_HTTP_Respond_Error(response, 404, "Recording not found");
                return;
            }
            cJSON* out = cJSON_Duplicate(recording, 1);
            if (!out) {
                ACAP_HTTP_Respond_JSON(response, recording);
                return;
            }
            Attach_Estimated_Video_Size(out, profileId);
            ACAP_HTTP_Respond_JSON(response, out);
            cJSON_Delete(out);
        } else {
            cJSON* out = cJSON_Duplicate(Recordings_Container, 1);
            if (!out) {
                ACAP_HTTP_Respond_JSON(response, Recordings_Container);
                return;
            }
            for (cJSON* item = out->child; item; item = item->next) {
                Attach_Estimated_Video_Size(item, item->string);
            }
            ACAP_HTTP_Respond_JSON(response, out);
            cJSON_Delete(out);
        }
        return;
    }

	if (strcmp(method, "DELETE") == 0) {
		const char* profileId = ACAP_HTTP_Request_Param(request, "id");
		if (!profileId) {
			ACAP_HTTP_Respond_Error(response, 400, "Missing profile ID");
			return;
		}

        if (Queue_Reset_Media(profileId)) {
            ACAP_HTTP_Respond_Text(response, "Recording media reset started");
		} else {
            ACAP_HTTP_Respond_Error(response, 500, "Failed to start recording media reset");
		}
		return;
	}

    if (strcmp(method, "PUT") == 0) {
        const char* profileId = ACAP_HTTP_Request_Param(request, "id");
        if (!profileId) {
            ACAP_HTTP_Respond_Error(response, 400, "Missing profile ID");
            return;
        }

        cJSON* profile = Timelapse_Find_Profile_By_Id(profileId);
        if (!profile) {
            ACAP_HTTP_Respond_Error(response, 404, "Profile not found");
            return;
        }
        cJSON* fpsItem = cJSON_GetObjectItem(profile, "fps");
        int fps = fpsItem ? fpsItem->valueint : 10;

        LOG("%s: queueing force-process of pending chunks profile=%s fps=%d\n", __func__, profileId, fps);
        // Runs on a background thread - see the Queue_Refresh_Media comment above for why this
        // request must not block on media_process_pending_force() itself.
        if (Queue_Refresh_Media(profileId, fps)) {
            ACAP_HTTP_Respond_Text(response, "Refresh started");
        } else {
            ACAP_HTTP_Respond_Error(response, 500, "Failed to start refresh");
        }
        return;
    }

    ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
}

// HTTP endpoint implementation for archive
static void HTTP_Endpoint_Archive(const ACAP_HTTP_Response response,
                                  const ACAP_HTTP_Request request) {
    const char *method = ACAP_HTTP_Get_Method(request);
	long long started_ms = monotonic_ms();
	LOG_TRACE("%s: %s\n",__func__,method);
    // Handle GET request: Provide the archive/recordings.json
    if (strcmp(method, "GET") == 0) {
        g_rec_mutex_lock(&archive_list_mutex);
        if (!ArchiveList) {
            load_archive_list();
        }
        cJSON* out = cJSON_Duplicate(ArchiveList, 1);
        g_rec_mutex_unlock(&archive_list_mutex);
        if (!out) {
            ACAP_HTTP_Respond_Error(response, 500, "Failed to read archive list");
            return;
        }
        ACAP_HTTP_Respond_JSON(response, out);
        cJSON_Delete(out);
        return;
    }

    // Handle DELETE request: Remove an entry from recordings.json and delete the file
	if (strcmp(method, "DELETE") == 0) {
		const char *filename = ACAP_HTTP_Request_Param(request, "filename");
		if (!filename) {
			ACAP_HTTP_Respond_Error(response, 400, "Missing profile filename");
			return;
		}

		if (Recordings_Delete_Archive(filename)) {
			ACAP_HTTP_Respond_Text(response, "Recording deleted");
		} else {
			ACAP_HTTP_Respond_Error(response, 404, "Recording not found");
		}
		return;
	}

    // Handle PUT request: Archive a recording by profile ID
    if (strcmp(method, "PUT") == 0) {
        const char *profileID = ACAP_HTTP_Request_Param(request, "id");
        if (!profileID) {
            ACAP_HTTP_Respond_Error(response, 400, "Missing profile ID");
            return;
        }

        LOG("%s: request profile=%s\n", __func__, profileID);

        // Run the actual (potentially slow) archive on a background thread - see the
        // Queue_Archive_Media comment above for why this can't block this request.
        if (Queue_Archive_Media(profileID)) {
            LOG("%s: queued profile=%s elapsed_ms=%lld\n", __func__, profileID, monotonic_ms() - started_ms);
            ACAP_HTTP_Respond_Text(response, "Archive started");
        } else {
            LOG_WARN("%s: failed to queue profile=%s elapsed_ms=%lld\n", __func__, profileID, monotonic_ms() - started_ms);
            ACAP_HTTP_Respond_Error(response, 500, "Failed to start archive");
        }
        return;
    }

    // If method is not GET or DELETE
    ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
}

static void HTTP_Endpoint_Download(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
        return;
    }

    const char* filename = ACAP_HTTP_Request_Param(request, "filename");
    if (!filename) {
        ACAP_HTTP_Respond_Error(response, 400, "Missing filename parameter");
        return;
    }
    const char* inline_param = ACAP_HTTP_Request_Param(request, "inline");

    char filepath[PATH_MAX_LEN];
    char archive_dir[PATH_MAX_LEN];
    if (!storage_archive_dir(archive_dir, sizeof(archive_dir)) ||
        !storage_join(filepath, sizeof(filepath), archive_dir, filename)) {
        ACAP_HTTP_Respond_Error(response, 500, "Failed to build archive path");
        return;
    }

    stream_file_response(response, filepath, "video/mp4", inline_param && strcmp(inline_param, "1") == 0 ? "inline" : "attachment", filename);
}


static void HTTP_Endpoint_Test(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method not allowed");
        return;
    }
	Retention_Cleanup();
	ACAP_HTTP_Respond_Text(response,"OK");
}

void
Recordings_Reset() {
    recording_store_reset();
    Recordings_Container = recording_store_list();
    g_rec_mutex_lock(&archive_list_mutex);
	if( ArchiveList )
		cJSON_Delete( ArchiveList );
	ArchiveList = cJSON_CreateArray();
	save_archive_list();
    g_rec_mutex_unlock(&archive_list_mutex);
}

static gboolean check_midnight(gpointer user_data) {

    static bool did_trigger_today = false;

    GDateTime *now = g_date_time_new_now_local();
    int hour = g_date_time_get_hour(now);
    int minute = g_date_time_get_minute(now);

LOG_TRACE("Midnight check: %d:%d", hour, minute);

    // If currently midnight [00:00]
    if (hour != 0 || minute != 0) {
        did_trigger_today = false;
		g_date_time_unref(now);
		return TRUE; // Keep the timeout running
	}
	LOG_TRACE("Midnight");
    if (did_trigger_today) {
		g_date_time_unref(now);
		return TRUE; // Keep the timeout running
	}

	did_trigger_today = true;

    LOG_TRACE("Archive duration check");
    cJSON* list = cJSON_CreateArray();
    cJSON *recording = Recordings_Container ? Recordings_Container->child : NULL;
    while(recording) {
        if (recording_due_for_archive(recording->string, recording, now)) {
            cJSON_AddItemToArray(list,cJSON_CreateString(recording->string));
        }
        recording = recording->next;
    }
    cJSON* profile = list ? list->child : NULL;
    while(profile) {
        LOG_TRACE("Archiving: %s", profile->valuestring);
        Recordings_Archive(profile->valuestring);
        profile = profile->next;
    }
    cJSON_Delete(list);
	Retention_Cleanup();
    g_date_time_unref(now);
	LOG_TRACE("%s: Exit",__func__);
    return TRUE; // Keep the timeout running
}

// This is what actually bounds raw-JPEG disk usage during long unattended stretches: without it,
// nothing would ever fold captured frames into the export cache (and purge the source JPEGs)
// unless someone happened to open the GUI. media_process_pending() only does real work once a
// profile has crossed INCREMENTAL_MIN_PENDING_FRAMES or gone quiet for INCREMENTAL_QUIET_WINDOW_MS
// (see should_process_incremental_update() in media.c) - most ticks are a no-op for most profiles.
static gint chunk_sweep_running = 0;

static gpointer process_chunks_thread(gpointer user_data) {
    (void)user_data;
    cJSON *recording = Recordings_Container ? Recordings_Container->child : NULL;
    int processed = 0;
    int skipped = 0;

    while (recording) {
        const char* profile_id = recording->string;
        cJSON* images = cJSON_GetObjectItem(recording, "images");
        int image_count = images ? images->valueint : 0;
        if (!profile_id || image_count < 1) {
            skipped++;
            recording = recording->next;
            continue;
        }

        cJSON* profile = Timelapse_Find_Profile_By_Id(profile_id);
        int fps = 10;
        if (profile) {
            cJSON* fps_item = cJSON_GetObjectItem(profile, "fps");
            if (fps_item && fps_item->type == cJSON_Number) {
                fps = fps_item->valueint;
            }
        }

        if (!media_process_pending(profile_id, fps)) {
            LOG_WARN("%s: skip profile=%s fps=%d err=%s\n", __func__, profile_id, fps, media_last_error());
            skipped++;
        } else {
            processed++;
        }

        recording = recording->next;
    }

    LOG("%s: done processed=%d skipped=%d\n", __func__, processed, skipped);
    g_atomic_int_set(&chunk_sweep_running, 0);
    ACAP_Background_Job_End();
    return NULL;
}

// A batch that actually crosses the (now much larger) threshold can take real time to encode -
// this must not run on the main loop thread, or it blocks capture handling for every profile
// for however long that encode takes. Runs on its own GThread instead, same reasoning as the
// Refresh/Archive background jobs above.
static gboolean process_chunks_hourly(gpointer user_data) {
    (void)user_data;
    if (g_atomic_int_compare_and_exchange(&chunk_sweep_running, 0, 1)) {
        ACAP_Background_Job_Begin();
        GThread* thread = g_thread_new("chunk-sweep", process_chunks_thread, NULL);
        if (thread) {
            g_thread_unref(thread);
        } else {
            ACAP_Background_Job_End();
            g_atomic_int_set(&chunk_sweep_running, 0);
            LOG_WARN("%s: failed to start background sweep thread\n", __func__);
        }
    } else {
        LOG_TRACE("%s: previous sweep still running, skipping this tick\n", __func__);
    }
    return TRUE;
}


int
Recordings_Init(void) {
    LOG_TRACE("%s:\n", __func__);
	recording_store_init();
    Recordings_Container = recording_store_list();
	load_archive_list();

    // Schedule retention check at midnight
	g_timeout_add_seconds(60, check_midnight, NULL);
    g_timeout_add_seconds(3600, process_chunks_hourly, NULL);


    ACAP_HTTP_Node("recordings", HTTP_Endpoint_Recordings);
    ACAP_HTTP_Node("video", HTTP_Endpoint_Video);
    ACAP_HTTP_Node("export", HTTP_Endpoint_Export);
    ACAP_HTTP_Node("archive", HTTP_Endpoint_Archive);
    ACAP_HTTP_Node("download", HTTP_Endpoint_Download);
    ACAP_HTTP_Node("test", HTTP_Endpoint_Test);

    return 0;
}
