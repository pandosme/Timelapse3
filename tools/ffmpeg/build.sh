#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
IMAGE_NAME="timelapse3-minimal-ffmpeg"
CONTAINER_ID=""
COMPONENTS_FILE="$(mktemp)"

cleanup() {
    if [ -n "${CONTAINER_ID}" ]; then
        docker rm "${CONTAINER_ID}" >/dev/null 2>&1 || true
    fi
    rm -f "${COMPONENTS_FILE}"
}
trap cleanup EXIT INT TERM

docker build --progress=plain -t "${IMAGE_NAME}" -f "${ROOT_DIR}/tools/ffmpeg/Dockerfile" "${ROOT_DIR}"
CONTAINER_ID="$(docker create "${IMAGE_NAME}")"
docker cp "${CONTAINER_ID}:/out/ffmpeg" "${ROOT_DIR}/third_party/ffmpeg/aarch64/ffmpeg.new"
docker cp "${CONTAINER_ID}:/out/config_components.h" "${COMPONENTS_FILE}"
chmod 755 "${ROOT_DIR}/third_party/ffmpeg/aarch64/ffmpeg.new"

for component in \
    AVI_DEMUXER CONCAT_DEMUXER IMAGE2_DEMUXER MOV_DEMUXER \
    MOV_MUXER MP4_MUXER NULL_MUXER \
    H264_DECODER MJPEG_DECODER \
    H264_V4L2M2M_ENCODER LIBX264_ENCODER \
    H264_PARSER MJPEG_PARSER H264_MP4TOANNEXB_BSF SCALE_FILTER \
    FILE_PROTOCOL PIPE_PROTOCOL; do
    grep -q "^#define CONFIG_${component} 1$" "${COMPONENTS_FILE}" || {
        echo "Missing required FFmpeg component: ${component}" >&2
        exit 1
    }
done

old_size="$(wc -c < "${ROOT_DIR}/third_party/ffmpeg/aarch64/ffmpeg")"
new_size="$(wc -c < "${ROOT_DIR}/third_party/ffmpeg/aarch64/ffmpeg.new")"
printf 'Existing FFmpeg: %s bytes\nMinimal FFmpeg:  %s bytes\n' "${old_size}" "${new_size}"
mv "${ROOT_DIR}/third_party/ffmpeg/aarch64/ffmpeg.new" "${ROOT_DIR}/third_party/ffmpeg/aarch64/ffmpeg"

