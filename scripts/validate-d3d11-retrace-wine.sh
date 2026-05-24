#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="$ROOT_DIR/../wine-enviroment/bin/wine"
WINESERVER_BIN="$ROOT_DIR/../wine-enviroment/bin/wineserver"
ROOT_BUILD_DIR="$ROOT_DIR/build/windows-cross"
RETRACE_PREFIX="$ROOT_DIR/test/artifacts/windows-x86_64/retrace"
RETRACE_BIN_DIR="$RETRACE_PREFIX/bin"
RUN_LOG_DIR="$RETRACE_BIN_DIR/retrace-d3d11-logs"
ROOT_TOOLCHAIN="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake"
WINE_PREFIX="$ROOT_DIR/test/artifacts/wineprefix-d3d11-retrace"
DXMT_D3D11_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/d3d11/d3d11.dll"
DXMT_DXGI_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/dxgi/dxgi.dll"
DXMT_WINEMETAL_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/winemetal.dll"
DXMT_UNIX_DIR="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/unix"
DXMT_PACKAGE_ROOT="$HOME/Library/Application Support/com.gamemac.test/wine-engine/downloads/dxmt-v0.80"
DXMT_WINE_ROOT=""
VISUAL_WINDOW_TITLE="${APITRACE_RETRACE_WINDOW_TITLE:-apitrace retrace d3d11}"
VISUAL_DESKTOP="${APITRACE_WINE_VIRTUAL_DESKTOP:-apitrace-retrace,1400x900}"
SCENES="${APITRACE_RETRACE_SCENES:-smoke_triangle indexed_instancing textured_quad depth_blend_scissor offscreen_copy_composite mip_sampling msaa_resolve}"

if [ -n "${APITRACE_VISUAL_CHECK:-}" ]; then
    VISUAL_CHECK="$APITRACE_VISUAL_CHECK"
else
    VISUAL_CHECK=0
fi
ACCEPT_VISUAL_SNAPSHOT="${APITRACE_ACCEPT_VISUAL_SNAPSHOT:-0}"

wine_path() {
    printf 'Z:%s' "$(printf '%s' "$1" | sed 's|/|\\\\|g')"
}

scene_fixture_dir() {
    scene_name="$1"
    case "$scene_name" in
        smoke_triangle)
            if [ -d "$ROOT_DIR/test/fixtures/retrace/d3d11-smoke_triangle/smoke_triangle-d3d11.apitrace" ]; then
                printf '%s\n' "$ROOT_DIR/test/fixtures/retrace/d3d11-smoke_triangle/smoke_triangle-d3d11.apitrace"
                return 0
            fi
            printf '%s\n' "$ROOT_DIR/test/fixtures/retrace/d3d11-triangle/triangle-d3d11.apitrace"
            ;;
        *)
            printf '%s\n' "$ROOT_DIR/test/fixtures/retrace/d3d11-$scene_name/$scene_name-d3d11.apitrace"
            ;;
    esac
}

scene_reference_visual() {
    scene_name="$1"
    case "$scene_name" in
        smoke_triangle)
            if [ -f "$ROOT_DIR/test/fixtures/retrace/d3d11-smoke_triangle/smoke_triangle-d3d11-visual.png" ]; then
                printf '%s\n' "$ROOT_DIR/test/fixtures/retrace/d3d11-smoke_triangle/smoke_triangle-d3d11-visual.png"
                return 0
            fi
            printf '%s\n' "$ROOT_DIR/test/fixtures/retrace/d3d11-triangle/triangle-d3d11-visual.png"
            ;;
        *)
            printf '%s\n' "$ROOT_DIR/test/fixtures/retrace/d3d11-$scene_name/$scene_name-d3d11-visual.png"
            ;;
    esac
}

scene_expected_stats() {
    trace_dir="$1"
    python3 - "$trace_dir" <<'PY'
import json
import pathlib
import sys

trace_dir = pathlib.Path(sys.argv[1])
callstream = trace_dir / "callstream.jsonl"
frames = 0
presents = 0
with callstream.open("r", encoding="utf-8") as stream:
    for line in stream:
        line = line.strip()
        if not line:
            continue
        record = json.loads(line)
        if record.get("record_kind") != "boundary":
            continue
        boundary = record.get("boundary")
        payload = record.get("payload", {})
        if boundary == "Frame" and payload.get("label") == "FrameBegin":
            frames += 1
        elif boundary == "Present":
            presents += 1
print(f"{frames} {presents}")
PY
}

capture_visual_window() {
    window_title="$1"
    screenshot_path="$2"
    reference_path="$3"
    attempt=0
    while [ "$attempt" -lt 40 ]; do
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
            if screencapture -x -l "$window_id" "$screenshot_path"; then
                if validate_visual_window "$screenshot_path" >/dev/null 2>&1; then
                    return 0
                fi
            fi
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

validate_visual_against_reference() {
    scene_name="$1"
    reference_path="$2"
    screenshot_path="$3"

    swift - "$scene_name" "$reference_path" "$screenshot_path" <<'SWIFT'
import AppKit
import Foundation

let sceneName = CommandLine.arguments[1]
let referencePath = CommandLine.arguments[2]
let screenshotPath = CommandLine.arguments[3]

func loadImage(_ path: String) throws -> NSBitmapImageRep {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    guard let rep = NSBitmapImageRep(data: data) else {
        throw NSError(domain: "visual-compare", code: 1, userInfo: [NSLocalizedDescriptionKey: "failed to decode \(path)"])
    }
    return rep
}

let reference = try loadImage(referencePath)
let screenshot = try loadImage(screenshotPath)

let referenceWidth = reference.pixelsWide
let referenceHeight = reference.pixelsHigh
let screenshotWidth = screenshot.pixelsWide
let screenshotHeight = screenshot.pixelsHigh

let referenceStartX = max(0, Int(Double(referenceWidth) * 0.12))
let referenceEndX = min(referenceWidth, Int(Double(referenceWidth) * 0.88))
let referenceStartY = max(0, Int(Double(referenceHeight) * 0.18))
let referenceEndY = min(referenceHeight, Int(Double(referenceHeight) * 0.90))

let screenshotStartX = max(0, Int(Double(screenshotWidth) * 0.12))
let screenshotEndX = min(screenshotWidth, Int(Double(screenshotWidth) * 0.88))
let screenshotStartY = max(0, Int(Double(screenshotHeight) * 0.18))
let screenshotEndY = min(screenshotHeight, Int(Double(screenshotHeight) * 0.90))

let referenceCropWidth = referenceEndX - referenceStartX
let referenceCropHeight = referenceEndY - referenceStartY
let screenshotCropWidth = screenshotEndX - screenshotStartX
let screenshotCropHeight = screenshotEndY - screenshotStartY

let sampleWidth = min(referenceCropWidth, screenshotCropWidth)
let sampleHeight = min(referenceCropHeight, screenshotCropHeight)

var totalDiff = 0.0
var maxDiff = 0.0
var mismatchPixels = 0
var comparedPixels = 0

func mappedOffset(_ index: Int, _ sampleSize: Int, _ cropSize: Int) -> Int {
    if sampleSize <= 1 || cropSize <= 1 {
        return 0
    }
    return Int((Double(index) / Double(sampleSize - 1)) * Double(cropSize - 1))
}

for sampleY in 0..<sampleHeight {
    for sampleX in 0..<sampleWidth {
        let referenceX = referenceStartX + mappedOffset(sampleX, sampleWidth, referenceCropWidth)
        let referenceY = referenceStartY + mappedOffset(sampleY, sampleHeight, referenceCropHeight)
        let screenshotX = screenshotStartX + mappedOffset(sampleX, sampleWidth, screenshotCropWidth)
        let screenshotY = screenshotStartY + mappedOffset(sampleY, sampleHeight, screenshotCropHeight)
        guard
            let rawReference = reference.colorAt(x: referenceX, y: referenceY),
            let rawScreenshot = screenshot.colorAt(x: screenshotX, y: screenshotY)
        else {
            continue
        }
        let refColor = rawReference.usingColorSpace(.deviceRGB) ?? rawReference
        let shotColor = rawScreenshot.usingColorSpace(.deviceRGB) ?? rawScreenshot
        let diff =
            abs(Double(refColor.redComponent - shotColor.redComponent)) +
            abs(Double(refColor.greenComponent - shotColor.greenComponent)) +
            abs(Double(refColor.blueComponent - shotColor.blueComponent))
        let normalized = diff / 3.0
        totalDiff += normalized
        if normalized > maxDiff {
            maxDiff = normalized
        }
        if normalized > 0.10 {
            mismatchPixels += 1
        }
        comparedPixels += 1
    }
}

if comparedPixels == 0 {
    fputs("\(sceneName): visual comparison crop was empty\n", stderr)
    exit(1)
}

let averageDiff = totalDiff / Double(comparedPixels)
let mismatchRatio = Double(mismatchPixels) / Double(comparedPixels)

let averageThreshold = 0.02
let mismatchThreshold = 0.05

if averageDiff > averageThreshold || mismatchRatio > mismatchThreshold {
    fputs(
        "\(sceneName): retrace visual output diverged from reference (avg_diff=\(averageDiff), mismatch_ratio=\(mismatchRatio), max_diff=\(maxDiff))\n",
        stderr
    )
    exit(1)
}

print("visual reference match: \(sceneName) avg_diff=\(averageDiff) mismatch_ratio=\(mismatchRatio) max_diff=\(maxDiff)")
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

rm -rf "$ROOT_BUILD_DIR" "$RETRACE_PREFIX"

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

retrace_bin_path="$(wine_path "$RETRACE_BIN_DIR")"
mkdir -p "$RUN_LOG_DIR"

validate_run_log() {
    run_log="$1"
    expected_frames="$2"
    expected_presents="$3"
    python3 - "$run_log" "$expected_frames" "$expected_presents" <<'PY'
import pathlib
import re
import sys

run_log = pathlib.Path(sys.argv[1])
expected_frames = int(sys.argv[2])
expected_presents = int(sys.argv[3])
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

require_stat("frames_seen", expected_frames)
require_stat("presents_seen", expected_presents)

calls_match = re.search(r"calls_replayed:\s*(\d+)", text)
if not calls_match or int(calls_match.group(1)) <= 0:
    raise SystemExit("calls_replayed was missing or zero")

print("validated retrace output:", run_log)
PY
}

for scene_name in $SCENES; do
    fixture_trace_dir="$(scene_fixture_dir "$scene_name")"
    if [ ! -d "$fixture_trace_dir" ]; then
        echo "missing retrace fixture for scene $scene_name: $fixture_trace_dir" >&2
        exit 1
    fi
    reference_visual="$(scene_reference_visual "$scene_name")"

    set -- $(scene_expected_stats "$fixture_trace_dir")
    expected_frames="$1"
    expected_presents="$2"
    if [ "$expected_frames" -le 0 ] || [ "$expected_presents" -le 0 ]; then
        echo "invalid expected stats for scene $scene_name: frames=$expected_frames presents=$expected_presents" >&2
        exit 1
    fi

    python3 - "$fixture_trace_dir" "$scene_name" <<'PY'
import json
import pathlib
import sys

trace_dir = pathlib.Path(sys.argv[1])
scene_name = sys.argv[2]
callstream = trace_dir / "callstream.jsonl"

def collect_paths(value):
    paths = []
    if isinstance(value, dict):
        for key, child in value.items():
            if key.endswith("_path") and isinstance(child, str):
                paths.append(child)
            else:
                paths.extend(collect_paths(child))
    elif isinstance(value, list):
        for child in value:
            paths.extend(collect_paths(child))
    return paths

referenced_paths = []
with callstream.open("r", encoding="utf-8") as stream:
    for line in stream:
        line = line.strip()
        if not line:
            continue
        referenced_paths.extend(collect_paths(json.loads(line).get("payload")))

if not any(path.startswith("pipelines/") for path in referenced_paths):
    raise SystemExit(f"{scene_name}: fixture trace is missing referenced pipeline assets")

for relative_path in referenced_paths:
    path = trace_dir / relative_path
    if not path.is_file():
        raise SystemExit(f"missing referenced fixture asset: {relative_path}")
    if path.stat().st_size == 0:
        raise SystemExit(f"empty referenced fixture asset: {relative_path}")
PY

    fixture_trace_path="$(wine_path "$fixture_trace_dir")"
    run_log="$RUN_LOG_DIR/$scene_name-run.log"
    visual_run_log="$RUN_LOG_DIR/$scene_name-visual-run.log"
    visual_screenshot="$RUN_LOG_DIR/$scene_name-visual.png"

    if [ "$VISUAL_CHECK" != "0" ]; then
        if [ "$ACCEPT_VISUAL_SNAPSHOT" = "0" ] && [ ! -f "$reference_visual" ]; then
            echo "missing reference visual for scene $scene_name: $reference_visual" >&2
            exit 1
        fi
        rm -f "$visual_screenshot"
        export APITRACE_RETRACE_SHOW_WINDOW=1
        "$WINE_BIN" explorer "/desktop=$VISUAL_DESKTOP" "$RETRACE_BIN_DIR/retrace.exe" "$fixture_trace_path" >"$visual_run_log" 2>&1 &
        wine_pid="$!"
        (
            capture_visual_window "$VISUAL_WINDOW_TITLE" "$visual_screenshot" "$reference_visual"
        ) &
        capture_pid="$!"
        deadline="$(( $(date +%s) + 90 ))"
        while kill -0 "$wine_pid" 2>/dev/null; do
            if [ "$(date +%s)" -ge "$deadline" ]; then
                kill "$wine_pid" >/dev/null 2>&1 || true
                wait "$wine_pid" >/dev/null 2>&1 || true
                echo "wine retrace visual pass timed out for scene $scene_name" >&2
                exit 1
            fi
            sleep 1
        done

        set +e
        wait "$wine_pid"
        wine_status="$?"
        set -e
        if [ "$wine_status" -ne 0 ]; then
            echo "retrace.exe visual pass exited with code $wine_status for scene $scene_name" >&2
            cat "$visual_run_log" >&2
            exit 1
        fi

        wait "$capture_pid"
        if [ ! -f "$visual_screenshot" ]; then
            echo "missing visual screenshot for scene $scene_name: $visual_screenshot" >&2
            exit 1
        fi
        validate_visual_window "$visual_screenshot"
        if [ "$ACCEPT_VISUAL_SNAPSHOT" = "0" ]; then
            validate_visual_against_reference "$scene_name" "$reference_visual" "$visual_screenshot"
        else
            echo "accepted visual snapshot for scene $scene_name: $visual_screenshot"
        fi
    fi

    export APITRACE_RETRACE_SHOW_WINDOW=0
    "$WINE_BIN" cmd /c "cd /d $retrace_bin_path && retrace.exe $fixture_trace_path > retrace-d3d11-logs/$scene_name-run.log 2>&1"
    validate_run_log "$run_log" "$expected_frames" "$expected_presents"

    echo "scene: $scene_name"
    echo "run log: $run_log"
    echo "fixture trace: $fixture_trace_dir"
    if [ "$VISUAL_CHECK" != "0" ]; then
        echo "visual run log: $visual_run_log"
        echo "visual screenshot: $visual_screenshot"
        echo "reference visual: $reference_visual"
    fi
done
