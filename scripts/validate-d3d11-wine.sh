#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="$ROOT_DIR/../wine-enviroment/bin/wine"
WINESERVER_BIN="$ROOT_DIR/../wine-enviroment/bin/wineserver"
ROOT_BUILD_DIR="$ROOT_DIR/build/windows-cross"
TEST_BUILD_DIR="$ROOT_DIR/test/build/windows-x86_64"
DEMO_PREFIX="$ROOT_DIR/test/artifacts/windows-x86_64/demo"
DEMO_BIN_DIR="$DEMO_PREFIX/bin"
RUN_LOG_DIR="$DEMO_BIN_DIR/dx11-core-scene-logs"
ROOT_TOOLCHAIN="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake"
WINE_PREFIX="$ROOT_DIR/test/artifacts/wineprefix-d3d11"
ROOT_D3D11_PROXY_DLL="$ROOT_BUILD_DIR/d3d11.dll"
DXMT_REPO_ROOT="${APITRACE_DXMT_REPO_ROOT:-$ROOT_DIR/../dxmt}"
DXMT_BUILD_DIR="${APITRACE_DXMT_BUILD_DIR:-$DXMT_REPO_ROOT/build-gs-native-builtin}"
DXMT_STAGE_DIR="${APITRACE_DXMT_STAGE_DIR:-$ROOT_DIR/test/artifacts/dxmt-runtime-d3d11}"
DXMT_RUNTIME_ROOT=""
DXMT_D3D11_DLL=""
DXMT_D3D12_DLL=""
DXMT_D3D12CORE_DLL=""
DXMT_DXGI_DLL=""
DXMT_WINEMETAL_DLL=""
DXMT_UNIX_DIR=""
D3D_COMPILER_DLL="$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-windows/d3dcompiler_47.dll"
VISUAL_WINDOW_TITLE="${APITRACE_VISUAL_WINDOW_TITLE:-apitrace test demo}"
VISUAL_DESKTOP="${APITRACE_WINE_VIRTUAL_DESKTOP:-apitrace,1400x900}"

if [ -n "${APITRACE_VISUAL_CHECK:-}" ]; then
    VISUAL_CHECK="$APITRACE_VISUAL_CHECK"
else
    VISUAL_CHECK=0
fi

SCENES="${APITRACE_D3D11_SCENES:-smoke_triangle indexed_instancing textured_quad depth_blend_scissor offscreen_copy_composite mip_sampling msaa_resolve}"
GENERATED_TRACE_BUNDLES=""

cleanup_generated_trace_bundles() {
    for bundle in $GENERATED_TRACE_BUNDLES; do
        rm -rf "$bundle"
    done
}

trap cleanup_generated_trace_bundles EXIT INT TERM

capture_visual_window() {
    window_title="$1"
    screenshot_path="$2"
    attempt=0
    while [ "$attempt" -lt 10 ]; do
        window_id="$(swift - "$window_title" <<'SWIFT'
import Foundation
import CoreGraphics

let targetTitle = CommandLine.arguments[1].lowercased()
let windows = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
var chosenId = 0
var chosenNumber = Int.min

for window in windows {
    let owner = (window[kCGWindowOwnerName as String] as? String ?? "").lowercased()
    let title = (window[kCGWindowName as String] as? String ?? "").lowercased()
    guard owner.contains("wine"), title == targetTitle else {
        continue
    }

    let number = window[kCGWindowNumber as String] as? Int ?? 0
    if number > chosenNumber {
        chosenNumber = number
        chosenId = number
    }
}

if chosenId != 0 {
    print(chosenId)
}
SWIFT
)"

        if [ -n "$window_id" ]; then
            capture_attempt=0
            while [ "$capture_attempt" -lt 10 ]; do
                if ! screencapture -x -l "$window_id" "$screenshot_path"; then
                    capture_attempt="$(( capture_attempt + 1 ))"
                    sleep 1
                    continue
                fi
                if validate_visual_window "$screenshot_path" >/dev/null 2>&1; then
                    return 0
                fi
                capture_attempt="$(( capture_attempt + 1 ))"
                sleep 1
            done
            return 1
        fi

        attempt="$(( attempt + 1 ))"
        sleep 1
    done

    echo "failed to locate visual window: $window_title" >&2
    return 1
}

validate_visual_window() {
    screenshot_path="$1"

    swift - "$screenshot_path" <<'SWIFT'
import AppKit
import Foundation

let screenshotPath = CommandLine.arguments[1]
let url = URL(fileURLWithPath: screenshotPath)
let data = try Data(contentsOf: url)
guard let rep = NSBitmapImageRep(data: data) else {
    fputs("failed to decode screenshot\n", stderr)
    exit(1)
}

var colorfulPixelCount = 0
for y in 0..<rep.pixelsHigh {
    for x in 0..<rep.pixelsWide {
        guard let raw = rep.colorAt(x: x, y: y) else {
            continue
        }
        let color = raw.usingColorSpace(.deviceRGB) ?? raw
        let red = Double(color.redComponent)
        let green = Double(color.greenComponent)
        let blue = Double(color.blueComponent)
        let alpha = Double(color.alphaComponent)
        let maxChannel = red > green ? (red > blue ? red : blue) : (green > blue ? green : blue)
        let minChannel = red < green ? (red < blue ? red : blue) : (green < blue ? green : blue)

        if alpha > 0.9 && (maxChannel - minChannel) > 0.15 && maxChannel > 0.25 {
            colorfulPixelCount += 1
        }
    }
}

if colorfulPixelCount < 1000 {
    fputs("captured visual window did not contain enough colorful pixels to prove the triangle rendered\n", stderr)
    exit(1)
}

print("visual colorful pixels: \(colorfulPixelCount)")
SWIFT
}

resolve_mingw_runtime() {
    dll_name="$1"
    stdlib_path="$(x86_64-w64-mingw32-g++ -print-file-name=libstdc++-6.dll)"
    if [ "$stdlib_path" = "libstdc++-6.dll" ] || [ ! -f "$stdlib_path" ]; then
        stdlib_path=""
    fi
    if [ -n "$stdlib_path" ]; then
        toolchain_lib_dir="$(dirname "$stdlib_path")"
        toolchain_bin_dir="$(CDPATH= cd -- "$toolchain_lib_dir/../bin" && pwd)"
    else
        toolchain_lib_dir=""
        toolchain_bin_dir=""
    fi

    case "$dll_name" in
        libgcc_s_seh-1.dll)
            candidate="$(x86_64-w64-mingw32-g++ -print-file-name="$dll_name")"
            if [ "$candidate" = "$dll_name" ]; then
                candidate=""
            fi
            ;;
        libstdc++-6.dll)
            candidate="$stdlib_path"
            ;;
        libwinpthread-1.dll)
            candidate="$toolchain_bin_dir/$dll_name"
            ;;
        *)
            candidate=""
            ;;
    esac

    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi

    printf '%s\n' ""
    return 1
}

resolve_dxmt_stage_prefix() {
    meson introspect "$DXMT_BUILD_DIR" --buildoptions | python3 -c 'import json, os, sys
data = json.load(sys.stdin)
prefix = ""
for item in data:
    if item.get("name") == "prefix":
        prefix = item.get("value") or ""
        break
if not prefix:
    sys.exit(1)
print(os.path.normpath(sys.argv[1] + prefix))
' "$DXMT_STAGE_DIR"
}

stage_dxmt_runtime() {
    if [ ! -f "$DXMT_BUILD_DIR/build.ninja" ]; then
        echo "missing DXMT build directory: $DXMT_BUILD_DIR" >&2
        exit 1
    fi

    rm -rf "$DXMT_STAGE_DIR"
    meson compile -C "$DXMT_BUILD_DIR"
    DESTDIR="$DXMT_STAGE_DIR" meson install -C "$DXMT_BUILD_DIR" --tags runtime,nvext

    DXMT_RUNTIME_ROOT="$(resolve_dxmt_stage_prefix)"
    DXMT_D3D11_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/d3d11.dll"
    DXMT_D3D12_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12.dll"
    DXMT_D3D12CORE_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12core.dll"
    DXMT_DXGI_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/dxgi.dll"
    DXMT_WINEMETAL_DLL="$DXMT_RUNTIME_ROOT/x86_64-windows/winemetal.dll"
    DXMT_UNIX_DIR="$DXMT_RUNTIME_ROOT/x86_64-unix"
}

if [ ! -x "$WINE_BIN" ]; then
    echo "missing wine binary: $WINE_BIN" >&2
    exit 1
fi

stage_dxmt_runtime

for required_path in "$DXMT_D3D11_DLL" "$DXMT_D3D12_DLL" "$DXMT_D3D12CORE_DLL" "$DXMT_DXGI_DLL" "$DXMT_WINEMETAL_DLL" "$D3D_COMPILER_DLL"; do
    if [ ! -f "$required_path" ]; then
        echo "missing D3D11 validation dependency: $required_path" >&2
        exit 1
    fi
done

if [ -x "$WINESERVER_BIN" ]; then
    WINEPREFIX="$WINE_PREFIX" WINEARCH="win64" "$WINESERVER_BIN" -k >/dev/null 2>&1 || true
fi

rm -rf "$ROOT_BUILD_DIR" "$TEST_BUILD_DIR" "$DEMO_PREFIX" "$RUN_LOG_DIR"

cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT_BUILD_DIR"

if [ ! -f "$ROOT_D3D11_PROXY_DLL" ]; then
    echo "missing D3D11 proxy DLL: $ROOT_D3D11_PROXY_DLL" >&2
    exit 1
fi

cmake -S "$ROOT_DIR/test" -B "$TEST_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$TEST_BUILD_DIR"
cmake --install "$TEST_BUILD_DIR" --prefix "$DEMO_PREFIX"

cp "$ROOT_D3D11_PROXY_DLL" "$DEMO_BIN_DIR/d3d11.dll"
cp "$DXMT_D3D12_DLL" "$DEMO_BIN_DIR/d3d12.dll"
cp "$DXMT_D3D12CORE_DLL" "$DEMO_BIN_DIR/d3d12core.dll"
cp "$DXMT_DXGI_DLL" "$DEMO_BIN_DIR/dxgi.dll"
cp "$DXMT_WINEMETAL_DLL" "$DEMO_BIN_DIR/winemetal.dll"
cp "$D3D_COMPILER_DLL" "$DEMO_BIN_DIR/d3dcompiler_47.dll"
for runtime_dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    runtime_path="$(resolve_mingw_runtime "$runtime_dll" || true)"
    if [ -z "$runtime_path" ] || [ ! -f "$runtime_path" ]; then
        echo "missing MinGW runtime DLL: $runtime_dll" >&2
        exit 1
    fi
    cp "$runtime_path" "$DEMO_BIN_DIR/$runtime_dll"
done

if [ "$VISUAL_CHECK" != "0" ]; then
    export APITRACE_TRIANGLE_MAX_FRAMES=300
else
    export APITRACE_TRIANGLE_MAX_FRAMES=36
fi
export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d11,d3d12,d3d12core,dxgi,winemetal=n,b"
export WINEDEBUG="-all"
export WINEARCH="win64"
export WINEPREFIX="$WINE_PREFIX"
export APITRACE_DOWNSTREAM_D3D11="$DXMT_D3D11_DLL"
export WINEDLLPATH="$DXMT_RUNTIME_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"

"$WINE_BIN" wineboot -u >/dev/null 2>&1 || true

if [ -x "$WINESERVER_BIN" ]; then
    "$WINESERVER_BIN" -w >/dev/null 2>&1 || true
fi

mkdir -p "$RUN_LOG_DIR"

run_scene() {
    scene="$1"
    run_log="$RUN_LOG_DIR/$scene-run.log"
    visual_screenshot="$RUN_LOG_DIR/$scene-visual.png"
    trace_bundle="$RUN_LOG_DIR/$scene.apitrace"
    GENERATED_TRACE_BUNDLES="$GENERATED_TRACE_BUNDLES $trace_bundle"

    rm -rf "$run_log" "$visual_screenshot" "$trace_bundle"

    if [ "$VISUAL_CHECK" != "0" ]; then
        APITRACE_TRACE_BUNDLE="$trace_bundle" "$WINE_BIN" explorer "/desktop=$VISUAL_DESKTOP" "$DEMO_BIN_DIR/apitrace_test_demo.exe" --dx dx11 --scene "$scene" >"$run_log" 2>&1 &
        wine_pid="$!"
        (
            capture_visual_window "$VISUAL_WINDOW_TITLE" "$visual_screenshot"
        ) &
        capture_pid="$!"
    else
        APITRACE_TRACE_BUNDLE="$trace_bundle" "$WINE_BIN" "$DEMO_BIN_DIR/apitrace_test_demo.exe" --dx dx11 --scene "$scene" 2>&1 | tee "$run_log"
        wine_pid=""
        capture_pid=""
    fi

    if [ -n "$wine_pid" ]; then
        deadline="$(( $(date +%s) + 90 ))"
        while kill -0 "$wine_pid" 2>/dev/null; do
            if [ "$(date +%s)" -ge "$deadline" ]; then
                kill "$wine_pid" >/dev/null 2>&1 || true
                wait "$wine_pid" >/dev/null 2>&1 || true
                echo "wine demo timed out for scene: $scene" >&2
                exit 1
            fi
            sleep 1
        done

        if ! wait "$wine_pid"; then
            echo "wine demo failed for scene: $scene" >&2
            cat "$run_log" >&2
            exit 1
        fi
    fi

    if [ -n "$capture_pid" ]; then
        wait "$capture_pid"
        if [ ! -f "$visual_screenshot" ]; then
            echo "missing visual screenshot: $visual_screenshot" >&2
            exit 1
        fi
        validate_visual_window "$visual_screenshot"
    fi

    if ! grep -F "scene pass: $scene" "$run_log" >/dev/null; then
        echo "$scene: missing pass result" >&2
        cat "$run_log" >&2
        exit 1
    fi
    if ! grep -F "failed=0" "$run_log" >/dev/null; then
        echo "$scene: unexpected failure summary" >&2
        cat "$run_log" >&2
        exit 1
    fi

    if [ -z "$trace_bundle" ] || [ ! -f "$trace_bundle/callstream.jsonl" ]; then
        echo "$scene: missing D3D11 trace bundle" >&2
        exit 1
    fi

    present_call_count="$(grep -c '"function":"IDXGISwapChain::Present"' "$trace_bundle/callstream.jsonl" || true)"
    present_boundary_count="$(grep -c '"boundary":"Present"' "$trace_bundle/callstream.jsonl" || true)"
    if [ "$present_call_count" -ne "$APITRACE_TRIANGLE_MAX_FRAMES" ] || [ "$present_boundary_count" -ne "$APITRACE_TRIANGLE_MAX_FRAMES" ]; then
        echo "$scene: D3D11 present semantic count mismatch calls=$present_call_count boundaries=$present_boundary_count expected=$APITRACE_TRIANGLE_MAX_FRAMES" >&2
        exit 1
    fi
    if ! grep -F '"function":"IDXGISwapChain::Present"' "$trace_bundle/callstream.jsonl" | grep -F '"frame_index":0' >/dev/null || \
       ! grep -F '"function":"IDXGISwapChain::Present"' "$trace_bundle/callstream.jsonl" | grep -F '"sync_interval":' >/dev/null || \
       ! grep -F '"function":"IDXGISwapChain::Present"' "$trace_bundle/callstream.jsonl" | grep -F '"flags":' >/dev/null || \
       ! grep -F '"boundary":"Present"' "$trace_bundle/callstream.jsonl" | grep -F '"sync_interval":' >/dev/null || \
       ! grep -F '"boundary":"Present"' "$trace_bundle/callstream.jsonl" | grep -F '"flags":' >/dev/null; then
        echo "$scene: missing D3D11 present semantic payloads" >&2
        exit 1
    fi

    echo "scene run log: $run_log"
    echo "scene trace bundle: $trace_bundle"
    if [ "$VISUAL_CHECK" != "0" ]; then
        echo "scene visual screenshot: $visual_screenshot"
    fi
}

for scene in $SCENES; do
    run_scene "$scene"
done
