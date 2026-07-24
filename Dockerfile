ARG ARCH=armv7hf
ARG VERSION=12.11.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}
ARG ARCH

WORKDIR /opt/app
COPY ./app .
COPY ./third_party /opt/third_party
RUN set -eu; \
	mkdir -p /opt/app/bin; \
	FFMPEG_SRC="/opt/third_party/ffmpeg/${ARCH}/ffmpeg"; \
	if [ ! -f "${FFMPEG_SRC}" ]; then \
		echo "Missing bundled ffmpeg for ${ARCH}: ${FFMPEG_SRC}"; \
		echo "Place your custom ffmpeg binary at third_party/ffmpeg/${ARCH}/ffmpeg"; \
		exit 1; \
	fi; \
	cp "${FFMPEG_SRC}" /opt/app/bin/ffmpeg; \
	chmod 755 /opt/app/bin/ffmpeg
RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
	-a 'bin/ffmpeg' \
	-a 'settings/settings.json' \
	-a 'settings/events.json' 
