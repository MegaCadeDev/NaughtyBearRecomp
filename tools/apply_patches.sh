#!/usr/bin/env bash
# Reset each patched submodule to its pinned commit and apply our patches on top.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

for name in XenonRecomp XenosRecomp; do
    sub="$root/tools/$name"
    [ -d "$root/patches/$name" ] || continue
    git -C "$sub" checkout -- .
    for patch in "$root"/patches/$name/*.patch; do
        echo "Applying $name/$(basename "$patch")"
        git -C "$sub" apply "$patch"
    done
done
