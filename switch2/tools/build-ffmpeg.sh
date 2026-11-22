#!/usr/bin/env bash
# Cross-compiles FFmpeg's XMA decoder (libavcodec, libavutil) for the Switch 2 into
# switch2/.deps/ffmpeg, from thirdparty/FFmpeg with the patches in patches/FFmpeg (applied by
# tools/apply_patches.sh). The PS5's configuration (ps5/tools/build-ffmpeg.sh) with
# devkitA64's compiler. Runs in the devkit image (switch2/tools/devkit.sh).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd -- "$root/.." && pwd)
prefix="$root/.deps/ffmpeg"
build="$prefix/build"
bin="$DEVKITPRO/devkitA64/bin"

if [[ -f $prefix/lib/libavcodec.a && -f $prefix/.revision && $(<"$prefix/.revision") == $(git -C "$repo/thirdparty/FFmpeg" rev-parse HEAD) ]]; then
    exit 0
fi

# configure's link checks need libnx's start-up files and libraries: libounce's specs.
flags="-mcpu=cortex-a78c+crypto -mtp=soft -fPIE -ftls-model=local-exec -D__SWITCH__ -D__SWITCH2__ -I$DEVKITPRO/libounce/include"
ldflags="$flags -specs=$DEVKITPRO/libounce/switch.specs -L$DEVKITPRO/libounce/lib"
mkdir -p "$build"
cd "$build"
"$repo/thirdparty/FFmpeg/configure" --prefix="$prefix" \
    --enable-cross-compile --target-os=none --arch=aarch64 \
    --cc="$bin/aarch64-none-elf-gcc" --ar="$bin/aarch64-none-elf-ar" --ranlib="$bin/aarch64-none-elf-ranlib" --nm="$bin/aarch64-none-elf-nm" \
    --extra-cflags="$flags" --extra-ldflags="$ldflags" --extra-libs="-lnx -lm" \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect --disable-asm \
    --disable-debug --enable-decoder=xma1,xma2 --disable-avdevice --disable-avformat --disable-swscale \
    --disable-swresample --disable-postproc --disable-avfilter --enable-static --disable-shared --enable-pic \
    --disable-pthreads > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
make -j"$(nproc)" > make.log 2>&1 || { tail -20 make.log; exit 1; }
make install > /dev/null
git -C "$repo/thirdparty/FFmpeg" rev-parse HEAD > "$prefix/.revision"
echo "==> [ffmpeg] built into $prefix"
