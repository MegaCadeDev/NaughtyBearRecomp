#!/usr/bin/env bash
# Maps the NaughtyBear.elf+0x... offsets in a Switch 2 crash log (log.txt from the SD card) to
# functions and source lines, with the NaughtyBear.elf from the same build.
#
#   switch2/tools/symbolize.sh log.txt
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
log=${1:?usage: $0 <log.txt>}
offsets=$(grep -oE 'NaughtyBear\.elf\+0x[0-9A-Fa-f]+' "$log" | sed 's/.*+//' | sort -u)
[[ -n $offsets ]] || { echo "No NaughtyBear.elf+0x... offsets in $log" >&2; exit 1; }
cd "$root/.."
# shellcheck disable=SC2086
switch2/tools/devkit.sh sh -c '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-addr2line -f -C -p -e switch2/build/cmake/NaughtyBear.elf "$@"' sh $offsets
