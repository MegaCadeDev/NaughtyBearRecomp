#!/usr/bin/env bash
# Reset the XenonRecomp submodule to its pinned commit and apply our patches on top.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
sub="$root/tools/XenonRecomp"

git -C "$sub" checkout -- .
for patch in "$root"/patches/XenonRecomp/*.patch; do
    echo "Applying $(basename "$patch")"
    git -C "$sub" apply "$patch"
done
