#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WINE_BIN="$ROOT_DIR/../wine-enviroment/bin/wine"
WINESERVER_BIN="$ROOT_DIR/../wine-enviroment/bin/wineserver"
ROOT_BUILD_DIR="$ROOT_DIR/build/windows-cross"
TEST_BUILD_DIR="$ROOT_DIR/test/build/windows-x86_64"
APITRACE_PREFIX="$ROOT_DIR/test/artifacts/windows-x86_64/apitrace"
DEMO_PREFIX="$ROOT_DIR/test/artifacts/windows-x86_64/demo"
DEMO_BIN_DIR="$DEMO_PREFIX/bin"
TRACE_DIR="$DEMO_BIN_DIR/triangle-d3d11.apitrace"
RUN_LOG="$DEMO_BIN_DIR/triangle-d3d11-run.log"
VISUAL_SCREENSHOT="$DEMO_BIN_DIR/triangle-d3d11-visual.png"
ROOT_TOOLCHAIN="$ROOT_DIR/test/toolchains/windows-x86_64-mingw.cmake"
DOWNSTREAM_DLL="$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-windows/d3d11.dll"
WINE_PREFIX="$ROOT_DIR/test/artifacts/wineprefix-d3d11"
DXMT_D3D11_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/d3d11/d3d11.dll"
DXMT_DXGI_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/dxgi/dxgi.dll"
DXMT_WINEMETAL_DLL="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/winemetal.dll"
DXMT_UNIX_DIR="$ROOT_DIR/../dxmt/build-gs-native-builtin/src/winemetal/unix"
DXMT_PACKAGE_ROOT="$HOME/Library/Application Support/com.gamemac.test/wine-engine/downloads/dxmt-v0.80"
DXMT_WINE_ROOT=""
VISUAL_WINDOW_TITLE="${APITRACE_VISUAL_WINDOW_TITLE:-apitrace triangle d3d11}"
VISUAL_DESKTOP="${APITRACE_WINE_VIRTUAL_DESKTOP:-apitrace,1400x900}"

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

if [ -d "$DXMT_PACKAGE_ROOT/wine" ]; then
    DXMT_WINE_ROOT="$DXMT_PACKAGE_ROOT/wine"
    DXMT_D3D11_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/d3d11.dll"
    DXMT_DXGI_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/dxgi.dll"
    DXMT_WINEMETAL_DLL="$DXMT_PACKAGE_ROOT/wine/x86_64-windows/winemetal.dll"
    DXMT_UNIX_DIR="$DXMT_PACKAGE_ROOT/wine/x86_64-unix"
fi

if [ ! -f "$DXMT_D3D11_DLL" ]; then
    echo "missing DXMT d3d11.dll: $DXMT_D3D11_DLL" >&2
    exit 1
fi

if [ ! -f "$DXMT_DXGI_DLL" ]; then
    echo "missing DXMT dxgi.dll: $DXMT_DXGI_DLL" >&2
    exit 1
fi

if [ ! -f "$DXMT_WINEMETAL_DLL" ]; then
    echo "missing DXMT winemetal.dll: $DXMT_WINEMETAL_DLL" >&2
    exit 1
fi

if [ -x "$WINESERVER_BIN" ]; then
    WINEPREFIX="$WINE_PREFIX" WINEARCH="win64" "$WINESERVER_BIN" -k >/dev/null 2>&1 || true
fi

rm -rf "$ROOT_BUILD_DIR" "$TEST_BUILD_DIR" "$APITRACE_PREFIX" "$DEMO_PREFIX" "$TRACE_DIR" "$VISUAL_SCREENSHOT"

cmake -S "$ROOT_DIR" -B "$ROOT_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT_BUILD_DIR"
cmake --install "$ROOT_BUILD_DIR" --prefix "$APITRACE_PREFIX"

cmake -S "$ROOT_DIR/test" -B "$TEST_BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug
cmake --build "$TEST_BUILD_DIR"
cmake --install "$TEST_BUILD_DIR" --prefix "$DEMO_PREFIX"

cp "$APITRACE_PREFIX/bin/d3d11.dll" "$DEMO_BIN_DIR/d3d11.dll"
cp "$DXMT_DXGI_DLL" "$DEMO_BIN_DIR/dxgi.dll"
cp "$DXMT_WINEMETAL_DLL" "$DEMO_BIN_DIR/winemetal.dll"
for runtime_dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    runtime_path="$(resolve_mingw_runtime "$runtime_dll" || true)"
    if [ -z "$runtime_path" ] || [ ! -f "$runtime_path" ]; then
        echo "missing MinGW runtime DLL: $runtime_dll" >&2
        exit 1
    fi
    cp "$runtime_path" "$DEMO_BIN_DIR/$runtime_dll"
done

export APITRACE_DOWNSTREAM_D3D11="$(wine_path "$DXMT_D3D11_DLL")"
export APITRACE_TRACE_BUNDLE="$(wine_path "$TRACE_DIR")"
if [ "$VISUAL_CHECK" != "0" ]; then
    export APITRACE_TRIANGLE_MAX_FRAMES=300
else
    export APITRACE_TRIANGLE_MAX_FRAMES=120
fi
export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d11,dxgi,winemetal=n,b"
export WINEDEBUG="-all"
export WINEARCH="win64"
export WINEPREFIX="$WINE_PREFIX"
if [ -n "$DXMT_WINE_ROOT" ]; then
    export WINEDLLPATH="$DXMT_WINE_ROOT:$ROOT_DIR/../wine-enviroment/lib/wine"
else
    export WINEDLLPATH="$ROOT_DIR/../wine-enviroment/lib/wine"
fi
export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$ROOT_DIR/../wine-enviroment/lib:$ROOT_DIR/../wine-enviroment/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"

if [ ! -f "$WINE_PREFIX/system.reg" ]; then
    "$WINE_BIN" wineboot -u >/dev/null 2>&1 || true
fi

if [ "$VISUAL_CHECK" != "0" ]; then
    "$WINE_BIN" explorer "/desktop=$VISUAL_DESKTOP" "$DEMO_BIN_DIR/apitrace_triangle_d3d11.exe" >"$RUN_LOG" 2>&1 &
    wine_pid="$!"
    (
        capture_visual_window "$VISUAL_WINDOW_TITLE" "$VISUAL_SCREENSHOT"
    ) &
    capture_pid="$!"
else
    "$WINE_BIN" "$DEMO_BIN_DIR/apitrace_triangle_d3d11.exe" >"$RUN_LOG" 2>&1 &
    wine_pid="$!"
    capture_pid=""
fi

deadline="$(( $(date +%s) + 90 ))"
while kill -0 "$wine_pid" 2>/dev/null; do
    if [ "$(date +%s)" -ge "$deadline" ]; then
        kill "$wine_pid" >/dev/null 2>&1 || true
        wait "$wine_pid" >/dev/null 2>&1 || true
        echo "wine demo timed out" >&2
        exit 1
    fi
    sleep 1
done

wait "$wine_pid"

if [ -n "$capture_pid" ]; then
    wait "$capture_pid"
    if [ ! -f "$VISUAL_SCREENSHOT" ]; then
        echo "missing visual screenshot: $VISUAL_SCREENSHOT" >&2
        exit 1
    fi
    validate_visual_window "$VISUAL_SCREENSHOT"
fi

python3 - "$TRACE_DIR" <<'PY'
import json
import pathlib
import sys

trace_dir = pathlib.Path(sys.argv[1])
callstream = trace_dir / "callstream.jsonl"
checksums = trace_dir / "checksums.json"
objects = trace_dir / "objects" / "objects.json"

for path in (callstream, checksums, objects):
    if not path.is_file():
        raise SystemExit(f"missing required trace file: {path}")

with checksums.open("r", encoding="utf-8") as stream:
    checksum_data = json.load(stream)

if "files" not in checksum_data or "callstream.jsonl" not in checksum_data["files"]:
    raise SystemExit("checksums.json does not cover callstream.jsonl")

with objects.open("r", encoding="utf-8") as stream:
    object_data = json.load(stream)

if not object_data.get("objects"):
    raise SystemExit("objects.json is empty")

records = []
with callstream.open("r", encoding="utf-8") as stream:
    for line in stream:
        line = line.strip()
        if line:
            records.append(json.loads(line))

if len(records) < 2:
    raise SystemExit("callstream.jsonl is unexpectedly short")

sequences = [record["sequence"] for record in records if "sequence" in record]
if sequences != sorted(sequences):
    raise SystemExit("callstream sequence is not monotonic")

required_functions = [
    "D3D11CreateDeviceAndSwapChain",
    "ID3D11Device::CreateBuffer",
    "ID3D11DeviceContext::Draw",
    "IDXGISwapChain::Present",
]
seen_functions = {
    record.get("function")
    for record in records
    if record.get("record_kind") == "call"
}
missing = [name for name in required_functions if name not in seen_functions]
if missing:
    raise SystemExit(f"missing required call records: {missing}")

def first_call(function_name: str):
    for record in records:
        if record.get("record_kind") == "call" and record.get("function") == function_name:
            return record
    raise SystemExit(f"missing call record for schema validation: {function_name}")

device_create = first_call("D3D11CreateDeviceAndSwapChain")
swap_chain = device_create.get("payload", {}).get("swap_chain")
required_swap_chain_keys = {
    "width",
    "height",
    "format",
    "sample_count",
    "sample_quality",
    "buffer_usage",
    "buffer_count",
    "swap_effect",
    "windowed",
    "flags",
}
if not isinstance(swap_chain, dict) or not required_swap_chain_keys.issubset(swap_chain):
    raise SystemExit("D3D11CreateDeviceAndSwapChain payload is missing replay-required swap-chain fields")

input_layout = first_call("ID3D11Device::CreateInputLayout")
input_layout_payload = input_layout.get("payload", {})
elements = input_layout_payload.get("elements")
if not isinstance(input_layout_payload.get("shader_path"), str):
    raise SystemExit("CreateInputLayout payload is missing shader_path")
if not isinstance(elements, list) or not elements:
    raise SystemExit("CreateInputLayout payload is missing input element descriptors")
required_input_element_keys = {
    "semantic_name",
    "semantic_index",
    "format",
    "input_slot",
    "aligned_byte_offset",
    "input_slot_class",
    "instance_data_step_rate",
}
if not all(isinstance(element, dict) and required_input_element_keys.issubset(element) for element in elements):
    raise SystemExit("CreateInputLayout payload has incomplete input element descriptors")

viewport_call = first_call("ID3D11DeviceContext::RSSetViewports")
viewports = viewport_call.get("payload", {}).get("viewports")
required_viewport_keys = {"top_left_x", "top_left_y", "width", "height", "min_depth", "max_depth"}
if not isinstance(viewports, list) or not viewports:
    raise SystemExit("RSSetViewports payload is missing full viewport descriptors")
if not all(isinstance(viewport, dict) and required_viewport_keys.issubset(viewport) for viewport in viewports):
    raise SystemExit("RSSetViewports payload has incomplete viewport descriptors")

vertex_buffers = first_call("ID3D11DeviceContext::IASetVertexBuffers")
bindings = vertex_buffers.get("payload", {}).get("bindings")
required_binding_keys = {"object_id", "stride", "offset"}
if not isinstance(bindings, list) or not bindings:
    raise SystemExit("IASetVertexBuffers payload is missing binding descriptors")
if not all(isinstance(binding, dict) and required_binding_keys.issubset(binding) for binding in bindings):
    raise SystemExit("IASetVertexBuffers payload has incomplete binding descriptors")

referenced_paths = []
for record in records:
    payload = record.get("payload")
    if not isinstance(payload, dict):
        continue
    for key, value in payload.items():
        if key.endswith("_path") and isinstance(value, str):
            referenced_paths.append(value)

if not referenced_paths:
    raise SystemExit("no asset paths were referenced from callstream payloads")

for relative_path in referenced_paths:
    path = trace_dir / relative_path
    if not path.is_file():
        raise SystemExit(f"missing referenced asset: {relative_path}")
    if path.stat().st_size == 0:
        raise SystemExit(f"empty referenced asset: {relative_path}")

print("validated trace bundle:", trace_dir)
print("events:", len(records))
print("assets:", len(referenced_paths))
PY

echo "run log: $RUN_LOG"
echo "trace bundle: $TRACE_DIR"
if [ "$VISUAL_CHECK" != "0" ]; then
    echo "visual screenshot: $VISUAL_SCREENSHOT"
fi
