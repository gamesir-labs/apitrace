#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
HOST_BUILD_DIR="${APITRACE_HOST_BUILD_DIR:-$ROOT_DIR/build/cmake}"
WIN_BUILD_DIR="${APITRACE_WIN_BUILD_DIR:-$ROOT_DIR/build/windows-x86_64}"
DEMO_BUILD_DIR="${APITRACE_DEMO_BUILD_DIR:-$ROOT_DIR/test/build/windows-x86_64}"
ARTIFACT_DIR="${APITRACE_ARTIFACT_DIR:-$ROOT_DIR/test/artifacts/windows-x86_64}"
BUILD_TYPE="${APITRACE_BUILD_TYPE:-Debug}"
WIN_BUILD_TYPE="${APITRACE_WIN_BUILD_TYPE:-Release}"
WINE_ROOT="${APITRACE_WINE_ROOT:-/Users/shiyu/Documents/Project/gamesir/wine-proton-macos}"
WINE_BRANCH="${APITRACE_WINE_BRANCH:-proton-11.0-macos}"
BUILD_WINE="${APITRACE_BUILD_WINE:-0}"

build_native() {
    cmake -S "$ROOT_DIR" -B "$HOST_BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    cmake --build "$HOST_BUILD_DIR"
}

build_windows_apitrace() {
    cmake -S "$ROOT_DIR" -B "$WIN_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$WIN_BUILD_TYPE" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake" \
        -DAPITRACE_BUILD_METAL_BACKEND=OFF
    cmake --build "$WIN_BUILD_DIR"
    cmake --install "$WIN_BUILD_DIR" --prefix "$ARTIFACT_DIR/apitrace"
}

build_triangle_demos() {
    cmake -S "$ROOT_DIR/test" -B "$DEMO_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$WIN_BUILD_TYPE" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake"
    cmake --build "$DEMO_BUILD_DIR"
    cmake --install "$DEMO_BUILD_DIR" --prefix "$ARTIFACT_DIR/demo"
}

build_wine_proton11() {
    if [ ! -d "$WINE_ROOT/.git" ]; then
        echo "Wine repo not found: $WINE_ROOT" >&2
        exit 1
    fi

    git -C "$WINE_ROOT" fetch origin "$WINE_BRANCH"
    git -C "$WINE_ROOT" checkout -B "$WINE_BRANCH" "origin/$WINE_BRANCH"

    case "$(uname -m)" in
        arm64)
            "$WINE_ROOT/scripts/build_on_m1.sh"
            ;;
        x86_64)
            "$WINE_ROOT/scripts/build_on_intel.sh"
            ;;
        *)
            echo "Unsupported host architecture: $(uname -m)" >&2
            exit 1
            ;;
    esac
}

mkdir -p "$ARTIFACT_DIR"
rm -rf "$ARTIFACT_DIR"
mkdir -p "$ARTIFACT_DIR"
build_native
build_windows_apitrace
build_triangle_demos

if [ "$BUILD_WINE" = "1" ]; then
    build_wine_proton11
fi
