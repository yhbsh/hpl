#!/bin/bash

set -e

mkdir -p .deps && cd .deps

FFMPEG_VERSION="7.0.2"
FFMPEG_TAR="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_DIR="ffmpeg-${FFMPEG_VERSION}"

if [ ! -d "$FFMPEG_DIR" ]; then
    curl -sL "https://ffmpeg.org/releases/${FFMPEG_TAR}" -o "$FFMPEG_TAR"
    tar xf "$FFMPEG_TAR"
fi

cd "$FFMPEG_DIR"

if [ ! -d "build" ]; then
    ./configure \
        --prefix="$(pwd)/build" \
        --disable-all --disable-autodetect --disable-debug --disable-doc \
        --enable-avcodec --enable-decoders \
        --enable-avformat --enable-demuxers --enable-protocols \
        --enable-swscale --enable-swresample

    make -j2
    make install
fi

cd ../../

FFMPEG_PREFIX="$(pwd)/.deps/ffmpeg-${FFMPEG_VERSION}/build"
PKG_CONFIG_PATH="${FFMPEG_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

set -xe
clang -O3 -DGL_SILENCE_DEPRECATION video.c -o video $(pkg-config --cflags --libs --static libavformat libavcodec libswscale glfw3) -framework OpenGL
clang -O3 audio.c -o audio $(pkg-config --cflags --libs libavcodec libavformat portaudio-2.0)
