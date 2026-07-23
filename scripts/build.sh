#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$ROOT" -B "$ROOT/build/release" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build/release"
ctest --test-dir "$ROOT/build/release" --output-on-failure

