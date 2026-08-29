The bundled `ffmpeg` is a minimal static AArch64 build for Timelapse3.

Rebuild it from the repository root with:

```sh
./tools/ffmpeg/build.sh
```

The build uses the Axis ACAP 12.11.0 SDK image and pinned FFmpeg 7.1.2 and x264
source revisions. It disables all components by default, then enables only the
JPEG/AVI/MP4/H.264 pipeline used by this ACAP. The script verifies the required
component manifest before replacing this binary.

The result is GPLv3-or-later because FFmpeg is built with GPL support and
statically links x264. See `app/LICENSE` for bundled-binary licensing details.
