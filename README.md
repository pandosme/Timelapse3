# Timelapse2

Timelapse2 creates MP4 timelapse videos directly on Axis cameras.

Typical uses include:

- Construction site progress
- Seasonal or long-term scene monitoring
- Forensic review
- Event trigger validation
- Daily daylight snapshots, such as sun-noon progress images

[Download ACAP (zip)](https://www.dropbox.com/scl/fi/uik3zu8potr7rv7u2djyb/Timelapse2.zip?rlkey=ovo9g8sb6qwmbyzptkn6c9674&dl=1)

If you find this ACAP valuable, please consider [buying me a coffee](https://buymeacoffee.com/fredjuhlinl).

Please report bugs and other issues [here](https://github.com/pandosme/Timelapse2/issues).

---

## Requirements

- Axis camera with AXIS OS 12 support.
- SD card storage enabled on the camera.
- Administrator access to install and configure the ACAP.

Timelapse2 version 3.0.0 keeps the package name `timelapse2`, so it can upgrade earlier Timelapse 2.x installations.

Migration from TimelapseMe is not supported because TimelapseMe and Timelapse2 are different applications.

---

## What's New in 3.0.0

Version 3.0.0 is a major update focused on easier operation, MP4 video, and safer upgrades from Timelapse 2.x.

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

Timelapse 2.x used AVI recordings. Version 3.0.0 uses MP4.

When old AVI recordings are found, Timelapse2 shows an **AVI migration required** dialog before normal operation starts.

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

If possible, an ongoing recording from Timelapse 2.x is imported as the live recording base for version 3.0.0 instead of being shown as an archive. This lets the recording continue in the new MP4 workflow.

Completed old AVI archives are converted to MP4 archives.

### Recommended Upgrade Procedure

For cameras with important long-running recordings:

1. Back up the camera SD card storage before upgrading.
2. Install Timelapse2 3.0.0.
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

Set the camera location so Timelapse2 can calculate:

- Dawn
- Sunrise
- Sun noon
- Sunset
- Dusk

These values can be used to limit captures to daylight hours.

Timelapse2 can also fire a **Sun Noon** event, useful for consistent daily construction-site progress images.

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
- For important legacy installations, back up `/var/spool/storage/SD_DISK/timelapse2` before installing 3.0.0.
- If the camera has many old AVI recordings, migration can take time.
- The app shows progress during migration and media processing.

---

## History

### 3.0.0 - July 24, 2026

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

### 1.1.3 - June 17, 2026

- Fixed error "Failed to read Archive".
- Fixed stability issues.

### 1.1.2 - September 30, 2025

- Fixed a bug that prevented automatic archive on daily, weekly, or monthly cadence.

### 1.1.0 - September 27, 2025

- Added automatic archive daily, weekly, or monthly.
- Fixed removal of archived recordings after the retention period.
- Adjusted the user interface.
- Added SD card initialization information at startup.

### 1.0.6 - September 18, 2025

- Added alternative SD card check.
- Added support for selecting any camera-supported resolution.

### 1.0.5 - February 14, 2025

- Fixed resolution selection for multi-sensor cameras.

### 1.0.4 - December 14, 2025

- Fixed sun recalculation.

### 1.0.3 - December 5, 2025

- Fixed Sun Noon event trigger.

### 1.0.2 - December 2, 2025

- Fixed playback after archiving a recording.
- Fixed Sun Noon event handling.
- Fixed captures so they only trigger on transition to active for stateful events.

### 1.0.1 - December 26, 2024

- Bug fixes.

### 1.0.0 - December 25, 2024

- Initial release.