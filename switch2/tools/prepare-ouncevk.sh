#!/usr/bin/env bash
# Makes switch2/.deps/ouncevk/libouncevk_local.a: OunceVK's Vulkan driver (libouncevk.a,
# Mesa NVK) with its public vk* functions made local, and its two ICD entry points made
# global. plume loads Vulkan with volk, whose function pointers have the vk* names, and
# finds the driver through vk_icdGetInstanceProcAddr (patches/plume/0003). The same as
# Dolphin-NS2's Tools/Switch/prepare_ouncevk.sh, for the Vulkan-only archive.
# Runs in the devkit image (switch2/tools/devkit.sh).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/.deps/ouncevk"
bin="$DEVKITPRO/devkitA64/bin"
source_archive="$DEVKITPRO/portlibs/switch2/lib/libouncevk.a"
archive="$out/libouncevk_local.a"

[[ -f $archive && $archive -nt $source_archive ]] && exit 0
mkdir -p "$out"
"$bin/aarch64-none-elf-nm" -g --defined-only "$source_archive" | awk '$NF ~ /^vk[A-Z]/ { print $NF }' | LC_ALL=C sort -u > "$out/public-vk.txt"
[[ -s $out/public-vk.txt ]] || { echo "OunceVK has no vk* functions to make local" >&2; exit 1; }
"$bin/aarch64-none-elf-objcopy" --localize-symbols="$out/public-vk.txt" \
    --globalize-symbol=vk_icdGetInstanceProcAddr --globalize-symbol=vk_icdNegotiateLoaderICDInterfaceVersion \
    "$source_archive" "$archive.tmp"
for symbol in vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion; do
    "$bin/aarch64-none-elf-nm" -g --defined-only "$archive.tmp" | grep -q " $symbol\$" || { echo "OunceVK doesn't export $symbol" >&2; exit 1; }
done
mv "$archive.tmp" "$archive"
echo "==> [ouncevk] $(wc -l < "$out/public-vk.txt") functions made local in $archive"
