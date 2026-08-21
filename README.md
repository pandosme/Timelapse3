# Timelapse V3

Timelapse V3 creates MP4 timelapse videos directly on Axis cameras.

Typical uses include:

- Construction site progress
- Seasonal or long-term scene monitoring
- Forensic review
- Event trigger validation
- Daily daylight snapshots, such as sun-noon progress images

[Download Pre-Compiled Signed ACAP (zip)](https://www.dropbox.com/scl/fi/7tmylmiza62oj597uxhfx/Timelapse3.zip?rlkey=zlfxfix394n3iivwit8qe2tdu&st=njmpa38n&dl=1)

## Support This Project

If Timelapse V3 is useful to you, please consider supporting the project on **BuyMeACofee**:

[Buy me a coffee](https://buymeacoffee.com/fredjuhlinl)

Please report bugs and other issues [here](https://github.com/pandosme/Timelapse3/issues).

---

## Requirements

- Axis camera with AXIS OS 12 support.
- aarch64 camera architecture.
- SD card storage enabled on the camera.
- Administrator access to install and configure the ACAP.

Timelapse V3 version 3.x supports aarch64 only.

If your camera is armv7hf-based, stay on Timelapse2 version 2.x.

Timelapse V3 keeps package ID and binary name `timelapse2` on purpose so Timelapse2 users get an in-place upgrade path and recording migration.

Because of this shared package identity, Timelapse2 and Timelapse V3 cannot be installed side by side on the same camera.

Migration from TimelapseMe is not supported because TimelapseMe and Timelapse V3 are different applications.

---

## What's New in 3.1.0

Version 3.1.0 is a major update focused on easier operation, MP4 video, and safer upgrades from Timelapse 2.x.

### MP4 Instead of AVI

- Recordings are now prepared as MP4/H.264 video.
- Live recordings can be played directly in the browser.
- Live recordings can be exported as MP4.
- Archived recordings can be played, downloaded, or deleted.
- MP4 files are easier to view in common browsers and video players than the older AVI files.

### One Recordings Page

- Active recordings and archives are shown together on the Recordings page.
- Recording settings are added and edited from the same page.
- The old separate Profiles page has been removed.
- Actions are available from each recording row.
- The table now shows **Days**, indicating how long the recording or archive covers.

### Improved Recording Settings

- Add or edit a recording in one dialog.
- The Save button is only shown when something has changed.
- Changing normal settings, such as name, trigger, expected images, or archive cadence, saves immediately without showing a media-processing progress dialog.
- Media progress is only shown when the change really requires media work, such as changing FPS for an existing recording with media.

### Archive Control

- Archive cadence can be set to:
  - No Archive
  - Daily
  - Weekly
  - Monthly
- Archive retention is configured on the Settings page.
- Archived MP4 recordings appear in the same recording library as live recordings.

### Safer Delete and Reset Actions

- **Reset media** clears captured media for a recording but keeps its settings and schedule.
- **Delete recording** removes the recording settings and its related media.
- Archive deletion removes the selected archive file and its entry from the archive list.

### Cleaner Interface

- New top navigation: Recordings, Settings, About.
- Archive management was moved into the main recording library.
- Settings now includes archive retention and migration status when relevant.
- About shows application, camera, and storage information.

---

## Upgrading From Timelapse 2.x

Timelapse 2.x used AVI recordings. Version 3.1.0 uses MP4.

When old AVI recordings are found, the ACAP shows an **AVI migration required** dialog before normal operation starts.

### Migration Choices

- **Convert to MP4** converts old AVI recordings to MP4.
- **Cancel and keep AVI** stops the migration and leaves the old AVI files unchanged.

Cancelling does not downgrade the app automatically. It keeps the old files intact so version 2.x can be reinstalled manually if needed.

### During Migration

- Progress is shown for the current file and for the overall migration.
- While migration is running, the normal action buttons are hidden.
- A cancel button is available to stop migration and keep AVI files intact.
- If cancelled, temporary MP4 output is removed and the original AVI files remain unchanged.

### After Successful Migration

- Converted recordings are available as MP4.
- Old legacy AVI and IDX files are removed after successful conversion.
- Old legacy Timelapse 2.x folders and files are cleaned up.
- The migration message is not shown again after a successful migration.

### Ongoing Recordings From 2.x

If possible, an ongoing recording from Timelapse 2.x is imported as the live recording base for version 3.1.0 instead of being shown as an archive. This lets the recording continue in the new MP4 workflow.

Completed old AVI archives are converted to MP4 archives.

### Recommended Upgrade Procedure

For cameras with important long-running recordings:

1. Back up the camera SD card storage before upgrading.
2. Install Timelapse V3 3.1.0.
3. Open the app from the camera web interface.
4. If prompted, run the AVI migration.
5. Verify that live recordings and archives are visible.
6. Play or download a few recordings to confirm the result.

The main storage folder is normally:

```text
/var/spool/storage/SD_DISK/timelapse2
```

---

## Recordings Page

The Recordings page is the main workspace.

It shows both:

- **Live** recordings that are still capturing images
- **Archive** recordings that have already been finalized

### Live Recording Actions

- **Play** prepares any pending frames and plays the recording from the beginning.
- **Export MP4** downloads the current recording as an MP4 file.
- **Archive now** creates an archive immediately and clears active media after success.
- **Reset media** removes captured media while keeping the recording settings.
- **Edit settings** opens the recording settings dialog.
- **Delete recording** removes the recording and its related media.

### Archive Actions

- **Play** plays the archived MP4 in the browser.
- **Download MP4** downloads the archived MP4 file.
- **Delete archive** removes the archive.

---

## Recording Settings

Each recording can be configured with:

- Name
- Resolution
- Playback FPS
- Event-based or timer-based capture
- Daylight condition filter
- Text overlay handling
- Archive cadence
- Expected images per day

### Trigger Types

**Event based** captures when the selected camera event is triggered.

**Timer based** captures at a fixed interval.

### Daylight Conditions

The daylight condition can suppress captures outside useful daylight hours.

Available options include:

- Any time
- Sunrise to sunset
- Dawn to dusk

### Archive Cadence

Use **No Archive** when the recording should keep running without automatic archive splits.

Use daily, weekly, or monthly archives when the integrator wants regular MP4 archive files.

---

## Settings Page

The Settings page contains shared camera/application settings.

It includes:

- Archive retention
- Migration status, when relevant
- Camera location
- Sun event calculation

### Archive Retention

Archive retention controls how long archived MP4 recordings are kept before automatic cleanup.

This helps reduce the risk of filling the SD card.

### Location and Sun Events

Set the camera location so Timelapse V3 can calculate:

- Dawn
- Sunrise
- Sun noon
- Sunset
- Dusk

These values can be used to limit captures to daylight hours.

Timelapse V3 can also fire a **Sun Noon** event, useful for consistent daily construction-site progress images.

---

## About Page

The About page shows:

- Application information
- Camera information
- Storage utilization
- Support link

---

## Notes for Integrators

- Use an SD card suitable for the expected retention period and image rate.
- Test migration on one representative camera before upgrading a large fleet.
- For important legacy installations, back up `/var/spool/storage/SD_DISK/timelapse2` before installing 3.1.0.
- If the camera has many old AVI recordings, migration can take time.
- The app shows progress during migration and media processing.

---

## History

### 3.1.4
- Improved thread safety for recording, archive, preview, export and reset operations.
- Captures made while an archive is being created are no longer removed before they are archived.
- Improved shutdown handling so background video jobs and FFmpeg processes stop cleanly.
- Fixed race conditions in settings, sun-event timers, status updates and media metadata.

### 3.1.3
- Improved several stability and behaviour issues

### 3.1.2
Video encoding on cameras with limited memory:
- Fixed exports, previews and archives still failing with "out of memory" on some cameras after 3.1.1. The encoder kept a queue of look-ahead frames and a set of B-frame references, each one a full-resolution copy of the picture; on a 4K recording that alone was several hundred megabytes. Encoding now keeps only what it needs, which cuts peak memory by more than half at the same quality.
- Added an automatic fallback for cameras that still cannot encode at the captured resolution. The app now checks free memory before encoding and, if the full-size encode does not fit, assembles the video at half or quarter size instead of failing. Captured images are always kept at full resolution on the SD card - only the assembled video is scaled, and the choice is made once per recording so playback stays consistent. The system log states the chosen size.
- If the encoder is killed anyway, the app now retries once at a smaller size instead of repeating the same failing encode every time the recording is played.

Archiving:
- Fixed the midnight archive failing with "No frame files available" for busy recordings. Once every captured image has been folded into the recording video, no image files are left on disk - the archive is now made from the recording video itself instead of reporting an empty recording.

Location:
- Fixed "Error storing GeoLocation" when saving sunrise/sunset settings. The camera's stored location text was sent to the device without escaping, so a location name containing a space or a national character made the request invalid. The camera's own reason for rejecting a location is now written to the system log.

### 3.1.1
Playback and user interface:
- Fixed archived recordings not playing: the progress dialog stayed on screen and blocked the player. Archives are finished MP4 files, so they now open directly in the player with no progress dialog.
- Fixed the busy dialog staying on screen after a fast Export or Play that completed while the dialog was still fading in.
- Fixed a dead progress bar left behind after an MP4 download started.
- Fixed a false error message shown when a video download was cancelled or the player was closed early.
- Added byte-range support to video responses. Without it the browser's player could stop part way through a recording and wait forever on the buffering spinner as soon as it needed to seek or refill its buffer.
- Fixed playback stopping at the point where new material was appended to a recording. Preview, export and archive video were encoded with different quality presets and then joined by copy, which produced an MP4 whose header did not match the second half. All parts now use the same encoder settings.
- Fixed the first playback of a recording having to download the whole video before it could start.
- A recording rebuilt while it is being played or downloaded no longer feeds the player a mix of the old and new file.
- The app answers "still starting" instead of "not found" for requests that arrive during the first seconds after start, so the interface no longer dead-ends on a slow SD card check.

Video encoding:
- Fixed exports, previews and archives failing on cameras with limited memory. The software encoder gave every thread its own copy of the frames in flight, which on high-resolution recordings used enough memory for the camera to kill it mid-encode. Encoding now splits each frame across the cores instead, keeping memory bounded without losing speed.
- Encoding failures now report the real cause in the log and the interface. A killed encoder previously produced an empty error message.
- The hardware H.264 encoder is now probed once per app start instead of on every job. On cameras without it, each export, preview and archive previously wasted about a second and filled the system log with the same failure text.

Stability:
- Fixed a crash on shutdown and restart caused by freeing application state twice.
- Fixed heap corruption when the interface polled status while a media job was updating it.
- Recording data used by background encoding is now read as a snapshot under lock, so capture writing new frames during an export or archive can no longer corrupt it.
- Removed misleading warnings about a missing settings file that is simply not created until settings are saved.

### 3.1.0 - July 24, 2026
Main changes from Timelapse Version 2.x:  
- Changed recording output from AVI to MP4/H.264.
- Added browser playback for live recordings.
- Added browser playback for archived recordings.
- Added MP4 export and download.
- Added migration from Timelapse 2.x AVI recordings.
- Added migration progress for current file and overall progress.
- Added migration cancellation while keeping AVI files intact.
- Added cleanup of legacy AVI/IDX files after successful migration.
- Imported ongoing 2.x recordings as live recordings where possible.
- Combined live recordings and archives into one recording library.
- Removed the separate Profiles page.
- Added recording editing directly from the Recordings page.
- Added No Archive archive cadence.
- Moved archive retention to Settings.
- Replaced First Capture with Days in the recording list.
- Added progress feedback for media operations that really need processing.
- Avoided media progress dialogs for settings-only saves.
- Improved reset and delete behavior for media cleanup.
- Improved navigation and overall user interface.
- Kept package name `timelapse2` for upgrade compatibility.

