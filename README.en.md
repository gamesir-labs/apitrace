# apitrace

[中文](README.zh-CN.md)

`apitrace` is a D3D trace / retrace tool for graphics translation-layer
development. It records D3D11 / D3D12 calls from Windows applications into a
readable trace bundle and replays the same API semantics through `retrace`.

The repository currently covers two debugging paths:

- The primary D3D path captures D3D calls, resources, object relationships, and
  synchronization boundaries, then replays the original D3D semantics.
- The auxiliary Metal path lets a translation layer write translated Metal
  callstreams and replay them with `retrace --metal`.

## Use Cases

- Reproduce rendering bugs in D3D translation layers.
- Compare captured D3D semantics with replay backend behavior.
- Inspect whether shader, buffer, texture, descriptor, pipeline, and sync state
  are preserved in the bundle.
- Validate D3D11, D3D12, and Metal debug replay paths with the shared test scene
  matrix.

## Deliverables

The Windows-side deliverables are:

- `retrace.exe`: the replay entrypoint for trace bundles.
- `d3d11.dll` / `d3d12.dll`: capture proxy DLLs intended for Wine or Windows override-based injection.

The macOS Metal deliverables are:

- `retrace`: native replay CLI, including the `--metal` auxiliary path.
- `libapitrace_platform_apple_metal.a`: static library for translation-layer
  Metal trace integration.
- `metal_capi.h`: C ABI header for the Metal trace facade.

## CI Artifacts

The repository ships with a GitHub Actions workflow:

- `.github/workflows/build-ci.yml`

The Windows artifact layout is fixed as:

```text
retrace/
  retrace.exe
override/
  d3d11.dll
  d3d12.dll
```

By design, the package only contains the minimal binaries produced by this repository. It does not include headers, static libraries, or MinGW runtime DLLs.

## Quick Start

### 1. capture: Wine override mode

Place the DLLs from `override/` next to the target executable, then configure DLL overrides as needed.

A typical DX11 example is:

```sh
WINEDLLOVERRIDES="d3d11=n,b;mscoree,mshtml=d" wine game.exe
```

If you also want to cover the DX12 path, place `d3d12.dll` in the same directory and add the corresponding override.

The capture output bundle is selected by the capture runtime. The repository
test scripts set `APITRACE_TRACE_BUNDLE` to the target bundle directory.

### 2. retrace: Wine + downstream runtime

`retrace.exe` itself is only the replay CLI. At runtime it still needs a real downstream D3D runtime, for example the one provided by DXMT:

- `d3d11.dll`
- `dxgi.dll`
- `winemetal.dll`

Because of that, do not place the capture proxy `d3d11.dll` from the CI artifact next to `retrace.exe` as a substitute for the downstream runtime.

A straightforward setup is:

1. Put `retrace/retrace.exe` into its own directory.
2. Then place the runtime DLLs from DXMT or another target translation layer into the same directory.
3. Launch `retrace.exe <trace-bundle>` with Wine.

An example for the DXMT path is:

```sh
WINEDLLOVERRIDES="d3d11,dxgi,winemetal=n,b;mscoree,mshtml=d" \
wine retrace.exe path/to/scene.apitrace
```

Retrace means re-executing the recorded D3D semantics. It is not recorded-frame
playback. `D3D11PresentFrame`, `D3D12PresentFrame`, and `MetalPresentFrame`
assets are debug and validation data only.

### 3. Metal auxiliary replay

If a bundle contains `metal-callstream.jsonl`, replay the translated Metal stream
with:

```sh
retrace --metal path/to/scene.apitrace
```

Use `--metal-backend <name>` to select a Metal replay backend. The default
backend is `native`.

## Build

Host build:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake
```

Windows cross build:

```sh
cmake -S . -B build/windows-cross -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=test/toolchains/windows-x86_64-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPITRACE_BUILD_TOOLS=ON \
  -DAPITRACE_BUILD_WINDOWS_DLLS=ON \
  -DAPITRACE_BUILD_METAL_BACKEND=OFF
cmake --build build/windows-cross --target \
  apitrace_retrace \
  apitrace_platform_windows_d3d11 \
  apitrace_platform_windows_d3d12
```

See [docs/BUILD.md](docs/BUILD.md) for more build details.

## Validation

Module-level validation scripts:

- `scripts/test-d3d11.sh`
- `scripts/test-d3d12.sh`
- `scripts/test-metal.sh`
- `scripts/test-d3d-metal-link.sh`

The scene matrix and PresentFrame comparison rules are documented in
[test/README.md](test/README.md).

## Documentation

- [docs/README.md](docs/README.md): technical documentation index.
- [docs/TECHNICAL_DETAILS.md](docs/TECHNICAL_DETAILS.md): current feature and runtime details.
- [docs/TRACE_LAYOUT.md](docs/TRACE_LAYOUT.md): trace bundle disk layout.
- [docs/RETRACE.md](docs/RETRACE.md): retrace semantics and constraints.
