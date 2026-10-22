#!/usr/bin/env bash
# Reset each patched submodule to its pinned commit and apply our patches on top.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

for sub in "$root/tools/XenonRecomp" "$root/tools/XenosRecomp" "$root/thirdparty/plume"; do
    name="$(basename "$sub")"
    [ -d "$root/patches/$name" ] || continue
    git -C "$sub" checkout -- .
    for patch in "$root"/patches/$name/*.patch; do
        echo "Applying $name/$(basename "$patch")"
        git -C "$sub" apply "$patch"
    done
done
