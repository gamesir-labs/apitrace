# apitrace

[English](README.en.md)

`apitrace` 是面向图形转译层研发的 D3D trace / retrace 工具。它的目标是把
Windows 程序的 D3D11 / D3D12 调用记录成可检查、可复制、可重放的 trace
bundle，并在 Windows、Wine 或转译层环境里用 `retrace` 重新执行同一份调用语义。

这个仓库当前同时覆盖两条调试链路：

- 主链路：捕获 D3D11 / D3D12 调用、资源、对象关系和同步边界，再通过
  `retrace` 按原始 D3D 语义重放。
- 辅助链路：由 DXMT 这类转译层显式写入转译后的 Metal callstream，并通过
  `retrace --metal` 做 Metal 侧调试回放。

## 适用场景

- 复现 D3D 转译层里的渲染错误。
- 对比 capture 时的 D3D 调用语义和 retrace 时的后端行为。
- 检查 shader、buffer、texture、descriptor、pipeline 和同步状态是否被完整保存。
- 用同一组测试场景验证 D3D11、D3D12 与 Metal 辅助 replay 路径。

## 当前产物

Windows 侧产物分成两类：

- `retrace.exe`：trace bundle 的回放入口。
- `d3d11.dll` / `d3d12.dll`：给 Wine/Windows override 模式使用的 capture proxy DLL。

macOS Metal 侧产物包括：

- `retrace`：native replay CLI，也用于 `--metal` 辅助回放。
- `libapitrace_platform_apple_metal.a`：给转译层接入 Metal trace facade 的静态库。
- `metal_capi.h`：转译层可直接调用的 C ABI 头文件。

CI 的 Windows artifact 布局固定为：

```text
retrace/
  retrace.exe
override/
  d3d11.dll
  d3d12.dll
```

这个 artifact 只包含本仓库生成的最小二进制，不包含 DXMT、Wine、MinGW runtime
或其他下游运行时。

## 快速使用

### 1. capture: Wine override 模式

把 `override/` 下的 proxy DLL 放到目标程序同目录，然后设置 DLL override。

DX11 典型示例：

```sh
WINEDLLOVERRIDES="d3d11=n,b;mscoree,mshtml=d" wine game.exe
```

如果要覆盖 DX12 路径，则把 `d3d12.dll` 一并放到同目录，并追加对应 override。

默认 trace bundle 输出位置由 capture runtime 决定；测试脚本使用
`APITRACE_TRACE_BUNDLE` 指向具体 bundle 目录。

### 2. retrace: Wine + downstream runtime

`retrace.exe` 本身只是 replay CLI。它运行时需要真正的 downstream D3D runtime，例如 DXMT 提供的：

- `d3d11.dll`
- `dxgi.dll`
- `winemetal.dll`

因此回放时不要把 CI artifact 里的 capture proxy `d3d11.dll` 放在 `retrace.exe` 旁边替代 downstream runtime。

更直接的做法是：

1. 把 `retrace/retrace.exe` 放到一个单独目录。
2. 再把 DXMT 或其他目标 translation-layer 提供的 runtime DLL 放到同目录。
3. 用 Wine 启动 `retrace.exe <trace-bundle>`。

DXMT 路径下的示例：

```sh
WINEDLLOVERRIDES="d3d11,dxgi,winemetal=n,b;mscoree,mshtml=d" \
wine retrace.exe path/to/scene.apitrace
```

注意：retrace 的语义是重新执行 D3D 调用，不是把 capture 时读回的画面帧贴回窗口。
`D3D12PresentFrame`、`D3D11PresentFrame` 和 `MetalPresentFrame` 只用于显式调试
capture、测试对比和诊断。

### 3. Metal 辅助回放

如果 trace bundle 中包含转译层写入的 `metal-callstream.jsonl`，可以使用：

```sh
retrace --metal path/to/scene.apitrace
```

可用 `--metal-backend <name>` 指定 Metal replay backend；当前默认 backend 为 `native`。

## 本地构建

Host build：

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake
```

Windows 交叉编译：

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

更完整的构建说明见 [docs/BUILD.md](docs/BUILD.md)。

## 验证入口

模块级验证脚本：

- `scripts/test-d3d11.sh`
- `scripts/test-d3d12.sh`
- `scripts/test-metal.sh`
- `scripts/test-d3d-metal-link.sh`

测试场景矩阵和 PresentFrame 对比规则见 [test/README.md](test/README.md)。

## 文档入口

- [docs/README.md](docs/README.md)：技术文档索引。
- [docs/TECHNICAL_DETAILS.md](docs/TECHNICAL_DETAILS.md)：当前功能、格式和运行链路的技术细节。
- [docs/TRACE_LAYOUT.md](docs/TRACE_LAYOUT.md)：trace bundle 磁盘布局。
- [docs/RETRACE.md](docs/RETRACE.md)：retrace 语义和约束。
