#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="$ROOT_DIR/../wine-enviroment/bin/wine"
WINESERVER_BIN="$ROOT_DIR/../wine-enviroment/bin/wineserver"
ROOT_BUILD_DIR="$ROOT_DIR/build/windows-cross"
RETRACE_PREFIX="$ROOT_DIR/test/artifacts/windows-x86_64/retrace"
RETRACE_BIN_DIR="$RETRACE_PREFIX/bin"
RUN_LOG="$RETRACE_BIN_DIR/retrace-d3d11-run.log"
VISUAL_RUN_LOG="$RETRACE_BIN_DIR/retrace-d3d11-visual-run.log"
VISUAL_SCREENSHOT="$RETRACE_BIN_DIR/retrace-d3d11-visual.png"
ROOT_TOOLCHAIN="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake"
WINE_PREFIX="$ROOT_DIR/test/artifacts/wineprefix-d3d11-retrace"
FIXTURE_TRACE_DIR="$ROOT_DIR/test/fixtures/retrace/d3d11-triangle/triangle-d3d11.apitrace"
DXMT_D3D11_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/d3d11/d3d11.dll"
DXMT_DXGI_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/dxgi/dxgi.dll"
DXMT_WINEMETAL_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/winemetal.dll"
DXMT_UNIX_DIR="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/unix"
DXMT_PACKAGE_ROOT="$HOME/Library/Application Support/com.gamemac.test/wine-engine/downloads/dxmt-v0.80"
DXMT_WINE_ROOT=""
VISUAL_WINDOW_TITLE="${APITRACE_RETRACE_WINDOW_TITLE:-apitrace retrace d3d11}"
VISUAL_DESKTOP="${APITRACE_WINE_VIRTUAL_DESKTOP:-apitrace-retrace,1400x900}"

if [ -n "${APITRACE_VISUAL_CHECK:-}" ]; then
    VISUAL_CHECK="$APITRACE_VISUAL_CHECK"
elif [ "$(uname -s)" = "Darwin" ]; then
    VISUAL_CHECK=1
else
    VISUAL_CHECK=0
fi

wine_path() {
    printf 'Z:%s' "$(printf '%s' "$1" | sed 's|/|\\\\|g')"
}

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
            screencapture -x -l "$window_id" "$screenshot_path"
            return 0
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
    fputs("captured visual window did not contain enough colorful pixels to prove the retrace rendered\n", stderr)
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

if [ ! -d "$FIXTURE_TRACE_DIR" ]; then
    echo "missing retrace fixture: $FIXTURE_TRACE_DIR" >&2
    exit 1
fi

if [ -d "$DXMT_PACKAGE_ROOT/wine" ]; then
    DXMT_WINE_ROOT="$DXMT_PACKAGE_ROOT/wine"
    DXMT_D3D11_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/d3d11.dll"
    DXMT_DXGI_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/dxgi.dll"
    DXMT_WINEMETAL_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/winemetal.dll"
    DXMT_UNIX_DIR="$DXMT_PACKAGE_ROOT/wine/x86_64-unix"
fi

for required_path in "$DXMT_D3D11_DLL" "$DXMT_DXGI_DLL" "$DXMT_WINEMETAL_DLL"; do
    if [ ! -f "$required_path" ]; then
        echo "missing DXMT runtime: $required_path" >&2
        exit 1
    fi
done

if [ -x "$WINESERVER_BIN" ]; then
    WINEPREFIX="$WINE_PREFIX" WINEARCH="win64" "$WINESERVER_BIN" -k >/dev/null 2>&1 || true
fi

rm -rf "$ROOT_BUILD_DIR" "$RETRACE_PREFIX" "$VISUAL_SCREENSHOT"

cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT_BUILD_DIR"
cmake --install "$ROOT_BUILD_DIR" --prefix "$RETRACE_PREFIX"

cp "$DXMT_D3D11_DLL" "$RETRACE_BIN_DIR/d3d11.dll"
cp "$DXMT_DXGI_DLL" "$RETRACE_BIN_DIR/dxgi.dll"
cp "$DXMT_WINEMETAL_DLL" "$RETRACE_BIN_DIR/winemetal.dll"
for runtime_dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    runtime_path="$(resolve_mingw_runtime "$runtime_dll" || true)"
    if [ -z "$runtime_path" ] || [ ! -f "$runtime_path" ]; then
        echo "missing MinGW runtime DLL: $runtime_dll" >&2
        exit 1
    fi
    cp "$runtime_path" "$RETRACE_BIN_DIR/$runtime_dll"
done

export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d11,dxgi,winemetal=n,b"
export WINEDEBUG="-all"
export WINEARCH="win64"
export WINEPREFIX="$WINE_PREFIX"
export APITRACE_RETRACE_WINDOW_TITLE="$VISUAL_WINDOW_TITLE"
if [ -n "$DXMT_WINE_ROOT" ]; then
    export WINEDLLPATH="$DXMT_WINE_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
else
    export WINEDLLPATH="$ROOT_DIR/../wine-enviroment/lib/wine"
fi
export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"

if [ ! -f "$WINE_PREFIX/system.reg" ]; then
    "$WINE_BIN" wineboot -u >/dev/null 2>&1 || true
fi

fixture_trace_path="$(wine_path "$FIXTURE_TRACE_DIR")"
retrace_bin_path="$(wine_path "$RETRACE_BIN_DIR")"

if [ "$VISUAL_CHECK" != "0" ]; then
    export APITRACE_RETRACE_SHOW_WINDOW=1
    "$WINE_BIN" explorer "/desktop=$VISUAL_DESKTOP" "$RETRACE_BIN_DIR/retrace.exe" "$fixture_trace_path" >"$VISUAL_RUN_LOG" 2>&1 &
    wine_pid="$!"
    (
        capture_visual_window "$VISUAL_WINDOW_TITLE" "$VISUAL_SCREENSHOT"
    ) &
    capture_pid="$!"
    deadline="$(( $(date +%s) + 90 ))"
    while kill -0 "$wine_pid" 2>/dev/null; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            kill "$wine_pid" >/dev/null 2>&1 || true
            wait "$wine_pid" >/dev/null 2>&1 || true
            echo "wine retrace visual pass timed out" >&2
            exit 1
        fi
        sleep 1
    done

    set +e
    wait "$wine_pid"
    wine_status="$?"
    set -e
    if [ "$wine_status" -ne 0 ]; then
        echo "retrace.exe visual pass exited with code $wine_status" >&2
        cat "$VISUAL_RUN_LOG" >&2
        exit 1
    fi

    wait "$capture_pid"
    if [ ! -f "$VISUAL_SCREENSHOT" ]; then
        echo "missing visual screenshot: $VISUAL_SCREENSHOT" >&2
        exit 1
    fi
    validate_visual_window "$VISUAL_SCREENSHOT"
fi

export APITRACE_RETRACE_SHOW_WINDOW=0
"$WINE_BIN" cmd /c "cd /d $retrace_bin_path && retrace.exe $fixture_trace_path > retrace-d3d11-run.log 2>&1"

python3 - "$RUN_LOG" <<'PY'
import pathlib
import re
import sys

run_log = pathlib.Path(sys.argv[1])
text = run_log.read_text(encoding="utf-8", errors="replace")

def require_stat(name: str, expected: int) -> None:
    match = re.search(rf"{re.escape(name)}:\s*(\d+)", text)
    if not match:
        raise SystemExit(f"missing stat in retrace output: {name}")
    value = int(match.group(1))
    if value != expected:
        raise SystemExit(f"unexpected {name}: expected {expected}, got {value}")

backend_match = re.search(r"backend:\s*(.+)", text)
if not backend_match:
    raise SystemExit("missing backend line in retrace output")
backend = backend_match.group(1).strip()
if backend != "translation-layer-d3d11-dxmt":
    raise SystemExit(f"unexpected backend: {backend}")

require_stat("frames_seen", 300)
require_stat("presents_seen", 300)

calls_match = re.search(r"calls_replayed:\s*(\d+)", text)
if not calls_match or int(calls_match.group(1)) <= 0:
    raise SystemExit("calls_replayed was missing or zero")

print("validated retrace output:", run_log)
PY

echo "run log: $RUN_LOG"
echo "fixture trace: $FIXTURE_TRACE_DIR"
if [ "$VISUAL_CHECK" != "0" ]; then
    echo "visual run log: $VISUAL_RUN_LOG"
    echo "visual screenshot: $VISUAL_SCREENSHOT"
fi
