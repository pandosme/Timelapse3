# Timelapse 2 MP4 Migration

This branch migrates Timelapse2 from AVI/MJPEG recordings to Timelapse 2 version 3.0.0 with MP4/H.264 playback, export, archive, and upgrade compatibility.

## Target

- App name: `timelapse2`
- Version: `3.0.0`
- Storage root: `/var/spool/storage/SD_DISK/timelapse2`
- FFmpeg binary: `/usr/local/packages/timelapse2/bin/ffmpeg`
- Verified FFmpeg ACAP package on `bullet.internal`: `v2.0.0-2` aarch64. The later signed `v2.0.0-Signed` asset was only about 0.5 MB, dynamically linked, and failed on camera with missing `libavfilter.so.11`.
- Backward compatibility with Timelapse2 AVI recordings: required
- User-facing format: MP4/H.264
- Active capture storage: JPEG frame sequence

## Architecture

The old `timelapse.*`, `recordings.*`, and `recordings_images.*` design is being replaced by focused Timelapse modules:

- `storage.*` owns storage paths, directory helpers, and Timelapse 2 root paths.
- `capture.*` owns one Axis VDO JPEG snapshot operation.
- `recording_store.*` owns active recording metadata and JPEG frame sequence storage.
- `media.*` owns FFmpeg probing and MP4 generation.
- Existing `recordings.*` currently acts as the HTTP/archive layer above the internal modules.

Planned modules still to split:

- `profiles.*` from `timelapse.*`
- `triggers.*` from `timelapse.*` and `main.c`
- `solar.*` from `sunevents.*`
- `archives.*` from archive code currently in `recordings.c`
- `api.*` from endpoint handlers currently embedded in domain modules

## Complete

- Timelapse 2 version 3.0.0 identity is set in manifest/build code with ACAP package name `timelapse2`.
- Storage startup/reset now uses `/var/spool/storage/SD_DISK/timelapse2` through `storage.*`.
- Profile persistence now writes `profiles.json` under the Timelapse 2 root.
- Active captures now write JPEG frames through `capture.*` and `recording_store.*`.
- Active recording metadata is stored in Timelapse 2 `recordings.json`.
- Legacy AVI migration detects `/var/spool/storage/SD_DISK/timelapse2/timelapse.json`, active `timelapse.avi` files, and archive `.avi` files.
- The Recordings page blocks startup with an accept/rollback migration dialog only when AVI files are found.
- Accepted migration converts AVI files to MP4 archive entries with per-file progress. Declining leaves AVI files unchanged so the user can reinstall version 2.x.x.
- Export now generates MP4 with FFmpeg and returns `video/mp4`.
- A new `video` endpoint generates MP4 for browser playback.
- Archive creation now generates final `.mp4` files.
- Archive download streams MP4 directly.
- The old image inspect endpoint is no longer registered or listed in the manifest.
- The UI no longer exposes Inspect; it has a Play button and HTML5 video modal.
- Default export filename changed from `.avi` to `.mp4`.
- Obsolete `recordings_images.*` files were removed.
- FFmpeg output format is now passed explicitly as MP4 so temporary files ending in `.mp4.tmp` work with the camera FFmpeg build.
- Playback/export MP4s now use sidecar cache metadata and are reused when profile id, media kind, FPS, frame count, last timestamp, source byte count, and dimensions still match.
- FFmpeg generation is globally throttled to one encode at a time, and encodes are capped to the recording frame count captured before generation starts.
- Preview/export/archive use faster x264 settings; preview uses a lower-quality fast profile, while export/archive use `veryfast` presets.
- Old cache variants and temporary cache files are pruned after successful preview/export generation.
- Archive triggering is now profile-specific and duration-only: daily at midnight, weekly at Sunday midnight, or monthly at midnight on the last day of the month.
- The old global archive size and archive split settings were removed from the UI/default settings.
- Profile settings include an expected-images-per-day guide to estimate images per archive.
- Docker ACAP build succeeds for `aarch64` and `armv7hf`.

## Remaining Work

- Move archive logic out of `recordings.c` into `archives.*`.
- Move endpoint handlers out of domain modules into `api.*`.
- Split `timelapse.*` into `profiles.*` and `triggers.*`.
- Move daylight condition checks out of `main.c` into trigger dispatch.
- Rename or wrap `sunevents.*` as `solar.*`.
- Remove dead AVI structs/helpers from `recordings.c` after endpoint migration is complete.
- Rename flat endpoints where practical: `timelapse` to `profiles`, `archive` to `archives`, `sunevents` to `solar`.
- Add FFmpeg timeout handling and richer status fields.
- Add configurable retention limits for active JPEG frames, archive MP4s, and cache files.
- Verify AVI-to-MP4 migration on a camera with legacy 2.x recordings.
- Verify Archive on camera.

## Verification

Completed locally:

- `git diff --check`
- `./build.sh`

Build output:

- `Timelapse_3_0_0_aarch64.eap`
- `Timelapse_3_0_0_armv7hf.eap`

Completed on `bullet.internal`:

- Replaced the broken signed FFmpeg package with the static `ffmpeg_2_0_0_aarch64.eap` from release `v2.0.0-2`.
- Confirmed `/usr/local/packages/ffmpeg/lib/ffmpeg -version` runs and includes `libx264`.
- Deployed rebuilt `Timelapse_3_0_0_aarch64.eap`.
- Confirmed active JPEG frames are written under `/var/spool/storage/SD_DISK/timelapse2/profiles/<id>/frames/`.
- Confirmed Download returns HTTP 200, `Content-Type: video/mp4`, and an H.264 MP4 file.
- Confirmed Play/video returns HTTP 200, inline `video/mp4`, and an H.264 MP4 file.
- Confirmed repeated Download cache hit on `Every 10 seconds`: first encode about 2.0 seconds to first byte, immediate cache hit about 0.006 seconds to first byte.
- Confirmed repeated Play cache hit on `Every 10 seconds`: first encode about 1.1 seconds to first byte, immediate cache hit about 0.006 seconds to first byte.
- Confirmed a non-destructive archive-style FFmpeg dry run can encode those frames to H.264 MP4 with the `.mp4.tmp` output pattern.

Camera verification still needed:

- Confirm Archive creates `.mp4`, updates archive metadata, and clears active frames only after success.
