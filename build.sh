#!/bin/bash
set -euo pipefail

CMAKE=$(which cmake)

mkdir -p cmake-build-release

# Prefer ninja if available, otherwise fall back to make.
if command -v ninja >/dev/null 2>&1; then
  $CMAKE -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=$(which ninja) -G Ninja -S . -B cmake-build-release
  $CMAKE --build cmake-build-release --target clean -j 4 || true
  $CMAKE --build cmake-build-release --target all -j 4
else
  $CMAKE -DCMAKE_BUILD_TYPE=Release -S . -B cmake-build-release
  $CMAKE --build cmake-build-release --target clean -j 4 || true
  $CMAKE --build cmake-build-release -j 4
fi
