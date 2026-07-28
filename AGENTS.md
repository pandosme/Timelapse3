# AGENTS.md — Timelapse2

Guidance for LLM coding agents working in this repo. Read this before making changes; it covers
things that are easy to get wrong from source alone (build environment, storage layout, and a
few non-obvious invariants discovered the hard way).

## What this is

Timelapse2 is an AXIS camera app (ACAP — Axis Camera Application Platform) written in C. It
captures still JPEG frames on a schedule (or camera event), and assembles them into MP4/H.264
timelapse videos directly on the camera, with a browser UI served from the camera itself.

Typical use cases: construction-site progress, long-term scene monitoring, forensic review,
daily daylight snapshots (e.g. sun-noon shots). Capture intervals range from every few seconds
to once a day; playback fps for the resulting video is a separate, user-chosen setting (10fps for
plain review, 30-60fps for a "nice" timelapse look) — the two are unrelated, see the GOP section
below.

User-facing feature history lives in `README.md`. This file is about how the code works.

## Build — there is no local toolchain

`app/*.c` depends on the Axis ACAP Native SDK (glib, axevent, vdostream, fcgi, libcurl) which is
**not installed on a normal dev machine**. Do not try to `gcc` these files directly — it will fail
on missing headers, and that failure tells you nothing about whether the code is actually correct.

The real build is Docker-based:

```sh
./build.sh          # builds the aarch64 .eap package
```

armv7hf is intentionally commented out in `build.sh` — those cameras won't get AXIS OS 12.x
(this app's `compatibleOsVersions` floor in `app/manifest.json`), so there's no point building
for that arch. `third_party/ffmpeg/armv7hf/` is left in place in case that changes.

`Dockerfile` pulls `axisecp/acap-native-sdk:<version>-<arch>-ubuntuXX.XX`, copies `app/` in, copies
a prebuilt `ffmpeg` binary from `third_party/ffmpeg/<arch>/ffmpeg` into `bin/`, and runs
`acap-build`. **The build hard-fails if that ffmpeg binary is missing** — it's not fetched or
built as part of this pipeline, it must already be present in `third_party/ffmpeg/<arch>/`.

If you don't have Docker / can't run the real build, you can still sanity-check pure-logic changes
(no glib/ACAP dependency) by extracting the function(s) into a standalone harness and compiling
with plain `gcc` against `storage.c` + `cJSON.c` (both are dependency-free standard C). This is
how several fixes in this codebase's history were verified without a device. It does **not**
substitute for a real build+flash before trusting a fix — only use it to validate algorithmic
logic (path construction, size math, string handling), not anything touching glib/ffmpeg/HTTP.

## Directory layout

```
app/                 all C source + web UI, this is what gets packaged
  main.c             entry point, startup sequence, SD card init, global reset endpoint
  ACAP.c / ACAP.h     vendored framework: HTTP routing, settings/status storage, device info,
                      file helpers, event firing — think of it as this app's "stdlib"
  cJSON.c / cJSON.h   vendored JSON library, used everywhere for config/state/API payloads
  timelapse.c/h       recording *profile* CRUD (name, resolution, fps, trigger, schedule, archive
                      cadence) + HTTP endpoint "timelapse"
  capture.c/h         takes one JPEG snapshot from the camera (vdo-stream) for a given profile
  recording_store.c/h persistent per-profile capture state: frame count, byte totals, fps, first/
                      last timestamp — the source of truth for "how much has this profile
                      captured, ever"
  recordings.c        HTTP endpoints for recordings/video/export/archive/download; orchestrates
                      capture → recording_store → media.c; archive lifecycle; AVI-era helpers
  media.c             all ffmpeg invocation: building export/preview/archive MP4s from JPEG
                      sequences, incremental re-encode, GOP/encoder settings, size estimation
  storage.c/h         filesystem path helpers (frame dirs, cache dirs, export/archive paths) —
                      dependency-free, safe to unit-test standalone
  sunevents.c/h       dawn/sunrise/sun-noon/sunset/dusk calculation + "Sun Noon" camera event
  migration.c/h       one-time migration of legacy Timelapse 2.x AVI recordings to MP4
  settings/           default settings.json / events.json bundled into the package
  html/index.html     the entire web UI: markup + inline JS (jQuery + Bootstrap), single page
third_party/ffmpeg/<arch>/ffmpeg   prebuilt ffmpeg binaries bundled into the package at build time
Dockerfile, build.sh  the only supported build path (see above)
```

There is no separate frontend build step — `app/html/index.html` is a single file with a large
inline `<script>` block. Edit it directly; there's no bundler/transpiler in front of it.

## Data flow, end to end

1. **Capture**: a timer or camera event fires → `Timelapse_Init`'s callback (`main.c`) → checks
   daylight conditions → `Recordings_Capture()` (`recordings.c`) → `capture_snapshot()`
   (`capture.c`, grabs one JPEG from vdo-stream) → `recording_store_capture_profile()`
   (`recording_store.c`) writes the JPEG to `<frames_dir>/00000123.jpg` and bumps the profile's
   cumulative counters (`frames`, `images`, `sizeBytes`, `size`, `last`, ...).
2. **Encoding**: nothing encodes automatically in the background. A video only gets (re)built when
   something asks for it — playing, downloading, refreshing, or archiving a recording all end up
   calling into `media.c`'s `generate_mp4()`, which shells out to the bundled `ffmpeg` against the
   raw JPEG sequence (`ffmpeg -framerate FPS -i frames/%08d.jpg ...`).
3. **Incremental export**: the "export" video (used for live preview/download) is built
   incrementally — only newly-captured frames get encoded in each pass, then concatenated
   (stream-copy, no re-encode) onto the existing export MP4. Once a batch of raw JPEGs has been
   folded in, **those JPEG files are deleted** (`delete_frame_range`) to save SD card space. See
   "Traps" below — this purging has real consequences for anything that assumes raw frames are
   still on disk.
4. **Archive**: a full, single-pass encode of everything captured so far, written to a
   timestamped file in the archive directory; on success the live recording's raw media is
   cleared (`Recordings_Clear`). Unlike export, archive always fully re-encodes (no cache-hit
   short-circuit) — see `cache_is_valid()` in `media.c`, which explicitly forces `MEDIA_ARCHIVE`
   to always regenerate.
5. **Frontend** polls `GET recordings` / `GET archive` / `GET timelapse` to render the Recordings
   table, and drives per-recording actions (Play, Download, Edit, Refresh, Archive, Reset, Delete)
   via the HTTP endpoints below. Long-running media jobs report progress through a shared
   `ACAP_STATUS` group called `"mediaJob"` (`active`/`stage`/`progress`/`estimatedSeconds`/
   `message`), which the frontend polls via `GET status` while the triggering request is still in
   flight (works because the ACAP HTTP server handles requests concurrently).

## HTTP endpoints (all registered via `ACAP_HTTP_Node`, admin-only)

| Node | Methods | File | Purpose |
|---|---|---|---|
| `timelapse` | GET/PUT/DELETE | timelapse.c | recording profile CRUD |
| `recordings` | GET/PUT/DELETE | recordings.c | list recordings; `PUT` force-processes pending chunks into the export MP4 ("Refresh"); `DELETE` resets a recording's media |
| `video` | GET | recordings.c | stream the live/preview MP4 inline (folds in pending frames first) |
| `export` | GET | recordings.c | download the live recording as MP4 (timestamped filename) |
| `archive` | GET/PUT/DELETE | recordings.c | list archives; `PUT` archives a recording now; `DELETE` removes an archive |
| `download` | GET | recordings.c | download/stream an existing archive file |
| `migration` | GET/POST | migration.c | legacy AVI→MP4 migration status/control |
| `sunevents` | GET/PUT | sunevents.c | camera location + sun event config |
| `reset` | POST | main.c | nuclear option: wipe all storage and app state |
| `app`, `settings`, `status` | — | ACAP.c | framework-level config/status (not app-specific) |

## Storage layout on the camera

`storage_ensure_root()` (`storage.c`) resolves the actual root at startup by searching, in order,
for an existing `timelapse2` directory: the modern AXIS "storage areas" path
(`/var/spool/storage/areas/SD_DISK/root`), the legacy path (`/var/spool/storage/SD_DISK`), then a
uid-scoped variant of the legacy path. Whichever one already has a `timelapse2` directory wins, so
upgrades and firmware differences don't strand a camera's existing data; if none exists yet
(fresh install), it creates one in the first candidate that's actually writable, same order.
**There is deliberately no internal-flash fallback** — recordings only ever belong on the SD card;
if no candidate is usable, `storage_ensure_root()` fails and the app reports no SD card rather than
silently writing somewhere small and non-persistent. (An internal-flash fallback existed briefly
and was removed after it caused exactly that: silent recording into
`/usr/local/packages/timelapse2/localdata`, which is wiped on a full uninstall/reinstall — see git
history around the storage.c rewrite if you need the postmortem.) The two real AXIS OS mount
layouts differ by device/firmware: some only bind-mount the SD card at the legacy path, some only
at the areas path, and some (older/other firmware) dual-mount it at both.

```
<SD card>/timelapse2/                           (path varies - see storage_ensure_root() above)
  profiles.json                                 timelapse profile definitions
  recordings.json                               recording_store state (frame counts, byte totals, fps, ...)
  profiles/<profileId>/
    frames/NNNNNNNN.jpg                         raw captured frames (purged as they're encoded)
    cache/export_<fps>fps.mp4                   the live/preview export video (fps is in the filename!)
    cache/export_<fps>fps.mp4.json              metadata sidecar: frames/fps/last/sizeBytes/width/height
                                                  at the time this file was last (re)generated
    cache/export_<fps>fps.mp4.state.json        incremental state: finalizedFrames/fps for the export
    cache/preview_<fps>fps.mp4                  (legacy path constant; current code routes preview
                                                  through the export path instead, see media.c)
  archive/<name>_<timestamp>.mp4                finalized archive videos
```

## Non-obvious things worth knowing before you touch `media.c` / `recordings.c`

- **GOP is a fixed frame count (`EXPORT_GOP_FRAMES` in media.c), not fps-scaled.** It used to be
  `fps * 10` ("10 seconds of playback"), which sounds reasonable but is wrong here: frame *count*
  never changes with the fps setting (fps is purely playback speed for a fixed image sequence), so
  scaling GOP-in-frames by fps meant the exact same clip could get 1 keyframe at 60fps but 4
  keyframes at 10fps — a large, surprising file-size swing for identical source images. Keep GOP
  frame-count-based; if you need to make it fps-aware again, cap it against the actual frame count.
- **`-sc_threshold 0` is intentional.** Without it, libx264's scene-cut detection inserts extra
  keyframes on any big frame-to-frame delta — very common in timelapse content (lighting changes
  between captures, or genuinely unrelated scenes when captures are hours/days apart). That
  produces short, irregular GOPs regardless of the fixed `-g`/`-keyint_min` you set. Don't drop it
  without addressing GOP irregularity another way.
- **`recording_store`'s `frames`/`images`/`size`/`sizeBytes` are lifetime, monotonically-increasing
  counters.** They never decrease, even though the underlying raw JPEG files get deleted once
  folded into the export video. Don't treat them as "what's currently on disk" — they answer "how
  much has ever been captured for this profile."
- **Export files are keyed by fps in their filename** (`export_10fps.mp4` vs `export_60fps.mp4`).
  If you resolve fps from two different sources for the same profile (e.g. the live profile
  config vs. `recording_store`'s cached copy from the last capture), and they've drifted apart,
  you'll silently be looking at two different files. Always resolve fps once and thread it through.
- **`generate_mp4()`'s `cache_is_valid()` treats `MEDIA_ARCHIVE` as always-invalid** — archives
  always fully regenerate; only export/preview (`MEDIA_EXPORT`/`MEDIA_PREVIEW`) short-circuit on a
  metadata match. If a "why didn't this re-encode" investigation involves an export/preview video,
  check the metadata JSON sidecar (`<path>.mp4.json`) against the live `recording_store` counters
  first — a stale/matching sidecar is a legitimate reason for `generate_mp4` to no-op.
- **Never call a `snprintf(out, len, "%s", src)`-style helper with `src == out`.** This bit the
  codebase twice (a filename-normalization helper self-aliased its in/out buffer, silently
  collapsing the string to empty under glibc). If you see a function taking both an output buffer
  and a `const char*` source, check whether callers ever pass the same buffer for both, and make
  the function copy-then-process internally if so.
- **Two separate in-memory JSON trees can represent the same `recordings.json`.**
  `recording_store.c` keeps a module-static `recording_state` (the actual source of truth, mutated
  in place by every capture). `recordings.c` has its own `Recordings_Container`, which is normally
  just an alias to the same object via `recording_store_list()` — but don't assume that always
  holds; check assignment sites if you're debugging staleness.
- Progress-reporting convention: anything that runs a real ffmpeg pass and might take a while
  should push status through `ACAP_STATUS_Set*("mediaJob", ...)` (see `set_media_encode_status_*`
  / `set_reencode_status_*` in media.c, `Set_Reset_Media_Status_*` in recordings.c) so the frontend
  can poll `GET status` and animate the busy-modal progress bar. Follow that pattern for new
  long-running media operations rather than inventing a new mechanism.

## Conventions

- Logging: `LOG(...)` / `LOG_WARN(...)` (syslog + stdout) / `LOG_TRACE(...)` (compiled out by
  default, flip the `#define` in the relevant file to enable). Match the existing style in
  whichever file you're editing rather than introducing a new logging helper.
- No automated test suite exists in this repo. Verification is: build via Docker, install on a
  real camera, exercise the feature in the browser UI, and check syslog. When you can't do that
  yourself, say so explicitly rather than claiming a fix works.
- Don't add `-no-verify`-style shortcuts, don't guess at ACAP SDK behavior — grep for existing
  usage of an `ACAP_*` function before assuming its signature or semantics.
