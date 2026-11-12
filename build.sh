#!/bin/sh
# build.sh - configure and build on macOS (the counterpart of build.cmd): a universal (arm64 + x86_64) release
# build into build/: Syndicate64.app and synd_headless.  Extra arguments go to CMake's configure step, e.g.
#   ./build.sh -DSYN_EMBED_GAME=OFF      a public build without the game files
#   ./build.sh -DSYN_DEVTEST=ON          with the app's development checks (src/platform/macos/macos_devtest.m)
set -e
cd "$(dirname "$0")"
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
cmake -S . -B build $GEN -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build --parallel
