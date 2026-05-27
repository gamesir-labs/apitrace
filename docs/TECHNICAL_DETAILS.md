# 技术细节

本文档汇总当前仓库对外可用的运行链路、trace bundle 结构、retrace 语义和验证入口。
更细的存储字段定义见 [TRACE_LAYOUT.md](TRACE_LAYOUT.md)，replay 约束见
[RETRACE.md](RETRACE.md)。

## 产物与职责

当前产物分为三组：

- `retrace` / `retrace.exe`：读取 trace bundle 并执行 replay。
- `d3d11.dll` / `d3d12.dll`：Windows 或 Wine app-local override 用的 capture proxy。
- `libapitrace_platform_apple_metal.a` 与 `metal_capi.h`：给转译层写入 Metal
  callstream 的 native facade。

`retrace` 是窄 CLI：它接收一个 bundle 路径，执行 replay，并输出统计信息和错误诊断。
capture、convert、inspect 等能力不混入这个入口。

## Trace Bundle

trace 的物理格式是目录 bundle，而不是单个归档文件。根目录至少包含：

```text
scene.apitrace/
  checksums.json
  callstream.jsonl
```

常见资产目录包括：

```text
scene.apitrace/
  shaders/
  textures/
  buffers/
  pipelines/
  objects/
  analysis/
```

`callstream.jsonl` 是 retrace 的主入口，保存原始 D3D 调用顺序、对象引用、资源引用、
返回值、错误码和 frame / submit / present / barrier / fence 等边界语义。shader、texture、
buffer、root signature 和 pipeline 等大资产按类型拆成独立文件，由 callstream 通过路径或稳定
id 引用。

`checksums.json` 负责完整性校验，不负责恢复调用语义。

## Capture 路径

当前可直接使用的 Windows capture 入口是 app-local proxy DLL：

- D3D11：把 `d3d11.dll` 放到目标程序同目录，并通过 Wine / Windows DLL 搜索顺序优先加载。
- D3D12：把 `d3d12.dll` 放到目标程序同目录，并对 D3D12 路径设置 override。

测试脚本通过 `APITRACE_TRACE_BUNDLE` 指定输出 bundle 目录。D3D12 调试画面读回需要显式开启：

```sh
APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1
```

PresentFrame 资产只用于调试和像素级对比，不属于默认 replay 输入语义。

## Retrace 路径

基本用法：

```sh
retrace path/to/scene.apitrace
```

Wine-hosted downstream runtime 用法：

```sh
WINEDLLOVERRIDES="d3d11,dxgi,winemetal=n,b;mscoree,mshtml=d" \
wine retrace.exe path/to/scene.apitrace
```

Metal 辅助 replay：

```sh
retrace --metal path/to/scene.apitrace
```

可选参数：

- `--metal`：消费 bundle 内的 `metal-callstream.jsonl`。
- `--metal-backend <name>`：选择 Metal replay backend，默认是 `native`。

retrace 的默认语义是重新发出记录到的 API 调用和 command stream。D3D12 路径不能把
`D3D12PresentFrame` 贴回 swapchain 来冒充重渲染；遇到未覆盖的原生语义时必须明确失败。

## 关键环境变量

- `APITRACE_TRACE_BUNDLE`：capture 或 retrace debug 输出 bundle 目录。
- `APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1`：D3D12 capture 时保存 Present 前 RGBA debug 帧。
- `APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1`：D3D12 retrace 真实 replay 后保存 Present 前
  debug 帧。
- `APITRACE_D3D12_RETRACE_PRESENT_DELAY_MS`：仅用于人工观察 D3D12 replay 窗口，不参与 trace
  语义或像素验收。
- `APITRACE_METAL_BUNDLE`：转译层写 Metal callstream 的目标 bundle。
- `APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1`：Metal retrace 保存 PresentFrame debug 资产。
- `APITRACE_RETRACE_SHOW_WINDOW=1`：D3D11 retrace 显示 replay 窗口。
- `APITRACE_RETRACE_WINDOW_TITLE`：D3D11 retrace 窗口标题。

## 验证方式

模块级脚本是当前推荐验证入口：

```sh
scripts/test-d3d11.sh
scripts/test-d3d12.sh
scripts/test-metal.sh
scripts/test-d3d-metal-link.sh
```

这些脚本会构建需要的 demo / retrace 产物，生成 trace bundle，执行 retrace，并用
`scripts/lib/present_frame_compare.py` 对 debug PresentFrame 资产做 tile 级对比。

手动执行 PresentFrame 对比：

```sh
python3 scripts/lib/present_frame_compare.py \
  --api <d3d11|d3d12|metal> \
  --baseline <trace-bundle> \
  --candidate <retrace-bundle> \
  --tile 100 \
  --tile-pixel-threshold 0.95
```

比较规则固定为 `100` 像素非重叠 tile；每个 tile 至少 `95%` 像素完全匹配。这个流程只验证
debug 输出结果，不改变 retrace 语义。

## 构建开关

根项目 CMake 开关：

- `APITRACE_BUILD_TOOLS`：构建 `retrace`。
- `APITRACE_BUILD_WINDOWS_DLLS`：在 Windows / MinGW 构建中生成 proxy DLL。
- `APITRACE_BUILD_METAL_BACKEND`：在 Apple 平台构建 Metal backend。

测试项目 CMake 开关：

- `APITRACE_BUILD_TEST_DEMO`：构建 Windows D3D 测试 demo。
- `APITRACE_BUILD_METAL_DEMO`：构建 macOS Metal 测试 demo。
- `APITRACE_ROOT_BUILD_DIR`：Metal demo 链接根项目静态库时使用的根构建目录。

## 当前覆盖边界

D3D11 replay 当前覆盖测试 demo 所需的最小 D3D11 链路，包括 device / context / swapchain、
buffer、shader、input layout、RTV、viewport、draw 和 present 等基础语义。

D3D12 replay 当前覆盖默认 D3D12 验证矩阵需要的 resource、descriptor、root signature、PSO、
command list、submission、draw、dispatch、copy、resolve、indirect 和同步语义。DXR、mesh
shader 和更多 descriptor 维度按同一规则继续扩展。

Metal replay 是转译层辅助调试路径，消费的是转译后的 `metal-callstream.jsonl`，不是主 D3D
callstream。
