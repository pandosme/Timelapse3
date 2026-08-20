#include "ACAP.h"
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <signal.h>
#include <poll.h>
#include <sys/resource.h>
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

static int is_x264_encoder(const char* encoder_name) {
    return encoder_name && strcmp(encoder_name, "libx264") == 0;
}

/* Every knob here costs memory in units of whole frames, so the bill scales with
   resolution. Peak RSS measured over a 60-frame 3840x2160 JPEG sequence:

     x264 defaults (frame threading)                       3.4 GB
     sliced-threads, sync-lookahead=0, rc-lookahead=10     654 MB   <- 3.1.1
     ...also rc-lookahead=2, bframes=0, ref=1              308 MB   <- now
     ...also rc-lookahead=0 (mb-tree off)                  263 MB

   Frame threading was the first cliff (each of the CPU-count x 1.5 threads keeps
   its own copy of the frames in flight); slice threading splits one frame across
   the cores instead. That alone was not enough - field logs still showed
   wait_status=9 with empty stderr, the kernel SIGKILLing the encoder. What
   remained was the lookahead queue and the B-frame pyramid, each holding more
   full-resolution buffers. Dropping to two lookahead frames keeps mb-tree (going
   to 0 turns it off and inflates the file by ~75% for the same CRF) while giving
   up most of its memory, and B-frames buy very little on timelapse content where
   consecutive frames are minutes apart anyway.

   Mid-stream this can only get cheaper, never richer: segments are joined with
   "-c copy" under the first segment's SPS, and a header promising B-frames the
   later samples do not contain is fine, while the reverse is not. */
#define X264_LOWMEM_PARAMS "sliced-threads=1:sync-lookahead=0:rc-lookahead=2:bframes=0:ref=1"

/* Linear fit of the peak RSS above against frame area (88 MB at 1280x720, 126 MB
   at 1920x1080, 308 MB at 3840x2160). Used to decide up front whether this device
   can afford to encode at the captured resolution - see pick_encode_dimensions. */
#define ENCODE_MEM_BASE_MB 70
#define ENCODE_MEM_PER_MPIXEL_MB 30
/* Left for everything else on the device while ffmpeg runs: this app, the capture
   thread's own buffers, and whatever the camera's own pipeline needs. 64 MB was not
   enough of a margin on a 1 GB camera - the device that rebooted under an encode had
   40 MB free and its swap already spent - and the cost of being conservative here is a
   smaller video, against a camera that restarts. */
#define ENCODE_MEM_HEADROOM_MB 128
/* How much of the budget an encode may use before finishing is still worth reporting.
   Below this it is routine; above it, the next encode of a slightly longer sequence on a
   slightly busier device is the one that gets stopped. */
#define ENCODE_MEM_NEAR_BUDGET_PERCENT 60
/* Floor for the automatic downscale. Below this the timelapse stops being worth
   looking at, and a video nobody wants is no better than the failed encode it replaced. */
#define ENCODE_MIN_WIDTH 640
#define ENCODE_MIN_HEIGHT 360

static long read_meminfo_kb(const char* key) {
    FILE* file = fopen("/proc/meminfo", "r");
    if (!file) {
        return 0;
    }

    size_t key_len = strlen(key);
    long value_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            value_kb = strtol(line + key_len + 1, NULL, 10);
            break;
        }
    }
    fclose(file);
    return value_kb;
}

/* How much RSS an encode may take on this device right now. MemAvailable is what
   actually decides whether the kernel reaches for the OOM killer; MemTotal only
   caps it, so one idle moment on a busy camera cannot talk us into a resolution
   the device cannot sustain. Returns 0 when /proc/meminfo says nothing, which is
   read as "no reason to scale anything down". */
static int encode_memory_budget_mb(void) {
    long available_mb = read_meminfo_kb("MemAvailable") / 1024;
    long total_mb = read_meminfo_kb("MemTotal") / 1024;
    if (available_mb <= 0) {
        return 0;
    }

    long budget_mb = available_mb - ENCODE_MEM_HEADROOM_MB;
    if (total_mb > 0 && budget_mb > total_mb / 2) {
        budget_mb = total_mb / 2;
    }
    return budget_mb > 0 ? (int)budget_mb : 1;
}

static int estimate_encode_peak_mb(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    double megapixels = ((double)width * (double)height) / 1000000.0;
    return (int)((double)ENCODE_MEM_BASE_MB + ((double)ENCODE_MEM_PER_MPIXEL_MB * megapixels) + 0.5);
}

/* Picks the resolution to encode at. Captured frames always stay untouched at full
   resolution on disk - this only decides what the assembled video is scaled to, and
   only for devices where the full-resolution encode does not fit in memory. Halving
   both dimensions quarters the per-frame cost, which is the difference between a
   video and the SIGKILL loop that shipped before. */
static void pick_encode_dimensions(int source_width, int source_height, int* out_width, int* out_height) {
    *out_width = source_width;
    *out_height = source_height;

    int budget_mb = encode_memory_budget_mb();
    if (budget_mb <= 0 || source_width <= 0 || source_height <= 0) {
        return;
    }

    int width = source_width;
    int height = source_height;
    // Two halvings is the floor: below that the video stops being worth keeping.
    for (int step = 0; step < 2; step++) {
        if (estimate_encode_peak_mb(width, height) <= budget_mb) {
            break;
        }
        int next_width = ((width / 2) / 2) * 2;
        int next_height = ((height / 2) / 2) * 2;
        if (next_width < ENCODE_MIN_WIDTH || next_height < ENCODE_MIN_HEIGHT) {
            break;
        }
        width = next_width;
        height = next_height;
    }

    if (width != source_width || height != source_height) {
        LOG_WARN("pick_encode_dimensions: scaling down source=%dx%d encode=%dx%d budget_mb=%d estimate_mb=%d\n",
                 source_width, source_height, width, height, budget_mb,
                 estimate_encode_peak_mb(source_width, source_height));
    }

    *out_width = width;
    *out_height = height;
}

/* Preview, export and archive segments get concatenated into each other with
   "-c copy" (see the merge steps below), and an MP4 carries ONE parameter set for
   the whole track. So every segment must be encoded with identical x264 settings:
   the presets differ in entropy coder and 8x8dct, which changes the H.264 profile
   (ultrafast -> Constrained Baseline/CAVLC, veryfast -> High/CABAC). Joining those
   produced a file whose header advertised High while half its samples were
   Baseline - ffmpeg still decoded it, but a browser stops at the join and waits
   forever. One setting for all three kinds is what keeps the copy-merge valid. */
#define H264_PRESET "veryfast"
#define H264_CRF "24"

/* g_spawn_sync() reports the raw wait status. A child killed by a signal leaves
   WIFEXITED false and normally writes nothing to stderr, so reporting stderr
   alone produced an empty "FFmpeg failed:" with no clue why. Name the signal -
   SIGKILL here is almost always the kernel reclaiming memory. */
static const char* describe_child_failure(gint wait_status, const char* stderr_text,
                                          char* buf, size_t buf_len) {
    if (WIFSIGNALED(wait_status)) {
        int sig = WTERMSIG(wait_status);
        snprintf(buf, buf_len, "killed by signal %d%s%s%s", sig,
                 sig == SIGKILL ? " (out of memory?)" : "",
                 (stderr_text && stderr_text[0]) ? " " : "",
                 (stderr_text && stderr_text[0]) ? stderr_text : "");
        return buf;
    }
    if (stderr_text && stderr_text[0]) {
        return stderr_text;
    }
    snprintf(buf, buf_len, "exit status %d",
             WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : wait_status);
    return buf;
}

/* Picking a resolution up front is a guess, and on these devices it was a guess that
   still ended in reboots. A field report from a 1 GB camera has an ffmpeg child at
   613 MB RSS with swap exhausted, and what died was not the child: AXIS OS watches the
   system with custodio, which pings by fork+exec'ing /usr/bin/ls every five seconds,
   so once memory pressure makes that time out the hardware watchdog fires and the whole
   camera restarts ("custodio: would have triggered OOM", then "Kernel panic - not
   syncing: watchdog pretimeout event"). The estimate that chose the resolution was off
   by 4x for that encode, and no constant tuned against a lab measurement fixes that on
   every device and every load.

   So the child is watched while it runs. It is killed by us - early, at a size the
   device can still absorb - when it grows past the budget, or when MemAvailable for the
   whole device drops toward the point where a fork starts to fail. Killing our own
   encode costs one video; letting the watchdog fire costs the camera and every other
   app on it. Every ffmpeg the app starts goes through here, not just the encoder: the
   copy, concat and re-encode passes handle the same footage and were never protected.

   Being stopped by the guard is reported like the kernel's own SIGKILL, so the callers
   that retry at a smaller resolution still do. */
#define MEM_GUARD_POLL_MS 250
/* Device-wide floor. Below this, the next fork+exec on the device is at risk - which is
   the failure that takes the camera down rather than just the encode. */
#define MEM_GUARD_MIN_AVAILABLE_MB 128
/* One sample below the floor can be a page-cache trough while a large file is written;
   two in a row is the device actually running out. */
#define MEM_GUARD_LOW_SAMPLES 2
/* An ffmpeg error is a line or two; anything past this is a stuck loop repeating itself
   and there is no reason to hold it all in memory - least of all in this app, while the
   device is short of memory. */
#define FFMPEG_STDERR_MAX 8192

typedef struct {
    gint wait_status;    // raw wait status, as g_spawn_sync would have reported it
    gchar* stderr_text;  // owned by the caller, free with g_free
    int guard_killed;    // the guard stopped it, the kernel did not
    int peak_rss_mb;     // VmHWM of the child, 0 when it could not be read
} FfmpegRun;

static void free_ffmpeg_run(FfmpegRun* run) {
    if (run && run->stderr_text) {
        g_free(run->stderr_text);
        run->stderr_text = NULL;
    }
}

static int ffmpeg_run_succeeded(const FfmpegRun* run) {
    return run && WIFEXITED(run->wait_status) && WEXITSTATUS(run->wait_status) == 0;
}

static long read_proc_status_kb(pid_t pid, const char* key) {
    char path[64];
    if (snprintf(path, sizeof(path), "/proc/%d/status", (int)pid) <= 0) {
        return 0;
    }

    FILE* file = fopen(path, "r");
    if (!file) {
        return 0;
    }

    size_t key_len = strlen(key);
    long value_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            value_kb = strtol(line + key_len + 1, NULL, 10);
            break;
        }
    }
    fclose(file);
    return value_kb;
}

/* Runs between fork and exec, so only async-signal-safe calls belong here. The address
   space cap is a backstop for what the 250 ms poll cannot see: an allocation burst that
   starts and finishes between two samples. It is deliberately loose - the field report
   above shows 1.0 GB of address space behind 613 MB of RSS, mostly thread stacks and
   allocator arenas that are never faulted in - because a cap that bites a healthy encode
   costs a video for nothing. */
typedef struct {
    rlim_t address_space_bytes;
} ChildLimits;

static void apply_child_limits(gpointer user_data) {
    const ChildLimits* limits = (const ChildLimits*)user_data;
    if (!limits || limits->address_space_bytes == 0) {
        return;
    }
    struct rlimit limit = { limits->address_space_bytes, limits->address_space_bytes };
    setrlimit(RLIMIT_AS, &limit);
}

/* One sample of a running child against what the device can still spare. Returns non-zero
   when this child has to go: either it is over the budget its resolution was chosen for,
   or the device as a whole is close enough to empty that the next fork could fail.
   low_samples and peak_kb carry state across calls and belong to one child. */
static int memory_guard_should_stop(pid_t pid, int budget_mb, const char* what,
                                    int* low_samples, long* peak_kb) {
    long rss_kb = read_proc_status_kb(pid, "VmRSS");
    long hwm_kb = read_proc_status_kb(pid, "VmHWM");
    if (peak_kb && hwm_kb > *peak_kb) {
        *peak_kb = hwm_kb;
    }

    long available_mb = read_meminfo_kb("MemAvailable") / 1024;
    int over_budget = budget_mb > 0 && (rss_kb / 1024) > budget_mb;
    int device_low = available_mb > 0 && available_mb < MEM_GUARD_MIN_AVAILABLE_MB;
    if (low_samples) {
        *low_samples = device_low ? *low_samples + 1 : 0;
    }

    int sustained_low = low_samples && *low_samples >= MEM_GUARD_LOW_SAMPLES;
    if (!over_budget && !sustained_low) {
        return 0;
    }

    LOG_WARN("%s: stopping %s before the device runs out reason=%s rss_mb=%ld peak_mb=%ld budget_mb=%d available_mb=%ld\n",
             __func__, what ? what : "ffmpeg", over_budget ? "over-budget" : "device-low",
             rss_kb / 1024, peak_kb ? *peak_kb / 1024 : 0, budget_mb, available_mb);
    return 1;
}

/* Drop-in for g_spawn_sync() with the memory guard attached. budget_mb <= 0 watches the
   device floor only, which is what the short metadata passes want - they have no
   resolution to scale down and no budget of their own. */
static gboolean run_ffmpeg_guarded(char** argv, int budget_mb, const char* what,
                                   FfmpegRun* run, GError** error) {
    memset(run, 0, sizeof(*run));

    ChildLimits limits = { 0 };
    if (budget_mb > 0) {
        limits.address_space_bytes = (rlim_t)budget_mb * 2 * 1024 * 1024;
    }

    GPid child_pid = 0;
    gint stderr_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
                                  apply_child_limits, &limits, &child_pid,
                                  NULL, NULL, &stderr_fd, error)) {
        return FALSE;
    }

    GString* stderr_buffer = g_string_new(NULL);
    long peak_rss_kb = 0;
    int low_memory_samples = 0;
    int reaped = 0;
    gint wait_status = 0;

    while (!reaped || stderr_fd >= 0) {
        if (stderr_fd >= 0) {
            struct pollfd poll_fd = { stderr_fd, POLLIN, 0 };
            if (poll(&poll_fd, 1, MEM_GUARD_POLL_MS) > 0) {
                char buffer[512];
                ssize_t got = read(stderr_fd, buffer, sizeof(buffer));
                if (got > 0) {
                    if (stderr_buffer->len < FFMPEG_STDERR_MAX) {
                        g_string_append_len(stderr_buffer, buffer, got);
                    }
                } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
                    close(stderr_fd);
                    stderr_fd = -1;
                }
            }
        } else {
            struct timespec nap = { 0, MEM_GUARD_POLL_MS * 1000000L };
            nanosleep(&nap, NULL);
        }

        if (reaped) {
            continue;
        }

        if (memory_guard_should_stop((pid_t)child_pid, budget_mb, what,
                                     &low_memory_samples, &peak_rss_kb) && !run->guard_killed) {
            run->guard_killed = 1;
            kill((pid_t)child_pid, SIGKILL);
        }

        pid_t finished = waitpid((pid_t)child_pid, &wait_status, WNOHANG);
        if (finished == (pid_t)child_pid) {
            reaped = 1;
        } else if (finished < 0 && errno != EINTR) {
            wait_status = -1;
            reaped = 1;
        }
    }

    if (stderr_fd >= 0) {
        close(stderr_fd);
    }
    g_spawn_close_pid(child_pid);

    run->wait_status = wait_status;
    run->peak_rss_mb = (int)(peak_rss_kb / 1024);
    run->stderr_text = g_string_free(stderr_buffer, FALSE);
    return TRUE;
}

/* Same job as describe_child_failure, plus the one failure the kernel never reports
   because it never happened: the guard got there first. */
static const char* describe_ffmpeg_failure(const FfmpegRun* run, int budget_mb,
                                           char* buf, size_t buf_len) {
    if (run->guard_killed) {
        snprintf(buf, buf_len, "stopped to protect device memory (peak %d MB, budget %d MB)",
                 run->peak_rss_mb, budget_mb);
        return buf;
    }
    return describe_child_failure(run->wait_status, run->stderr_text, buf, buf_len);
}

/* The v4l2 encoder device either exists on this product or it never will, so
   probe it once per process. Retrying it on every job cost ~1s and pushed the
   same 20-line failure blob into syslog each time. */
static gint hw_encoder_unavailable = 0;

static int hw_encoder_missing_device(const char* stderr_text) {
    return stderr_text && strstr(stderr_text, "Could not find a valid device") != NULL;
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

/* The resolution a profile's videos are encoded at, decided once and then reused by
   preview, export and archive alike. It has to be one answer per profile: those three
   are joined with "-c copy", and a resolution change part way through a track produces
   a file that plays until the join and then stops. Stored next to the videos so it also
   survives a restart mid-recording. */
static int encode_dims_path(char* out, size_t out_len, const char* profile_id) {
    char cache_dir[1024];
    if (!storage_cache_dir(cache_dir, sizeof(cache_dir), profile_id)) {
        return 0;
    }
    return storage_join(out, out_len, cache_dir, "encode.json");
}

static int load_encode_dims(const char* profile_id, int* width, int* height) {
    char path[1200];
    if (!encode_dims_path(path, sizeof(path), profile_id)) {
        return 0;
    }

    cJSON* root = read_json_file(path);
    if (!root) {
        return 0;
    }

    cJSON* width_item = cJSON_GetObjectItem(root, "width");
    cJSON* height_item = cJSON_GetObjectItem(root, "height");
    int ok = width_item && height_item && width_item->valueint > 0 && height_item->valueint > 0;
    if (ok) {
        *width = width_item->valueint;
        *height = height_item->valueint;
    }
    cJSON_Delete(root);
    return ok;
}

static int save_encode_dims(const char* profile_id, int width, int height) {
    char path[1200];
    char temp_path[1208];
    if (width <= 0 || height <= 0 || !encode_dims_path(path, sizeof(path), profile_id)) {
        return 0;
    }
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
        return 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "profileId", profile_id);
    cJSON_AddNumberToObject(root, "width", width);
    cJSON_AddNumberToObject(root, "height", height);

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

/* Resolves the encode resolution for one job. "locked" means something already
   encoded exists that this job's output will be concatenated onto, so its resolution
   is not ours to change - only a video being started from scratch gets to pick. */
static int resolve_encode_scale(const char* profile_id, const RecordingSnapshot* snapshot,
                                int locked, char* scale_arg, size_t scale_arg_len) {
    int width = 0;
    int height = 0;

    if (!load_encode_dims(profile_id, &width, &height)) {
        if (locked) {
            width = snapshot->width;
            height = snapshot->height;
        } else {
            pick_encode_dimensions(snapshot->width, snapshot->height, &width, &height);
        }
        save_encode_dims(profile_id, width, height);
    }

    if (width <= 0 || height <= 0 || snapshot->width <= 0 || snapshot->height <= 0) {
        return 0;
    }
    if (width == snapshot->width && height == snapshot->height) {
        return 0;
    }

    int written = snprintf(scale_arg, scale_arg_len, "scale=%d:%d", width, height);
    return written > 0 && (size_t)written < scale_arg_len;
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
    /* A copy: this runs on the media worker threads while capture keeps mutating
       the live recording tree on the GLib main thread. */
    cJSON* recording = recording_store_get_copy(profile_id);
    cJSON* frames = recording ? cJSON_GetObjectItem(recording, "frames") : NULL;
    if (!frames || frames->valueint < 1) {
        set_last_error("Recording has no frames for %s", profile_id);
        cJSON_Delete(recording);
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
    cJSON_Delete(recording);
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

/* Halves the persisted encode size one more step, for when the device turns out to be
   tighter than estimate_encode_peak_mb() predicted and the encoder was SIGKILLed anyway.
   Only ever called for a video being built from scratch, so no already-encoded segment
   can end up joined to one at a different resolution. */
static int halve_encode_dims(const char* profile_id, char* scale_arg, size_t scale_arg_len, int* use_scale) {
    int width = 0;
    int height = 0;
    if (!load_encode_dims(profile_id, &width, &height) || width <= 0 || height <= 0) {
        return 0;
    }

    int next_width = ((width / 2) / 2) * 2;
    int next_height = ((height / 2) / 2) * 2;
    if (next_width < ENCODE_MIN_WIDTH || next_height < ENCODE_MIN_HEIGHT) {
        return 0;
    }
    if (!save_encode_dims(profile_id, next_width, next_height)) {
        return 0;
    }

    int written = snprintf(scale_arg, scale_arg_len, "scale=%d:%d", next_width, next_height);
    if (written <= 0 || (size_t)written >= scale_arg_len) {
        return 0;
    }
    *use_scale = 1;
    return 1;
}

/* Copies an already-encoded video to a new path without re-encoding. Used when an
   archive covers a span whose raw frames have already been folded into the export
   video and deleted - the pixels still exist, just in H.264 form. */
static int remux_copy_video(const char* input_path, const char* output_path) {
    char temp_path[1200];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.copy.tmp", output_path);
    if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
        set_last_error("Archive copy path is too long for %s", output_path);
        return 0;
    }
    unlink(temp_path);

    const char* ffmpeg_exec = resolve_ffmpeg_path();
    char* argv[] = {
        (char*)ffmpeg_exec,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        (char*)input_path,
        "-c",
        "copy",
        "-movflags",
        "+faststart",
        "-f",
        "mp4",
        temp_path,
        NULL
    };

    GError* error = NULL;
    FfmpegRun run;
    int budget_mb = encode_memory_budget_mb();
    gboolean ok = run_ffmpeg_guarded(argv, budget_mb, "archive copy", &run, &error);
    if (!ok || !ffmpeg_run_succeeded(&run)) {
        char failure_buf[512];
        set_last_error("Archive copy failed: %s",
                       !ok ? (error ? error->message : "spawn failed")
                           : describe_ffmpeg_failure(&run, budget_mb, failure_buf, sizeof(failure_buf)));
        if (error) {
            g_error_free(error);
        }
        free_ffmpeg_run(&run);
        unlink(temp_path);
        return 0;
    }

    if (error) {
        g_error_free(error);
    }
    free_ffmpeg_run(&run);

    if (rename(temp_path, output_path) == -1) {
        set_last_error("Failed to finalize archive copy: %s", strerror(errno));
        unlink(temp_path);
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
            /* No raw frames left at all. For an archive that is the normal end state of a
               profile whose frames have all been folded into the export video already
               (which is what a midnight archive of a busy profile looks like): the whole
               recording lives in that video, so copy it instead of reporting an empty
               recording and losing the day's archive. */
            if (kind == MEDIA_ARCHIVE &&
                storage_export_path(export_base_path, sizeof(export_base_path), profile_id, fps) &&
                file_exists_nonempty(export_base_path)) {
                int copied = remux_copy_video(export_base_path, output_path);
                LOG("%s: archived from export video kind=%s profile=%s source=%s ok=%d elapsed_ms=%lld\n",
                    __func__, media_kind_name(kind), profile_id, export_base_path, copied,
                    monotonic_ms() - started_ms);
                if (media_job) {
                    set_media_encode_status_done(copied, copied ? "Archive ready" : media_last_error());
                }
                if (copied) {
                    last_error[0] = '\0';
                }
                g_mutex_unlock(&encode_mutex);
                return copied;
            }

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

    const char* preset = H264_PRESET;
    const char* crf = H264_CRF;

    /* Fixed frame-count GOP (not fps-scaled); sc_threshold=0 stops libx264 inserting extra
       keyframes on scene cuts. Together these keep the keyframe cadence stable regardless of
       capture interval or playback fps, instead of short/irregular or fps-dependent GOPs. */
    char gop_arg[16];
    snprintf(gop_arg, sizeof(gop_arg), "%d", EXPORT_GOP_FRAMES);

    unlink(temp_path);
    const char* ffmpeg_exec = resolve_ffmpeg_path();

    /* Locked once anything this segment will be concatenated onto already exists:
       the tail of an export/preview being extended, or an archive that has to be
       prefixed with the export video because its early frames were purged. */
    int scale_locked = finalized_frames > 0 || export_base_path[0] != '\0';
    char scale_arg[64];
    int use_scale = resolve_encode_scale(profile_id, &snapshot, scale_locked, scale_arg, sizeof(scale_arg));

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
    int encode_killed = 0;
    int oom_retry_used = 0;
    char final_error[512];
    final_error[0] = '\0';

    /* Only the last candidate failing is a real failure. Everything before it is
       the fallback chain doing its job - h264_v4l2m2m is simply absent on some
       devices, and warning about it on every export buried the actual errors. */

retry_encode:
    for (int i = 0; i < candidate_count; i++) {
        const char* encoder = encoder_candidates[i];
        int use_hw = is_v4l2m2m_encoder(encoder);
        int use_faststart = kind != MEDIA_ARCHIVE;

        // Known to have no device on this product, and a fallback is queued behind it.
        if (use_hw && i + 1 < candidate_count && g_atomic_int_get(&hw_encoder_unavailable)) {
            continue;
        }

        char* argv[48];
        int argc = 0;
        argv[argc++] = (char*)ffmpeg_exec;
        argv[argc++] = "-y";
        argv[argc++] = "-hide_banner";
        argv[argc++] = "-loglevel";
        argv[argc++] = "error";
        argv[argc++] = "-framerate";
        argv[argc++] = fps_arg;
        argv[argc++] = "-start_number";
        argv[argc++] = start_number_arg;
        argv[argc++] = "-i";
        argv[argc++] = input_pattern;
        argv[argc++] = "-frames:v";
        argv[argc++] = frame_count_arg;
        if (use_scale) {
            argv[argc++] = "-vf";
            argv[argc++] = scale_arg;
        }
        argv[argc++] = "-c:v";
        argv[argc++] = (char*)encoder;
        if (use_hw) {
            argv[argc++] = "-pix_fmt";
            argv[argc++] = "nv12";
            argv[argc++] = "-b:v";
            argv[argc++] = "4M";
            argv[argc++] = "-g";
            argv[argc++] = gop_arg;
        } else {
            argv[argc++] = "-preset";
            argv[argc++] = (char*)preset;
            argv[argc++] = "-crf";
            argv[argc++] = (char*)crf;
            argv[argc++] = "-g";
            argv[argc++] = gop_arg;
            argv[argc++] = "-keyint_min";
            argv[argc++] = gop_arg;
            argv[argc++] = "-sc_threshold";
            argv[argc++] = "0";
            argv[argc++] = "-pix_fmt";
            argv[argc++] = "yuv420p";
            if (is_x264_encoder(encoder)) {
                argv[argc++] = "-x264-params";
                argv[argc++] = X264_LOWMEM_PARAMS;
            }
        }
        if (use_faststart) {
            argv[argc++] = "-movflags";
            argv[argc++] = "+faststart";
        }
        argv[argc++] = "-f";
        argv[argc++] = "mp4";
        argv[argc++] = temp_path;
        argv[argc] = NULL;

        unlink(temp_path);

        GError* error = NULL;
        FfmpegRun run;
        int budget_mb = encode_memory_budget_mb();
        long long ffmpeg_started_ms = monotonic_ms();
        LOG("%s: ffmpeg try kind=%s profile=%s encoder=%s input=%s output=%s fps=%s start=%s frames=%s preset=%s crf=%s gop=%s faststart=%s scale=%s mem_budget_mb=%d\n",
            __func__, media_kind_name(kind), profile_id, encoder, input_pattern, output_path,
            fps_arg, start_number_arg, frame_count_arg, preset, crf, gop_arg, use_faststart ? "on" : "off",
            use_scale ? scale_arg : "native", budget_mb);

        gboolean ok = run_ffmpeg_guarded(argv, budget_mb, "encode", &run, &error);
        int last_candidate = (i + 1 == candidate_count);
        if (!ok) {
            snprintf(final_error, sizeof(final_error), "spawn failed: %s", error ? error->message : "unknown error");
            if (last_candidate) {
                LOG_WARN("%s: ffmpeg spawn failed kind=%s profile=%s encoder=%s elapsed_ms=%lld err=%s\n",
                         __func__, media_kind_name(kind), profile_id, encoder,
                         monotonic_ms() - ffmpeg_started_ms, error ? error->message : "unknown error");
            } else {
                LOG("%s: encoder %s unavailable, falling back: %s\n",
                    __func__, encoder, error ? error->message : "unknown error");
            }
            if (error) {
                g_error_free(error);
            }
            free_ffmpeg_run(&run);
            continue;
        }

        if (!ffmpeg_run_succeeded(&run)) {
            char failure_buf[512];
            const char* failure = describe_ffmpeg_failure(&run, budget_mb, failure_buf, sizeof(failure_buf));
            snprintf(final_error, sizeof(final_error), "%s", failure);
            /* Both endings mean the same thing to the retry below: this resolution does
               not fit on this device. The guard version is the one we want to see - it
               got there before the kernel had to choose a victim. */
            encode_killed = run.guard_killed ||
                            (WIFSIGNALED(run.wait_status) && WTERMSIG(run.wait_status) == SIGKILL);
            if (use_hw && hw_encoder_missing_device(run.stderr_text)) {
                g_atomic_int_set(&hw_encoder_unavailable, 1);
                LOG("%s: encoder %s has no device on this product; skipping it from now on\n",
                    __func__, encoder);
            }
            if (last_candidate) {
                LOG_WARN("%s: ffmpeg failed kind=%s profile=%s encoder=%s wait_status=%d peak_rss_mb=%d elapsed_ms=%lld err=%s\n",
                         __func__, media_kind_name(kind), profile_id, encoder, run.wait_status,
                         run.peak_rss_mb, monotonic_ms() - ffmpeg_started_ms, failure);
            } else {
                LOG("%s: encoder %s failed, falling back to next candidate\n", __func__, encoder);
            }
            free_ffmpeg_run(&run);
            continue;
        }

        encode_ok = 1;
        /* The measured peak, logged on every success: the numbers this app scales by were
           fitted to a handful of lab encodes, and this is the only way the real ones from
           real devices ever come back. */
        LOG("%s: ffmpeg success kind=%s profile=%s encoder=%s peak_rss_mb=%d budget_mb=%d elapsed_ms=%lld\n",
            __func__, media_kind_name(kind), profile_id, encoder, run.peak_rss_mb, budget_mb,
            monotonic_ms() - ffmpeg_started_ms);
        /* An encode that finished but came close is the warning shot before the one that
           does not, and it is the only version of this number that leaves the camera:
           these devices forward warnings and above to the syslog server, so an info line
           is visible on the device alone. */
        if (budget_mb > 0 && run.peak_rss_mb > (budget_mb * ENCODE_MEM_NEAR_BUDGET_PERCENT) / 100) {
            LOG_WARN("%s: encode came close to the memory budget kind=%s profile=%s peak_rss_mb=%d budget_mb=%d scale=%s\n",
                     __func__, media_kind_name(kind), profile_id, run.peak_rss_mb, budget_mb,
                     use_scale ? scale_arg : "native");
        }
        free_ffmpeg_run(&run);
        break;
    }

    /* Killed - by the guard above, or by the kernel when it got there first - means the
       memory an encode needs at this frame size does not fit on this device. The estimate
       that picked the resolution is a model, not a measurement of this device under this
       load, so when it is wrong, halve and try once more rather than handing the user a
       failure they cannot act on. */
    if (!encode_ok && encode_killed && !oom_retry_used && !scale_locked &&
        halve_encode_dims(profile_id, scale_arg, sizeof(scale_arg), &use_scale)) {
        oom_retry_used = 1;
        encode_killed = 0;
        LOG_WARN("%s: encoder was killed, retrying smaller kind=%s profile=%s scale=%s err=%s\n",
                 __func__, media_kind_name(kind), profile_id, scale_arg, final_error);
        goto retry_encode;
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
        FfmpegRun concat_run;
        int concat_budget_mb = encode_memory_budget_mb();
        gboolean concat_ok = run_ffmpeg_guarded(concat_argv, concat_budget_mb, "merge",
                                                &concat_run, &concat_error);
        unlink(concat_list_path);
        unlink(temp_path);

        if (!concat_ok || !ffmpeg_run_succeeded(&concat_run)) {
            char concat_failure_buf[512];
            const char* concat_failure = !concat_ok
                ? (concat_error ? concat_error->message : "spawn failed")
                : describe_ffmpeg_failure(&concat_run, concat_budget_mb,
                                          concat_failure_buf, sizeof(concat_failure_buf));
            if (media_job) {
                set_media_encode_status_done(0, "Video merge failed");
            }
            set_last_error("FFmpeg concat failed: %s", concat_failure);
            if (concat_error) {
                g_error_free(concat_error);
            }
            free_ffmpeg_run(&concat_run);
            unlink(concat_output_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }

        if (concat_error) {
            g_error_free(concat_error);
        }
        free_ffmpeg_run(&concat_run);

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

    char gop_arg[16];
    snprintf(gop_arg, sizeof(gop_arg), "%d", EXPORT_GOP_FRAMES);

    const char* ffmpeg_exec = resolve_ffmpeg_path();

    // Same resolution the profile's other videos use - this tail gets concatenated onto them.
    char scale_arg[64];
    int use_scale = resolve_encode_scale(profile_id, &snapshot, base_frames > 0, scale_arg, sizeof(scale_arg));

    char* tail_argv[40];
    int tail_argc = 0;
    tail_argv[tail_argc++] = (char*)ffmpeg_exec;
    tail_argv[tail_argc++] = "-y";
    tail_argv[tail_argc++] = "-hide_banner";
    tail_argv[tail_argc++] = "-loglevel";
    tail_argv[tail_argc++] = "error";
    tail_argv[tail_argc++] = "-framerate";
    tail_argv[tail_argc++] = fps_arg;
    tail_argv[tail_argc++] = "-start_number";
    tail_argv[tail_argc++] = start_number_arg;
    tail_argv[tail_argc++] = "-i";
    tail_argv[tail_argc++] = input_pattern;
    tail_argv[tail_argc++] = "-frames:v";
    tail_argv[tail_argc++] = frame_count_arg;
    if (use_scale) {
        tail_argv[tail_argc++] = "-vf";
        tail_argv[tail_argc++] = scale_arg;
    }
    tail_argv[tail_argc++] = "-c:v";
    tail_argv[tail_argc++] = "libx264";
    tail_argv[tail_argc++] = "-preset";
    tail_argv[tail_argc++] = H264_PRESET;
    tail_argv[tail_argc++] = "-crf";
    tail_argv[tail_argc++] = H264_CRF;
    tail_argv[tail_argc++] = "-g";
    tail_argv[tail_argc++] = gop_arg;
    tail_argv[tail_argc++] = "-keyint_min";
    tail_argv[tail_argc++] = gop_arg;
    tail_argv[tail_argc++] = "-sc_threshold";
    tail_argv[tail_argc++] = "0";
    tail_argv[tail_argc++] = "-pix_fmt";
    tail_argv[tail_argc++] = "yuv420p";
    tail_argv[tail_argc++] = "-x264-params";
    tail_argv[tail_argc++] = X264_LOWMEM_PARAMS;
    /* The tail becomes preview.mp4 verbatim when there is no base to merge
       with (first play of a profile). Without faststart that file carries its
       moov atom at the end, so the browser cannot start playing until it has
       pulled the whole thing. The merge path re-applies faststart anyway. */
    tail_argv[tail_argc++] = "-movflags";
    tail_argv[tail_argc++] = "+faststart";
    tail_argv[tail_argc++] = "-f";
    tail_argv[tail_argc++] = "mp4";
    tail_argv[tail_argc++] = tail_path;
    tail_argv[tail_argc] = NULL;

    GError* error = NULL;
    FfmpegRun run;
    int budget_mb = encode_memory_budget_mb();
    long long tail_started_ms = monotonic_ms();
    LOG("%s: tail encode profile=%s start=%s frames=%s scale=%s mem_budget_mb=%d\n", __func__, profile_id,
        start_number_arg, frame_count_arg, use_scale ? scale_arg : "native", budget_mb);
    gboolean ok = run_ffmpeg_guarded(tail_argv, budget_mb, "preview encode", &run, &error);
    if (!ok || !ffmpeg_run_succeeded(&run)) {
        char failure_buf[512];
        set_last_error("Preview encode failed: %s",
                       !ok ? (error ? error->message : "spawn failed")
                           : describe_ffmpeg_failure(&run, budget_mb, failure_buf, sizeof(failure_buf)));
        LOG_WARN("%s: tail encode failed profile=%s err=%s\n", __func__, profile_id, last_error);
        if (error) {
            g_error_free(error);
        }
        free_ffmpeg_run(&run);
        unlink(tail_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }
    if (error) {
        g_error_free(error);
    }
    LOG("%s: tail encode done profile=%s peak_rss_mb=%d elapsed_ms=%lld\n", __func__, profile_id,
        run.peak_rss_mb, monotonic_ms() - tail_started_ms);
    free_ffmpeg_run(&run);

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
        FfmpegRun concat_run;
        int concat_budget_mb = encode_memory_budget_mb();
        gboolean concat_ok = run_ffmpeg_guarded(concat_argv, concat_budget_mb, "merge",
                                                &concat_run, &concat_error);
        unlink(concat_list_path);
        unlink(tail_path);

        if (!concat_ok || !ffmpeg_run_succeeded(&concat_run)) {
            char concat_failure_buf[512];
            set_last_error("Preview merge failed: %s",
                           !concat_ok ? (concat_error ? concat_error->message : "spawn failed")
                                      : describe_ffmpeg_failure(&concat_run, concat_budget_mb,
                                                                concat_failure_buf, sizeof(concat_failure_buf)));
            if (concat_error) {
                g_error_free(concat_error);
            }
            free_ffmpeg_run(&concat_run);
            unlink(concat_output_path);
            g_mutex_unlock(&encode_mutex);
            return 0;
        }
        if (concat_error) {
            g_error_free(concat_error);
        }
        free_ffmpeg_run(&concat_run);

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
        /* Only the header is wanted here. Without "-c copy" this decoded every frame of
           the file - minutes of CPU and a decoder's worth of memory, on a device that has
           neither to spare - to learn a number ffmpeg prints before the first frame. */
        "-c",
        "copy",
        "-f",
        "null",
        "-",
        NULL
    };

    GError* spawn_error = NULL;
    FfmpegRun run;
    gboolean ok = run_ffmpeg_guarded(argv, 0, "duration probe", &run, &spawn_error);
    if (spawn_error) {
        g_error_free(spawn_error);
    }

    gchar* stderr_text = run.stderr_text;
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

    free_ffmpeg_run(&run);
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
        H264_PRESET,
        "-crf",
        H264_CRF,
        "-pix_fmt",
        "yuv420p",
        "-x264-params",
        X264_LOWMEM_PARAMS,
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
    /* Same guard as every other ffmpeg here, sampled off the progress lines this one
       already reads rather than a poll loop of its own. */
    int budget_mb = encode_memory_budget_mb();
    int low_memory_samples = 0;
    long peak_rss_kb = 0;
    int guard_killed = 0;
    ChildLimits limits = { budget_mb > 0 ? (rlim_t)budget_mb * 2 * 1024 * 1024 : 0 };
    gboolean ok = g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                                          apply_child_limits, &limits,
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
            if (memory_guard_should_stop(child_pid, budget_mb, "AVI migration",
                                         &low_memory_samples, &peak_rss_kb)) {
                guard_killed = 1;
                kill(child_pid, SIGKILL);
                break;
            }
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

    if (guard_killed) {
        char guard_detail[128];
        snprintf(guard_detail, sizeof(guard_detail), "peak %d MB, budget %d MB",
                 (int)(peak_rss_kb / 1024), budget_mb);
        set_last_error("AVI migration stopped to protect device memory (%s)", guard_detail);
        if (spawn_error) {
            g_error_free(spawn_error);
        }
        g_free(stderr_text);
        unlink(temp_path);
        g_mutex_unlock(&encode_mutex);
        return 0;
    }

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
        char failure_buf[512];
        set_last_error("FFmpeg AVI migration failed: %s",
                       !ok ? (spawn_error ? spawn_error->message : "spawn failed")
                           : describe_child_failure(wait_status, stderr_text,
                                                    failure_buf, sizeof(failure_buf)));
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
        H264_PRESET,
        "-crf",
        H264_CRF,
        "-g",
        gop_arg,
        "-keyint_min",
        gop_arg,
        "-sc_threshold",
        "0",
        "-pix_fmt",
        "yuv420p",
        "-x264-params",
        X264_LOWMEM_PARAMS,
        "-movflags",
        "+faststart",
        "-f",
        "mp4",
        new_temp,
        NULL
    };

    GError* error = NULL;
    FfmpegRun run;
    /* This one both decodes and encodes the whole export, at whatever resolution it was
       written in, so it is the heaviest thing the app runs - and until now the only heavy
       one with nothing watching it. */
    int budget_mb = encode_memory_budget_mb();
    gboolean ok = run_ffmpeg_guarded(argv, budget_mb, "fps re-encode", &run, &error);

    if (!ok || !ffmpeg_run_succeeded(&run)) {
        char failure_buf[512];
        set_reencode_status_done(0, "Re-encode failed");
        set_last_error("Failed to re-encode export: %s",
                       !ok ? (error ? error->message : "spawn failed")
                           : describe_ffmpeg_failure(&run, budget_mb, failure_buf, sizeof(failure_buf)));
        if (error) {
            g_error_free(error);
        }
        free_ffmpeg_run(&run);
        unlink(new_temp);
        return 0;
    }

    if (error) {
        g_error_free(error);
    }
    free_ffmpeg_run(&run);

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