#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DXMT_REPO_ROOT="${APITRACE_DXMT_REPO_ROOT:-$(CDPATH= cd -- "$ROOT_DIR/../.." && pwd)}"
GAMESIR_ROOT="${APITRACE_GAMESIR_ROOT:-$(CDPATH= cd -- "$DXMT_REPO_ROOT/.." && pwd)}"
WINE_ENV_ROOT="${APITRACE_WINE_ENV_ROOT:-$GAMESIR_ROOT/wine-enviroment}"
WINE_BIN="${APITRACE_WINE_BIN:-$WINE_ENV_ROOT/bin/wine}"
if [ ! -x "$WINE_BIN" ]; then
    WINE_BIN="$(command -v wine || true)"
fi

HOST_BUILD_DIR="${APITRACE_HOST_BUILD_DIR:-$ROOT_DIR/build/cmake-arm64}"
WINDOWS_BUILD_DIR="${APITRACE_WINDOWS_BUILD_DIR:-$ROOT_DIR/build/windows-cross}"
TEST_PREFIX="${APITRACE_D3D12_TEST_PREFIX:-$ROOT_DIR/test/artifacts/d3d12}"
LINK_DIR="${APITRACE_LINK_ARTIFACT_DIR:-$ROOT_DIR/test/artifacts/link}"
LINK_TRACE="${APITRACE_LINK_TRACE_BUNDLE:-$ROOT_DIR/test/artifacts/link-probe-d3d12.apitrace}"
METAL_RETRACE_BUNDLE="$LINK_DIR/d3d12-metal-retrace.apitrace"
RUN_LOG="$LINK_DIR/d3d12-trace.log"
METAL_RETRACE_LOG="$LINK_DIR/d3d12-metal-retrace.log"
METAL_COMPARE_LOG="$LINK_DIR/d3d12-metal-compare.log"

DXMT_BUILD_DIR="${APITRACE_DXMT_BUILD_DIR:-$DXMT_REPO_ROOT/build-builtin}"
DXMT_STAGE_DIR="${APITRACE_DXMT_STAGE_DIR:-$ROOT_DIR/test/artifacts/dxmt-runtime-d3d12}"
DXMT_RUNTIME_ROOT=""
DXMT_UNIX_DIR=""

DEMO_BIN_DIR="$TEST_PREFIX/bin"
DEMO_EXE="$DEMO_BIN_DIR/apitrace_test_d3d12.exe"
ROOT_D3D12_PROXY_DLL="$WINDOWS_BUILD_DIR/d3d12.dll"
ROOT_DXGI_PROXY_DLL="$WINDOWS_BUILD_DIR/dxgi.dll"
NATIVE_RETRACE="$HOST_BUILD_DIR/retrace"
HOST_BUNDLE_CHECK="$HOST_BUILD_DIR/bundle-check"

fail() {
    echo "error: $*" >&2
    exit 1
}

require_file() {
    [ -f "$1" ] || fail "missing file: $1"
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
    [ -f "$DXMT_BUILD_DIR/build.ninja" ] || fail "missing DXMT build directory: $DXMT_BUILD_DIR"
    meson compile -C "$DXMT_BUILD_DIR"
    DESTDIR="$DXMT_STAGE_DIR" meson install -C "$DXMT_BUILD_DIR" --tags runtime,nvext
    DXMT_RUNTIME_ROOT="$(resolve_dxmt_stage_prefix)"
    DXMT_UNIX_DIR="$DXMT_RUNTIME_ROOT/x86_64-unix"
}

prepare_demo_runtime() {
    require_file "$ROOT_D3D12_PROXY_DLL"
    require_file "$ROOT_DXGI_PROXY_DLL"
    require_file "$DEMO_EXE"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12.dll"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12core.dll"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/dxgi.dll"
    require_file "$DXMT_RUNTIME_ROOT/x86_64-windows/winemetal.dll"

    cp "$ROOT_D3D12_PROXY_DLL" "$DEMO_BIN_DIR/d3d12.dll"
    cp "$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12core.dll" "$DEMO_BIN_DIR/d3d12core.dll"
    cp "$ROOT_DXGI_PROXY_DLL" "$DEMO_BIN_DIR/dxgi.dll"
    cp "$DXMT_RUNTIME_ROOT/x86_64-windows/winemetal.dll" "$DEMO_BIN_DIR/winemetal.dll"
}

prepare_wine_env() {
    export WINEDLLOVERRIDES="mscoree,mshtml=d;d3d12,d3d12core,dxgi,winemetal=n,b"
    export WINEDEBUG="-all"
    export WINEARCH="win64"
    export WINEPREFIX="${APITRACE_WINEPREFIX:-$LINK_DIR/wineprefix-d3d12-link}"
    export APITRACE_D3D12_BACKEND="dxmt"
    export APITRACE_DOWNSTREAM_D3D12="$DXMT_RUNTIME_ROOT/x86_64-windows/d3d12.dll"
    export APITRACE_DOWNSTREAM_DXGI="$DXMT_RUNTIME_ROOT/x86_64-windows/dxgi.dll"
    export DXMT_EXPERIMENT_DX12_SUPPORT=1
    export WINEDLLPATH="$DXMT_RUNTIME_ROOT:$WINE_ENV_ROOT/lib/wine"
    export DYLD_FALLBACK_LIBRARY_PATH="$DXMT_UNIX_DIR:$WINE_ENV_ROOT/lib:$WINE_ENV_ROOT/lib/wine/x86_64-unix:/opt/homebrew/lib:/usr/local/lib"
    export APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1
}

trace_d3d12_to_metal() {
    rm -rf "$LINK_TRACE"
    prepare_wine_env
    export APITRACE_TRACE_BUNDLE="$LINK_TRACE"
    export APITRACE_METAL_BUNDLE="$LINK_TRACE"
    "$WINE_BIN" "$DEMO_EXE" | tr -d '\r' | tee "$RUN_LOG"
    unset APITRACE_TRACE_BUNDLE
    unset APITRACE_METAL_BUNDLE

    require_file "$LINK_TRACE/callstream.jsonl"
    require_file "$LINK_TRACE/metal-callstream.jsonl"
    require_file "$LINK_TRACE/analysis/translation-links.jsonl"
    if [ -x "$HOST_BUNDLE_CHECK" ]; then
        "$HOST_BUNDLE_CHECK" "$LINK_TRACE" >/dev/null
    fi
    "$NATIVE_RETRACE" --metal --validate-only "$LINK_TRACE" >/dev/null
    if grep -F "scene start: dxr_smoke" "$RUN_LOG" >/dev/null; then
        fail "dxr_smoke should remain skipped in d3d12 all"
    fi
}

retrace_metal() {
    require_file "$NATIVE_RETRACE"
    rm -rf "$METAL_RETRACE_BUNDLE"
    APITRACE_TRACE_BUNDLE="$METAL_RETRACE_BUNDLE" \
    APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
        "$NATIVE_RETRACE" --metal "$LINK_TRACE" | tee "$METAL_RETRACE_LOG"
    require_file "$METAL_RETRACE_BUNDLE/callstream.jsonl"
}

compare_metal() {
    python3 "$ROOT_DIR/scripts/lib/present_frame_compare.py" \
        --baseline-api d3d12 \
        --candidate-api metal \
        --baseline "$LINK_TRACE" \
        --candidate "$METAL_RETRACE_BUNDLE" \
        --tile 100 \
        --tile-pixel-threshold 0.95 | tee "$METAL_COMPARE_LOG"
    grep -F "failed_frames=0" "$METAL_COMPARE_LOG" >/dev/null || fail "d3d12-to-metal compare failed"
}

check_metal_link_coverage() {
    python3 - "$LINK_TRACE/analysis/translation-links.jsonl" <<'PY'
import json
import pathlib
import sys

required_scope_kinds = {"encoder", "draw_to_metal_calls"}


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


path = pathlib.Path(sys.argv[1])
if not path.is_file():
    fail(f"missing translation-links file: {path}")

coverage = {}
with path.open("r", encoding="utf-8") as stream:
    for line_number, line in enumerate(stream, 1):
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as exc:
            fail(f"{path}:{line_number}: invalid JSON: {exc}")
        if record.get("record_type") != "scope":
            continue
        d3d_sequence = int(record.get("d3d_sequence", 0))
        if d3d_sequence == 0:
            continue
        scope_kind = str(record.get("scope_kind", ""))
        coverage.setdefault(d3d_sequence, set()).add(scope_kind)

if not coverage:
    fail(f"{path}: no non-zero d3d_sequence scope records found")

missing = []
for d3d_sequence in sorted(coverage):
    required_missing = sorted(required_scope_kinds - coverage[d3d_sequence])
    if required_missing:
        missing.append(f"{d3d_sequence}:{','.join(required_missing)}")

if missing:
    print("missing_link_coverage=" + ";".join(missing))
    raise SystemExit(1)

print(f"d3d_sequences={len(coverage)}")
print("coverage=PASS")
PY
}

[ -n "$WINE_BIN" ] && [ -x "$WINE_BIN" ] || fail "missing wine binary"
require_file "$NATIVE_RETRACE"
mkdir -p "$LINK_DIR"

stage_dxmt_runtime
prepare_demo_runtime
trace_d3d12_to_metal
check_metal_link_coverage
retrace_metal
compare_metal

echo "test-d3d-metal-link PASS"
