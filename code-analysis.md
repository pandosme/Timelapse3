# Concurrency and Thread-Safety Review

Date: 2026-08-21

## Scope and Method

This is a static review of the C application in `app/`, focused on shared state, worker lifecycle, shutdown, lock ordering, file-system races, and child-process handling. It does not include a camera runtime test or an ACAP SDK build.

The application has at least three concurrent execution contexts:

- the GLib main-context thread, which runs timers, scheduled captures, and callbacks;
- the single FastCGI HTTP thread;
- detached GLib workers for media generation, reset, migration, re-encoding, and periodic processing.

No ordinary mutex lock inversion was found. The most serious issues instead come from operations protected by different locks, detached workers outliving shared state, and file deletion occurring without coordination with capture or ffmpeg.

## Executive Summary

| Severity | Count | Main risks |
| --- | ---: | --- |
| Critical | 3 | lost captures, deletion beneath active workers, shutdown use-after-free |
| High | 6 | export corruption, cJSON races, duplicate jobs, unsafe cancellation |
| Medium | 6 | deadlock, stale cache metadata, formal data races, misleading status |
| Low / residual | 4 | initialization races and unsafe APIs with limited current exposure |

## Critical Findings

### 1. Archive can delete captures that were never archived

**Evidence:** Archive takes a metadata snapshot in [app/recordings.c](app/recordings.c#L1225), generates media from that snapshot at [app/recordings.c](app/recordings.c#L1269), and clears the entire live recording at [app/recordings.c](app/recordings.c#L1326). Capture writes new frames while holding only `recording_mutex` in [app/recording_store.c](app/recording_store.c#L148). The long archive encode is serialized by `encode_mutex`, but capture does not use that mutex.

**Interleaving:**

1. Archive snapshots frame count $N$.
2. ffmpeg begins encoding those $N$ frames.
3. A scheduled capture writes frame $N+1$ and updates `recordings.json`.
4. Archive succeeds and `Recordings_Clear()` removes the profile directory and recording entry.

Frame $N+1$ was not in the archive and is permanently lost.

**Recommended fix:** Atomically rotate the active recording generation under `recording_mutex`: move the current frame/cache generation to an archive staging area and immediately create a fresh active generation for new captures. Encode and clear only the staged generation. A less desirable alternative is to pause capture for the whole archive operation.

### 2. Reset and clear can delete files beneath capture, ffmpeg, or migration

**Evidence:** Global reset removes storage first in [app/main.c](app/main.c#L73), then resets profile and recording state in [app/main.c](app/main.c#L79). Per-profile clear recursively removes the profile directory under `recording_mutex` in [app/recording_store.c](app/recording_store.c#L237), while normal media readers use the unrelated `encode_mutex`. The reset endpoint is registered before delayed storage initialization in [app/main.c](app/main.c#L177), so reset can also overlap startup initialization.

**Interleaving:** A preview, export, archive, migration, or FPS re-encode is reading an input or writing a temporary output when HTTP reset or a profile clear removes its directory. Capture can similarly recreate or write into paths while global reset is traversing them.

**Impact:** Failed or corrupt media, partial metadata, lost captures, and inconsistent in-memory state versus disk state.

**Recommended fix:** Put all destructive storage operations through the same media-operation coordinator as encoding. Stop or reject new capture/media work, wait for active per-profile operations, then reset under a defined lock order. Prefer per-profile operation state over one process-wide mutex so unrelated profiles need not block one another.

### 3. Cleanup frees shared state after a bounded wait even when workers survive

**Evidence:** Detached jobs are counted atomically, but shutdown waits only 30 seconds in [app/ACAP.c](app/ACAP.c#L2615). If jobs remain, it explicitly proceeds in [app/ACAP.c](app/ACAP.c#L2597), then deletes `status_container`/`app` in [app/ACAP.c](app/ACAP.c#L2624) and [app/ACAP.c](app/ACAP.c#L2640). Archive retry sleeps can last 60 seconds at a time in [app/recordings.c](app/recordings.c#L360), and ffmpeg jobs can run for minutes.

**Interleaving:** Cleanup times out while a worker is sleeping or encoding. Cleanup frees the cJSON status tree. The worker wakes and calls an `ACAP_STATUS_Set*()` function, dereferencing freed state.

**Impact:** Shutdown heap corruption, use-after-free, partial output, and abrupt termination of child processes or file updates.

**Recommended fix:** Make workers joinable and cancellable through a central job manager. On shutdown, close admission, signal cancellation, terminate/reap active child processes, and join every worker before freeing shared state. If a hard timeout remains necessary, do not free objects reachable by surviving threads.

## High-Severity Findings

### 4. FPS re-encode bypasses the global encoder lock

**Evidence:** Normal generation and migration serialize through `encode_mutex`, for example [app/media.c](app/media.c#L1207) and [app/media.c](app/media.c#L2277). `media_reencode_export()` starts ffmpeg, renames the new export, copies state, and deletes the old export at [app/media.c](app/media.c#L2515) without acquiring that mutex. It runs in its own detached worker from [app/timelapse.c](app/timelapse.c#L350).

**Interleaving:** A normal export catches up pending frames and purges their JPEGs while re-encode converts an older export. Re-encode then renames its older result over the new-fps path and copies an older finalized-frame count.

**Impact:** Newly finalized frames can disappear from both raw storage and the selected export.

**Recommended fix:** Admit re-encode through the same media coordinator and hold `encode_mutex` across input inspection, ffmpeg, state/meta updates, renames, and old-file removal. Revalidate source generation before commit.

### 5. Sun-event JSON and GSource objects are modified from the HTTP thread

**Evidence:** `SunEventsSettings`, `midnight_timer`, and `sunnoon_timer` are shared globals in [app/sunevents.c](app/sunevents.c#L16). HTTP calls `SunEvents_Set()` in [app/sunevents.c](app/sunevents.c#L274), which replaces cJSON children in [app/sunevents.c](app/sunevents.c#L413) and destroys/replaces timer sources in [app/sunevents.c](app/sunevents.c#L52). Main-context callbacks concurrently read the same tree in [app/sunevents.c](app/sunevents.c#L69), while capture conditions read it in [app/sunevents.c](app/sunevents.c#L160) and [app/sunevents.c](app/sunevents.c#L214).

**Impact:** cJSON use-after-free, concurrent tree corruption, and unsafe GSource lifecycle manipulation across contexts.

**Recommended fix:** Parse and validate the request on the HTTP thread, then marshal the complete state/timer update to the GLib main context. Return an immutable duplicate for HTTP responses. If reads remain cross-thread, protect the tree with a lock and copy scalar values while locked.

### 6. Queue functions allow duplicate and conflicting detached jobs

**Evidence:** Reset, refresh, archive, and preview queue paths create workers without an atomic admission check in [app/recordings.c](app/recordings.c#L192), [app/recordings.c](app/recordings.c#L286), [app/recordings.c](app/recordings.c#L396), and [app/recordings.c](app/recordings.c#L517). `encode_mutex` limits some ffmpeg overlap, but it does not coordinate reset/clear, waiting workers, or duplicate requests.

**Interleaving:** Repeated clicks or two clients enqueue the same job twice. One worker finishes or fails a try-lock and publishes completion while another remains queued or active. A reset worker can run while an archive or preview is reading the same profile.

**Impact:** Redundant expensive work, deletion beneath active media, false completion, and nondeterministic final cache/state ownership.

**Recommended fix:** Add atomic admission keyed by operation and profile. Return the existing job ID for duplicate requests, reject incompatible operations, and release admission only in a single worker cleanup path.

### 7. Shared `mediaJob` status has no job ownership

**Evidence:** Every queue writes the same `mediaJob` object, for example [app/recordings.c](app/recordings.c#L298) and [app/recordings.c](app/recordings.c#L529). Independent workers set the same `active` field false in their completion paths. Status field mutation is memory-safe because `status_lock` protects individual writes, but the multi-field state transition is not atomic and has no owner token.

**Interleaving:** Job A is active. Job B updates `kind`, `profileId`, and `active`; Job A then finishes and clears `active`. The browser sees Job B as complete even though it is waiting for the encoder or still running.

**Impact:** Premature success/failure UI, stale archive lists, and downloads or playback started before their requested output exists.

**Recommended fix:** Give each job a monotonically increasing ID. Update status as one locked object, and allow only the matching owner to report progress or completion. A job list is preferable if concurrent jobs are intentionally supported.

### 8. Settings cJSON is replaced while another thread reads borrowed pointers

**Evidence:** HTTP replaces children of the shared settings object in [app/ACAP.c](app/ACAP.c#L204). `ACAP_Get_Config()` returns a borrowed pointer without synchronization in [app/ACAP.c](app/ACAP.c#L248). Retention cleanup reads `retentionMonths` from that tree in [app/recordings.c](app/recordings.c#L847).

**Interleaving:** Retention obtains a child pointer. HTTP replaces that child, freeing it. Retention then reads `valueint` through the stale pointer.

**Impact:** Use-after-free or corrupted retention decisions.

**Recommended fix:** Add a settings lock. Mutations and serialization must hold it; readers should either copy required scalars while locked or receive a deep snapshot with explicit ownership.

### 9. Cancelling the FastCGI thread can strand locks and ffmpeg children

**Evidence:** Cleanup uses `pthread_cancel()` followed by join in [app/ACAP.c](app/ACAP.c#L373). Direct GET fallback paths can still run media generation synchronously on the HTTP thread, including export handling in [app/recordings.c](app/recordings.c#L1550). ffmpeg supervision uses cancellation points such as `poll`, reads, and sleeps while `encode_mutex` is held, without pthread cleanup handlers.

**Impact:** Cancellation can leave `encode_mutex` locked, descriptors open, and a child ffmpeg process running and writing after application cleanup has begun.

**Recommended fix:** Stop HTTP cooperatively by closing/waking the accept socket and checking an atomic stop flag. Do not cancel a thread that may own application locks. Route all potentially slow media builds to tracked workers whose child PID can be terminated and reaped during shutdown.

## Medium-Severity Findings

### 10. AVI migration can deadlock while draining child pipes

**Evidence:** Migration captures both stdout and stderr in [app/media.c](app/media.c#L2327), drains stdout to EOF first in [app/media.c](app/media.c#L2345), and reads stderr only afterward in [app/media.c](app/media.c#L2389).

**Deadlock:** ffmpeg fills the stderr pipe and blocks waiting for the parent to drain it. The parent blocks waiting for stdout EOF, which ffmpeg cannot produce because it is blocked on stderr.

**Impact:** Permanent migration stall while holding `encode_mutex`; shutdown then reaches its timeout path.

**Recommended fix:** Drain both descriptors concurrently with `poll()`/nonblocking I/O, separate reader threads, or a GLib subprocess API that communicates on both pipes.

### 11. Preview MP4 and frame-count sidecar are committed outside one critical section

**Evidence:** Preview finalization completes, then releases `encode_mutex` in [app/media.c](app/media.c#L2123). Its `.frames` sidecar is written afterward in [app/media.c](app/media.c#L2128).

**Interleaving:** Another preview begins after the MP4 rename but before the sidecar update and observes a new video with stale coverage metadata.

**Impact:** Wrong append range, unnecessary rebuild, or a preview reported as covering a different frame count than its MP4.

**Recommended fix:** Write the sidecar to a temporary file and atomically rename it before releasing the media lock. For stronger crash consistency, store a generation ID in both metadata and media state.

### 12. Media error and throughput globals have data races

**Evidence:** `last_error` and `avg_encode_ms_per_frame` are process-wide mutable globals in [app/media.c](app/media.c#L24) and [app/media.c](app/media.c#L107). `media_last_error()` returns the shared buffer directly in [app/media.c](app/media.c#L1075). Preview releases `encode_mutex` before updating throughput in [app/media.c](app/media.c#L2135), and re-encode is currently unlocked.

**Impact:** Torn or misattributed error messages, incorrect retry classification based on another job's error, and undefined behavior from unsynchronized C reads/writes.

**Recommended fix:** Return errors as per-operation result data or use thread-local storage. Protect throughput reads/writes with a dedicated mutex, or update it while the centralized media-operation lock is held.

### 13. HTTP lifecycle fields are unsynchronized plain integers

**Evidence:** The worker reads `http_thread_running` in [app/ACAP.c](app/ACAP.c#L272), while cleanup writes it in [app/ACAP.c](app/ACAP.c#L372). `initialized` and `fcgi_sock` are also read and written by both threads in [app/ACAP.c](app/ACAP.c#L279) and [app/ACAP.c](app/ACAP.c#L366).

**Impact:** These are formal C data races, so behavior is undefined even if closing/cancelling the socket usually makes shutdown appear to work.

**Recommended fix:** Use GLib atomics for the stop flag and guard socket/initialization transitions with a lifecycle mutex. Keep socket ownership in one thread where possible.

### 14. Shared media-path initialization is not one-time/thread-safe

**Evidence:** `ffmpeg_path`, `ffmpeg_path_resolved`, and `ffmpeg_path_logged` are shared globals in [app/media.c](app/media.c#L26). `resolve_ffmpeg_path()` performs an unlocked check-then-initialize in [app/media.c](app/media.c#L71), and availability checks may call it before an encode lock is held.

**Impact:** Concurrent first use is a C data race and can expose a partially initialized path or duplicate/incorrect logging. The practical window is small but real.

**Recommended fix:** Use `g_once_init_enter()` / `g_once_init_leave()` or initialize the path synchronously before worker admission.

### 15. `status_lock` is held while writing a complete HTTP response

**Evidence:** The `/app` endpoint holds `status_lock` across `ACAP_HTTP_Respond_JSON()` in [app/ACAP.c](app/ACAP.c#L159), unlike the `/status` endpoint, which duplicates under lock and responds after unlocking in [app/ACAP.c](app/ACAP.c#L766).

**Impact:** A slow or blocked client can prevent workers from publishing status and can lengthen shutdown races. This is blocking, not a lock inversion.

**Recommended fix:** Duplicate `app` or its status-bearing data under the lock, unlock, then serialize/write the snapshot.

## Low-Severity and Residual Risks

### 16. Migration completion scheduling flag is cross-thread and unlocked

`services_callback_queued` is written by a migration worker and the main-context callback without synchronization in [app/migration.c](app/migration.c#L43), [app/migration.c](app/migration.c#L615), and [app/migration.c](app/migration.c#L623). This can theoretically duplicate or miss callback admission. Use an atomic compare-and-exchange or keep all accesses on the main context.

### 17. Status getter API returns borrowed mutable data without locking

`ACAP_STATUS_Bool`, `ACAP_STATUS_Int`, `ACAP_STATUS_Double`, `ACAP_STATUS_String`, and `ACAP_STATUS_Object` access the shared tree without acquiring `status_lock` in [app/ACAP.c](app/ACAP.c#L922). The string/object getters return pointers that can be invalidated by another status write. Current exposure appears limited, but the API is unsafe for worker use. Return values or deep copies under the lock.

### 18. Several worker-accessible paths still use `localtime()`

Archive naming uses `localtime()` in [app/recordings.c](app/recordings.c#L1253), sun-event calculations use it in [app/sunevents.c](app/sunevents.c#L81) and [app/sunevents.c](app/sunevents.c#L344), and date helpers use it in [app/ACAP.c](app/ACAP.c#L1502). `localtime()` may share static storage and is unsafe under concurrent calls. Replace all uses with `localtime_r()` and check its return value.

### 19. Main-thread marshalling has no shutdown state

`Run_On_Main_Thread()` queues a callback containing a pointer to a caller's stack object and waits indefinitely in [app/timelapse.c](app/timelapse.c#L100). If the main loop stops after enqueue but before dispatch, the HTTP thread remains waiting until cancellation; the queued callback retains a pointer to the cancelled thread's dead stack. Add a shutdown flag, reject new marshalled work once shutdown starts, and use heap-owned tasks with cancellation/timeout semantics.

## Synchronization That Appears Sound

- `recording_state` operations consistently use the recursive `recording_mutex`; recursive behavior is intentional because capture calls locked helpers.
- Timelapse profile/tree operations use `profiles_lock`, and structural timer/subscription updates are normally marshalled to the main context.
- `ArchiveList` is protected by `archive_list_mutex`. It must remain recursive because retention cleanup calls archive deletion while already holding it.
- Individual status setters and `/status` snapshots are protected by `status_lock`.
- Normal export, archive, preview, and AVI conversion ffmpeg operations use `encode_mutex`; the FPS re-encode omission is the exception.
- Every currently identified detached worker path brackets normal execution with `ACAP_Background_Job_Begin()` / `ACAP_Background_Job_End()`. The defect is the bounded cleanup policy, not a missing counter call.
- `midnight_archive_active` and periodic sweep admission use GLib atomics.
- The SIGTERM callback is dispatched by a GLib Unix signal source in the main context; it is not executing logging or GLib APIs directly in an asynchronous POSIX signal handler.

## Recommended Fix Order

1. Introduce a media/destructive-operation coordinator with per-profile admission and job IDs.
2. Make archive operate on an atomically rotated recording generation.
3. Replace detached/bounded shutdown with cancellation, child termination, and worker joins.
4. Marshal sun-event state and GSource updates to the main context.
5. Put FPS re-encode and all file commits under the media coordinator.
6. Add locks/snapshots for settings, media errors, throughput, and HTTP lifecycle state.
7. Drain migration pipes concurrently and tighten the lower-severity APIs.

## Validation Needed After Fixes

The repository has no automated test suite and requires the Docker ACAP build plus camera testing for full validation. At minimum, exercise these adversarial scenarios on-device:

- capture repeatedly while starting an archive and verify post-snapshot frames remain live;
- issue duplicate Play/Archive/Reset requests from two clients;
- change FPS during hourly export processing;
- update sun-event location while interval captures fire;
- send SIGTERM during ffmpeg, archive retry sleep, migration, and an HTTP media fallback;
- force high ffmpeg stderr output during migration and verify progress remains live;
- throttle an `/app` client while workers publish status.