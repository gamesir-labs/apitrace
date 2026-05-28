# Standalone D3D / DXGI / Windows headers

These headers are imported from [DXMT](https://github.com/3Shain/dxmt)'s
`include/native/` tree, which itself imports the DirectX-related headers from
[MinGW-w64](https://www.mingw-w64.org/). They are used by the macOS native
D3D retrace backend so that apitrace can consume the DXMT-provided
`d3d11.dylib` / `d3d12.dylib` / `dxgi.dylib` using exactly the same ABI that
those libraries were built against — without pulling in the full MinGW-w64
toolchain or Wine's headers.

Layout:

- `directx/` — D3D, DXGI and related interface headers from MinGW-w64.
- `windows/` — a minimal stub of `windows.h` and the COM bits required to
  satisfy the includes above. Most files are placeholders; the real content
  lives in `windows_base.h` and `unknwn.h`.

The directory is added as an `INTERFACE` CMake target (`apitrace_d3d_headers`)
when `APITRACE_BUILD_D3D_NATIVE_RETRACE` is `ON`.

See `directx/COPYING.MinGW-w64.txt` for the upstream license.
