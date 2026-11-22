#!/bin/sh
# Runs a command in the ounce-devkit Docker image (devkitPro's devkitA64 with libounce,
# OunceVK and kaiHEN's library for Switch 2 homebrew), with the repository mounted at /work
# and the current directory kept:
#
#   switch2/tools/devkit.sh cmake --build switch2/build/cmake
#
# switch2/tools/build.sh builds the image when it's missing (see ensure_image there).
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
REL=${PWD#"$REPO"}
[ "$REL" = "$PWD" ] && REL=
exec docker run --rm --network=host \
    -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$REPO:/work" -w "/work$REL" \
    "${DEVKIT_IMAGE:-ounce-devkit:20261114}" "$@"
