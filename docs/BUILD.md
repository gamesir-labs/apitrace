# 构建流程

本仓库统一使用 CMake。

## Host build

在当前 macOS 主机上构建核心库和本地工具：

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake
```

也可以直接用脚本：

```sh
./scripts/build-cmake.sh
```

## Windows 交叉编译

根项目 Windows 产物使用 MinGW 交叉工具链：

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

`test/` 下的 Windows demo 也使用同一套 MinGW toolchain：

```sh
cmake -S test -B test/build/windows-x86_64 -DCMAKE_TOOLCHAIN_FILE=test/toolchains/windows-x86_64-mingw.cmake
cmake --build test/build/windows-x86_64
cmake --install test/build/windows-x86_64 --prefix test/artifacts/windows-x86_64/demo
```

该 toolchain 使用 `x86_64-w64-mingw32-gcc` / `x86_64-w64-mingw32-g++`，不要改成 `clang` / `clang++`。

## macOS Metal build

在 Apple 平台构建 native retrace 和 Metal backend：

```sh
cmake -S . -B build/macos-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPITRACE_BUILD_METAL_BACKEND=ON \
  -DAPITRACE_BUILD_TOOLS=ON
cmake --build build/macos-native --target \
  apitrace_retrace \
  apitrace_platform_apple_metal
```

## 批量构建

如果需要同时构建 host 库、Windows 产物和测试 demo，可使用：

```sh
./scripts/build-matrix.sh
```

## CI artifact

Windows CI artifact 布局：

```text
retrace/
  retrace.exe
override/
  d3d11.dll
  d3d12.dll
```

macOS native Metal artifact 布局：

```text
metal/
  retrace
  libapitrace_platform_apple_metal.a
  metal_capi.h
```

CI 包只包含本仓库产物，不包含 downstream D3D runtime、Wine 或 MinGW runtime DLL。
