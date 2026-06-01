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
  callstream.jsonl
  assets.json
  checksums.json
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

`assets.json` 是资源索引入口，负责把 `blob_refs` 映射到独立资源文件。D3D callstream 和
Metal callstream 都引用同一套资源索引；API 无关的 buffer / texture 可以共享到同一个文件，
pipeline、shader、root signature、Metal library 等 API/后端相关资产则保留各自语义。
索引会保留每个 blob 的最终路径、资源类型、是否 Metal-specific、内容 hash、字节数、
binary 标志和 debug name。`content_hash` 用于稳定资源身份和跨 API 去重审计；
`fast_fingerprint` 只用于捕获期早期去重，不写入最终索引，也不能作为最终完整性依据。

`checksums.json` 负责完整性校验，不负责恢复调用语义。

大资源可以先写入单调临时路径，由后台线程计算内容 hash 后重命名到 hash 路径。bundle 关闭时会
重写 JSON/JSONL 中的临时路径和 `assets.json` 索引，并允许多个 blob id 指向同一内容文件；
这样保留每次 API 调用的完整元数据，同时避免重复落盘相同 buffer / texture / shader 载荷。

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

完整性自检：

```sh
bundle-check path/to/scene.apitrace
bundle-check --require-d3d --require-d3d-replay-closure --require-d3d-native-readiness --require-d3d-present-frames path/to/d3d12.apitrace
bundle-check --require-metal --require-metal-replay-closure --require-metal-present-frames path/to/metal.apitrace
bundle-check --strict-cross-api path/to/d3d-metal.apitrace
```

可选参数：

- `--validate-only`：只构建并校验 D3D replay 闭包，不创建 D3D/Metal 设备；用于无图形会话下的
  smoke 和大 trace 预检，不能替代最终 replay。配合 `--metal` 时，它校验 Metal callstream 的
  library、pipeline、pipeline bind 和 draw/dispatch 闭包，不创建 `MTLDevice`。
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
- `APITRACE_WRITER_STATS=1`：bundle 关闭时向 stderr 输出一行 writer 统计，并写入
  `analysis/writer-stats.jsonl`。统计包含资产注册次数、去重命中、异步写入、队列 fallback、
  路径 alias 重写和 checksum 文件数；用于大 trace 判断瓶颈在 hash、排队、同步落盘还是引用重写。
  `APITRACE_EXACT_DEDUP_COMPARE_THRESHOLD` 可覆盖同 fast fingerprint 资产的主线程精确比较上限；
  默认 1 MiB。低于该上限的重复资源会在 capture 早期直接复用路径，避免第二个异步写入任务、
  临时 asset 路径和 close 阶段引用重写；更大的资源默认避免前台大块 `memcmp`，走后台
  hash-only 路径。
  其中 `async_hash_only_candidates` 表示大资源在已有相同快速 fingerprint 的情况下进入后台
  hash-only 路径，`async_hash_only_write_avoids` 表示最终确认同内容并避免了第二次落盘，
  `async_hash_only_write_bytes_avoided` 表示这条路径实际避免写出的 payload 字节数，
  `async_hash_only_late_writes` 表示 fingerprint 冲突或首次内容尚未落地，后台 hash 后仍需写出新文件。
  `asset_rewrite_candidates_scanned`、`asset_rewrite_candidates_skipped_clean` 和
  `asset_rewrite_replacements` 用于区分引用重写阶段的候选文件扫描、无 alias 文件跳过和实际替换量。
- `APITRACE_REQUIRE_D3D_NATIVE_ABI=1`：运行 native D3D ABI smoke 时，把
  `ABI_SMOKE_SKIP` 视为失败。默认允许没有 Metal-backed DXGI adapter 的进程跳过，便于无图形会话
  的开发环境继续跑纯 bundle 测试；最终验收必须打开这个开关，确认 D3D retrace 真的能进入
  DXMT-backed native adapter。
- `APITRACE_RETRACE_SHOW_WINDOW=1`：D3D11 retrace 显示 replay 窗口。
- `APITRACE_RETRACE_WINDOW_TITLE`：D3D11 retrace 窗口标题。

## 验证方式

模块级脚本是当前推荐验证入口：

```sh
scripts/test-d3d11.sh
scripts/test-d3d12.sh
scripts/test-metal.sh
scripts/test-d3d-metal-link.sh
scripts/test-d3d-native-abi.sh
```

这些脚本会构建需要的 demo / retrace 产物，生成 trace bundle，执行 retrace，并用
`scripts/lib/present_frame_compare.py` 对 debug PresentFrame 资产做 tile 级对比。
对真实游戏这类大 bundle，先运行 `bundle-check` 可以在 retrace 前校验 `checksums.json`
列出的文件闭包、digest 和 manifest 级 `bundle_hash`，并检查 PresentFrame 资产是否能匹配
D3D Present call / boundary 或 Metal `PresentDrawable`，避免把异步资产写入、路径重写或孤立
debug 帧误判成 replay 问题。
对 D3D12 smoke，额外使用 `--require-d3d-replay-closure`、`--require-d3d-native-readiness` 和 `--require-d3d-present-frames`，
要求 D3D 调用流引用独立的 pipeline、shader 和 root signature 资产，并要求 debug PresentFrame
资产匹配 captured Present call / boundary。`--require-d3d-native-readiness` 会执行 D3D12
replay backend 的 validate-only 语义预检，不创建设备，但会拒绝当前不能直接 native replay 的
DXMT-only 语义，避免只有 Metal 侧资产完整而 D3D 原始重放闭包缺失。
对 Metal smoke，额外使用 `--require-metal-replay-closure`、`--require-metal-present-frames` 和
`retrace --metal --validate-only`，要求 Metal 调用流引用独立的 metallib、pipeline 资产，
包含 pipeline bind 与 draw/dispatch，并要求 debug PresentFrame 资产匹配 `PresentDrawable`。
双侧 D3D→Metal smoke 使用 `--require-shared-resources` 时，`bundle-check` 会分别统计
buffer 和 texture 的 D3D 引用、Metal 引用与共享路径；如果两侧都引用某类资源但没有同类共享路径，
检查会失败。双侧 bundle 还必须打开 D3D replay closure 和 native-readiness 检查，避免只凭
translation link / Metal 侧闭包误判 D3D 原始调用元数据已经足够重放。PresentFrame debug 资产
不计入这组资源共享统计，避免画面对比输入掩盖真实游戏资源是否去重。
如果 Metal 闭包里有 draw/dispatch，`bundle-check` 会进一步要求对应 `draw_to_metal_calls`
link 指向 D3D 侧 pipeline-dependent 调用；否则说明 D3D 原始 callstream 缺少足以重放该 workload
的 API 元数据。

`scripts/test-d3d-native-abi.sh` 是 D3D native retrace 接入 DXMT 的前置 smoke。普通执行时，若当前
进程拿不到 Metal-backed DXGI adapter，它会报告 `ABI_SMOKE_SKIP` 并以成功状态结束；用于最终验收或
准备跑大 trace 前应执行：

```sh
APITRACE_REQUIRE_D3D_NATIVE_ABI=1 scripts/test-d3d-native-abi.sh
```

此时 skip 会转为失败，避免把只有 bundle 层闭包正确、但实际 native D3D replay 没有进入 DXMT 的
状态误判为端到端可用。

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

D3D12-to-Metal 的 smoke gate 使用：

```sh
scripts/test-cross-api-smoke.sh
```

该脚本生成合成双侧 bundle，要求 D3D/Metal 调用元数据各自闭包、共享资源走 API 无关路径、
translation links 存在，随后执行双侧 validate-only 并用 `d3d12-to-metal` tile compare 校验
PresentFrame debug 输出。

## 构建开关

根项目 CMake 开关：

- `APITRACE_BUILD_TOOLS`：构建 `retrace`。
- `APITRACE_BUILD_WINDOWS_DLLS`：在 Windows / MinGW 构建中生成 proxy DLL。
- `APITRACE_BUILD_METAL_BACKEND`：在 Apple 平台构建 Metal backend。

测试项目 CMake 开关：

- `APITRACE_BUILD_TEST_DEMO`：构建 Windows D3D 测试 demo。
- `APITRACE_BUILD_METAL_DEMO`：构建 macOS Metal 测试 demo。
- `APITRACE_ROOT_BUILD_DIR`：Metal demo 链接根项目静态库时使用的根构建目录。Metal 原生验证默认使用
  `build/cmake-metal-arm64` 并显式设置 `CMAKE_OSX_ARCHITECTURES=arm64`，避免复用 D3D native retrace 的
  x86_64 DXMT 构建目录。

## 当前覆盖边界

D3D11 replay 当前覆盖测试 demo 所需的最小 D3D11 链路，包括 device / context / swapchain、
buffer、shader、input layout、RTV、viewport、draw 和 present 等基础语义。

D3D12 replay 当前覆盖默认 D3D12 验证矩阵需要的 resource、descriptor、root signature、PSO、
command list、submission、draw、dispatch、copy、resolve、indirect 和同步语义。DXR、mesh
shader 和更多 descriptor 维度按同一规则继续扩展。

Metal replay 是转译层辅助调试路径，消费的是转译后的 `metal-callstream.jsonl`，不是主 D3D
callstream。
