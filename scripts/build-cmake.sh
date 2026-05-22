#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="${APITRACE_BUILD_DIR:-$ROOT_DIR/build/cmake}"
BUILD_TYPE="${APITRACE_BUILD_TYPE:-Debug}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR"

