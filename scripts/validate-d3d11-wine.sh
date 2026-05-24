#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="$ROOT_DIR/../wine-enviroment/bin/wine"
WINESERVER_BIN="$ROOT_DIR/../wine-enviroment/bin/wineserver"
TEST_BUILD_DIR="$ROOT_DIR/test/build/windows-x86_64"
DEMO_PREFIX="$ROOT_DIR/test/artifacts/windows-x86_64/demo"
DEMO_BIN_DIR="$DEMO_PREFIX/bin"
RUN_LOG_DIR="$DEMO_BIN_DIR/dx11-core-scene-logs"
ROOT_TOOLCHAIN="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake"
WINE_PREFIX="$ROOT_DIR/test/artifacts/wineprefix-d3d11"
D3DMETAL_ROOT="${APITRACE_D3DMETAL_ROOT:-$ROOT_DIR/../wine-enviroment/D3DMetal}"
D3DMETAL_D3D11_DLL="$D3DMETAL_ROOT/wine/x86_64-windows/d3d11.dll"
D3DMETAL_D3D12_DLL="$D3DMETAL_ROOT/wine/x86_64-windows/d3d12.dll"
D3DMETAL_DXGI_DLL="$D3DMETAL_ROOT/wine/x86_64-windows/dxgi.dll"
D3DMETAL_WINE_ROOT="$D3DMETAL_ROOT/wine"
D3DMETAL_UNIX_DIR="$D3DMETAL_ROOT/wine/x86_64-unix"
D3DMETAL_EXTERNAL_DIR="$D3DMETAL_ROOT/external"
D3D12CORE_DLL="$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-windows/d3d12core.dll"
D3D_COMPILER_DLL="$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-windows/d3dcompiler_47.dll"
VISUAL_WINDOW_TITLE="${APITRACE_VISUAL_WINDOW_TITLE:-apitrace test demo}"
VISUAL_DESKTOP="${APITRACE_WINE_VIRTUAL_DESKTOP:-apitrace,1400x900}"

if [ -n "${APITRACE_VISUAL_CHECK:-}" ]; then
    VISUAL_CHECK="$APITRACE_VISUAL_CHECK"
else
    VISUAL_CHECK=0
fi

SCENES="${APITRACE_D3D11_SCENES:-smoke_triangle indexed_instancing textured_quad depth_blend_scissor offscreen_copy_composite mip_sampling msaa_resolve}"

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

if [ ! -x "$WINE_BIN" ]; then
    echo "missing wine binary: $WINE_BIN" >&2
    exit 1
fi

if [ ! -f "$D3DMETAL_D3D11_DLL" ]; then
    echo "missing D3DMetal d3d11.dll: $D3DMETAL_D3D11_DLL" >&2
    exit 1
fi

if [ ! -f "$D3DMETAL_DXGI_DLL" ]; then
    echo "missing D3DMetal dxgi.dll: $D3DMETAL_DXGI_DLL" >&2
    exit 1
fi

if [ ! -f "$D3DMETAL_D3D12_DLL" ]; then
    echo "missing D3DMetal d3d12.dll: $D3DMETAL_D3D12_DLL" >&2
    exit 1
fi

if [ ! -f "$D3D12CORE_DLL" ]; then
    echo "missing d3d12core.dll: $D3D12CORE_DLL" >&2
    exit 1
fi

if [ ! -f "$D3D_COMPILER_DLL" ]; then
    echo "missing d3dcompiler_47.dll: $D3D_COMPILER_DLL" >&2
    exit 1
fi

if [ -x "$WINESERVER_BIN" ]; then
    WINEPREFIX="$WINE_PREFIX" WINEARCH="win64" "$WINESERVER_BIN" -k >/dev/null 2>&1 || true
fi

rm -rf "$TEST_BUILD_DIR" "$DEMO_PREFIX" "$RUN_LOG_DIR"

cmake -S "$ROOT_DIR/test" -B "$TEST_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$TEST_BUILD_DIR"
cmake --install "$TEST_BUILD_DIR" --prefix "$DEMO_PREFIX"

cp "$D3DMETAL_D3D11_DLL" "$DEMO_BIN_DIR/d3d11.dll"
cp "$D3DMETAL_D3D12_DLL" "$DEMO_BIN_DIR/d3d12.dll"
cp "$D3D12CORE_DLL" "$DEMO_BIN_DIR/d3d12core.dll"
cp "$D3DMETAL_DXGI_DLL" "$DEMO_BIN_DIR/dxgi.dll"
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
export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d11,d3d12,d3d12core,dxgi=n,b"
export WINEDEBUG="-all"
export WINEARCH="win64"
export WINEPREFIX="$WINE_PREFIX"
export WINEDLLPATH="$D3DMETAL_WINE_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
export DYLD_FALLBACK_LIBRARY_PATH="$D3DMETAL_EXTERNAL_DIR:$D3DMETAL_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"

"$WINE_BIN" wineboot -u >/dev/null 2>&1 || true

if [ -x "$WINESERVER_BIN" ]; then
    "$WINESERVER_BIN" -w >/dev/null 2>&1 || true
fi

mkdir -p "$RUN_LOG_DIR"

run_scene() {
    scene="$1"
    run_log="$RUN_LOG_DIR/$scene-run.log"
    visual_screenshot="$RUN_LOG_DIR/$scene-visual.png"

    rm -rf "$run_log" "$visual_screenshot"

    if [ "$VISUAL_CHECK" != "0" ]; then
        "$WINE_BIN" explorer "/desktop=$VISUAL_DESKTOP" "$DEMO_BIN_DIR/apitrace_test_demo.exe" --dx dx11 --scene "$scene" >"$run_log" 2>&1 &
        wine_pid="$!"
        (
            capture_visual_window "$VISUAL_WINDOW_TITLE" "$visual_screenshot"
        ) &
        capture_pid="$!"
    else
        "$WINE_BIN" "$DEMO_BIN_DIR/apitrace_test_demo.exe" --dx dx11 --scene "$scene" 2>&1 | tee "$run_log"
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

    echo "scene run log: $run_log"
    if [ "$VISUAL_CHECK" != "0" ]; then
        echo "scene visual screenshot: $visual_screenshot"
    fi
}

for scene in $SCENES; do
    run_scene "$scene"
done
