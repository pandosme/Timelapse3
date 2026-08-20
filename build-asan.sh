#!/bin/sh
# Builds the AddressSanitizer debug package (see Dockerfile.asan). Leaves both the .eap and
# the unstripped binary in ./asan-build - keep the binary, an ASan report from the camera is
# module+offset only and needs addr2line against exactly this build.
set -eu

arch=aarch64
tag="acap-asan-${arch}"

rm -rf asan-build
docker build --progress=plain --build-arg ARCH="${arch}" --tag "${tag}" -f Dockerfile.asan .

container_id="$(docker create "${tag}")"
mkdir -p asan-build
docker cp "${container_id}":/opt/app/. ./asan-build/
docker rm "${container_id}" >/dev/null

echo "package:  $(ls asan-build/*.eap)"
echo "binary:   asan-build/timelapse2   (unstripped, for addr2line)"
