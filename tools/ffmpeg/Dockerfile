ARG ARCH=aarch64
ARG SDK_VERSION=12.11.0
ARG UBUNTU_VERSION=24.04

FROM axisecp/acap-native-sdk:${SDK_VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

ARG FFMPEG_REVISION=7003a8c4544b780eb0ab5c02ac4ad89d2132f2a2
ARG X264_REVISION=b35605ace3ddf7c1a5d67a2eb553f034aef41d55

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates gcc git libc6-dev make pkg-config && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/src
RUN git clone https://code.videolan.org/videolan/x264.git && \
    git -C x264 checkout "${X264_REVISION}" && \
    git clone https://github.com/FFmpeg/FFmpeg.git ffmpeg && \
    git -C ffmpeg checkout "${FFMPEG_REVISION}"

RUN . /opt/axis/acapsdk/environment-setup* && \
    cd /opt/src/x264 && \
    AS="${CC}" ./configure \
        --prefix=/opt/minimal-ffmpeg \
        --host=aarch64-linux \
        --cross-prefix=aarch64-linux-gnu- \
        --sysroot="${SDKTARGETSYSROOT}" \
        --enable-static \
        --disable-cli \
        --disable-opencl \
        --bit-depth=8 \
        --chroma-format=420 && \
    make -j"$(nproc)" && \
    make install

RUN . /opt/axis/acapsdk/environment-setup* && \
    export PKG_CONFIG_PATH=/opt/minimal-ffmpeg/lib/pkgconfig && \
    cd /opt/src/ffmpeg && \
    if ! ./configure \
        --prefix=/opt/minimal-ffmpeg \
        --arch=aarch64 \
        --target-os=linux \
        --cross-prefix=aarch64-linux-gnu- \
        --host-cc=gcc \
        --pkg-config=pkg-config \
        --sysroot="${SDKTARGETSYSROOT}" \
        --pkg-config-flags=--static \
        --extra-cflags="-I/opt/minimal-ffmpeg/include" \
        --extra-ldflags="-L/opt/minimal-ffmpeg/lib -static" \
        --extra-libs="-lpthread -lm" \
        --enable-cross-compile \
        --enable-gpl \
        --enable-static \
        --disable-shared \
        --disable-autodetect \
        --disable-everything \
        --disable-doc \
        --disable-debug \
        --disable-network \
        --disable-ffplay \
        --disable-ffprobe \
        --enable-small \
        --enable-ffmpeg \
        --enable-avcodec \
        --enable-avformat \
        --enable-avfilter \
        --enable-swscale \
        --enable-v4l2-m2m \
        --enable-protocol=file,pipe \
        --enable-demuxer=avi,concat,image2,mov \
        --enable-muxer=mov,mp4,null \
        --enable-decoder=h264,mjpeg \
        --enable-encoder=libx264,h264_v4l2m2m \
        --enable-parser=h264,mjpeg \
        --enable-bsf=h264_mp4toannexb \
        --enable-filter=scale \
        --enable-libx264; then \
        tail -n 100 ffbuild/config.log; \
        exit 1; \
    fi && \
    make -j"$(nproc)" ffmpeg && \
    mkdir -p /out && \
    cp ffmpeg /out/ffmpeg && \
    cp config.h config_components.h ffbuild/config.mak /out/
