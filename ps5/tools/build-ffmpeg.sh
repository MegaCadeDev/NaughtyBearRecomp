#!/usr/bin/env bash
# Cross-compiles FFmpeg's XMA decoder (libavcodec, libavutil) for the PS5 into
# .deps/ffmpeg, from thirdparty/FFmpeg with the patches in patches/FFmpeg (applied by
# tools/apply_patches.sh). The same configuration as the PC build, without threads.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd -- "$root/.." && pwd)
sdk="${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}"
prefix="$root/.deps/ffmpeg"
build="$prefix/build"

if [[ -f $prefix/lib/libavcodec.a && -f $prefix/.revision && $(<"$prefix/.revision") == $(git -C "$repo/thirdparty/FFmpeg" rev-parse HEAD) ]]; then
    exit 0
fi

# The SDK's own compiler driver: the same target and flags as tooling/prospero-clang18,
# and it can link, which configure's checks need.
mkdir -p "$build"
cd "$build"
"$repo/thirdparty/FFmpeg/configure" --prefix="$prefix" \
    --enable-cross-compile --target-os=none --arch=x86_64 \
    --cc="$sdk/bin/prospero-clang" --ar="$sdk/bin/prospero-ar" --ranlib="$sdk/bin/prospero-ranlib" --nm="$sdk/bin/prospero-nm" \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect --disable-asm \
    --disable-debug --enable-decoder=xma1,xma2 --disable-avdevice --disable-avformat --disable-swscale \
    --disable-swresample --disable-postproc --disable-avfilter --enable-static --disable-shared --enable-pic \
    --disable-pthreads > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
make -j"$(nproc)" > make.log 2>&1 || { tail -20 make.log; exit 1; }
make install > /dev/null
git -C "$repo/thirdparty/FFmpeg" rev-parse HEAD > "$prefix/.revision"
echo "==> [ffmpeg] built into $prefix"
