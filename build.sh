#!/bin/sh
set -eu

build_arch() {
	arch="$1"
	tag="acap-${arch}"

	rm -rf build
	docker build --progress=plain --no-cache --build-arg ARCH="${arch}" --tag "${tag}" .

	container_id="$(docker create "${tag}")"
	docker cp "${container_id}":/opt/app ./build
	docker rm "${container_id}" >/dev/null

	mv build/*.eap .
	rm -rf build
}

build_arch aarch64
#build_arch armv7hf

