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

`test/` 下的 Windows demo 和验证脚本使用 MinGW 交叉工具链：

```sh
cmake -S test -B test/build/windows-x86_64 -DCMAKE_TOOLCHAIN_FILE=test/toolchains/windows-x86_64-mingw.cmake
cmake --build test/build/windows-x86_64
cmake --install test/build/windows-x86_64 --prefix test/artifacts/windows-x86_64/demo
```

该 toolchain 使用 `x86_64-w64-mingw32-gcc` / `x86_64-w64-mingw32-g++`，不要改成 `clang` / `clang++`。

## 批量构建

如果需要同时构建 host 库、Windows 产物和测试 demo，可使用：

```sh
./scripts/build-matrix.sh
```
