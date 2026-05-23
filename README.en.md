# apitrace

[中文](README.zh-CN.md)

`apitrace` currently provides two kinds of Windows-side deliverables:

- `retrace.exe`: the replay entrypoint for trace bundles.
- `d3d11.dll` / `d3d12.dll`: capture proxy DLLs intended for Wine or Windows override-based injection.

For development documentation, see [docs/README.md](docs/README.md).

## CI Build

The repository ships with a GitHub Actions workflow:

- `.github/workflows/build-ci.yml`

It currently runs only on `push`, not on `pull_request`.

The CI artifact layout is fixed as:

```text
retrace/
  retrace.exe
override/
  d3d11.dll
  d3d12.dll
```

By design, the package only contains the minimal binaries produced by this repository. It does not include headers, static libraries, or MinGW runtime DLLs.

## Usage

### 1. capture: Wine override mode

Place the DLLs from `override/` next to the target executable, then configure DLL overrides as needed.

A typical DX11 example is:

```sh
WINEDLLOVERRIDES="d3d11=n,b;mscoree,mshtml=d" wine game.exe
```

If you also want to cover the DX12 path, place `d3d12.dll` in the same directory and add the corresponding override.

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

For the validation flow of `retrace.exe`, see [scripts/validate-d3d11-retrace-wine.sh](scripts/validate-d3d11-retrace-wine.sh).
