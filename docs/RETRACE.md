# Retrace 设计

## 核心语义

retrace 的核心要求是：

- 输入仍是原始 D3D 调用
- 输出行为要尽量接近捕获时的时序和资源状态
- replay 不改变 API 语义层级

retrace 不是把原始 D3D 调用先改写成统一 IR，再从 IR 倒推回放。

## retrace 过程

1. 读取 trace 容器
2. 建立对象表
3. 恢复资源 blob
4. 按调用顺序重放 D3D 调用
5. 将同一份调用流送给目标后端
6. 做必要的校验和状态对比

## 为什么不把 replay 建成统一 IR 还原

我们的目标不是把 API 变成一个抽象命令集再倒回来，而是保留原始 D3D 语义。

IR 只能在内部帮助：

- 组织资源
- 安排回放
- 做差异分析
- 做后端转换前的中间准备

它不能替代原始调用流。

## 库层实现约束

- retrace 的核心逻辑应当放在 library 或 runtime 层
- 平台可执行文件只负责参数解析、进程启动和结果返回
- 不允许在 `retrace.exe` 和 `retrace` 之间各自维护一套独立 replay 实现

## 可执行入口

用户侧保留两个命令行入口：

- Windows：`retrace.exe`
- macOS：`retrace`

两者都应复用同一套 replay library。

## Wine 与 native 的回放路径

- Wine 回放：直接用 Wine 启动 `retrace.exe`
- native 回放：直接运行 macOS 下的 `retrace`

这两条路径是不同的进程入口，但不应分叉底层 replay 逻辑。

## CLI 契约

命令行工具只做一件事：重放 trace。

因此它应保持最小契约：

- 输入一个 trace 文件
- 执行 replay
- 输出成功、失败和必要的诊断信息

不要把 capture、convert 或其他无关调试能力继续塞进这个 CLI。

## D3D11 replay 重点

- 先恢复 device/context
- 再恢复资源和状态对象
- 最后按事件流提交 `draw` / `dispatch` / `present`
- `IDXGISwapChain::Present` call 与 Present boundary 必须携带同一个连续递增的
  `frame_index`，并校验 `sync_interval` / `flags` 一致；D3D11 replay 使用原始 Present
  参数调用 swapchain，不插入额外帧间隔

## D3D12 replay 重点

- 恢复设备和 queue
- 恢复 PSO、root signature、descriptor 绑定关系
- 复原每一段 command list 的边界
- 尽量保持和捕获时一致的提交节奏

D3D12 replay 的主路径必须重新发出记录到的 D3D12 调用和 command stream，不能把捕获时读回的
RGBA 帧贴回 swapchain 来冒充重渲染。默认 trace 只记录 `IDXGISwapChain::Present` call、
Present boundary、FrameBegin / FrameEnd、`sync_interval`、`flags` 和 `frame_index`。

`D3D12PresentFrame` 仅是显式 debug 资产：

- 只有设置 `APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1` 时，测试 runtime 才会在 Present 前读回
  RGBA 帧并写入 `textures/*.texture`
- 设置 `APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1` 时，D3D12 retrace 可以在真实 command
  replay 后、Present 前读回自己的 back buffer，并把结果写成同类 `D3D12PresentFrame`
- retrace 不允许使用 trace 内的 `D3D12PresentFrame` 做画面播放；这些帧只能用于预览、像素级对比
  和 bug 诊断
- `APITRACE_D3D12_RETRACE_PRESENT_DELAY_MS` 只用于人工观察窗口播放，默认值为 `0`；它不是 trace
  输入语义，也不参与逐帧像素对比验收

默认 D3D12 retrace 会按收集到的语义重建 replay-side device、swapchain back buffer、resource、
descriptor heap、root signature、PSO、command signature 和 command list，然后按
`ExecuteCommandLists` submission batch 重新提交。遇到尚未覆盖的 native D3D12 语义时必须明确失败，
并报告 `D3D12 native command replay incomplete`，不能回退到 present-frame playback。

逐帧验收路径应当是：第一次 trace 打开 `APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1` 生成参考帧；
再对 retrace 进程进行 trace，并打开 `APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1` 生成
retrace 实际渲染帧；最后用 `scripts/compare-d3d12-present-frames.py` 按 `frame_index` 做
raw RGBA 像素对比。这个流程验证 retrace 真实渲染结果，不改变 retrace 的 replay 语义。

为靠近 D3D11 当前进度，D3D12 retrace 已开始消费并校验这些语义记录：

- resource / root signature / PSO 创建记录及其资产引用
- device 创建记录，包括 device object id 和 `minimum_feature_level`
- root signature object id 到 serialized root signature asset、blob_refs 和 bytecode bytes 的映射
- PSO object id 到 pipeline asset path、blob_refs 和可重建 graphics/compute PSO metadata 的映射；
  graphics pipeline asset 必须包含 root signature、input layout、blend/rasterizer/depth-stencil、
  sample、RTV/DSV 和 shader 资产引用
- resource 创建参数，包括完整 resource desc、optimized clear value、Map 状态、Unmap 写入区间、buffer asset bytes 与 blob 引用的资源数据表
- GPU virtual address 到 resource object id / offset 的重定位，用于 VB/IB/root CBV/root SRV/root UAV/indirect buffer 等绑定
- descriptor heap 元数据、descriptor view 创建记录、结构化 view desc，以及 raw descriptor handle 到
  heap/index 的重定位；CBV 的 `buffer_location` 和 RTAS SRV 的 `location` 会按 GPU VA 规则重定位
- descriptor heap shader-visible 约束、`SetDescriptorHeaps` 数量/类型一致性，以及 root descriptor
  table 是否引用当前 command list 已绑定 heap
- command list 的 Reset / Close、root signature、PSO、descriptor heap、root table、RTV/DSV、viewport/scissor、barrier 绑定
- graphics / compute root 绑定，包括 32-bit constants、root CBV、root SRV 和 root UAV
- RTV / DSV clear 的 descriptor、颜色、depth/stencil、clear flags 和 rect 数量
- fence 创建、queue signal / wait、CPU fence signal、GetCompletedValue 和 SetEventOnCompletion
  的同步语义；retrace 只记录并校验应用自身同步顺序，不用 timestamp 或固定 sleep 重造帧间隔
- command allocator reset 会结合前一次提交关联的 queue signal 和后续 `SetEventOnCompletion` /
  CPU fence signal，校验应用已经表达了提交完成语义；retrace 不自行插入等待
- 多槽 VB 和多 RTV 绑定数组；旧的 `first` / `first_rtv` 只作为旧 bundle 兼容输入
- `CopyTextureRegion` 的结构化 dst/src copy location，包括 subresource index 和 placed footprint
- `CopyResource` 的 dst/src resource 关系，以及 `ResolveSubresource` 的 dst/src resource、
  subresource index 和 format
- command signature 的 indirect argument schema，以及 `ExecuteIndirect` 的 command signature、
  argument buffer、可选 count buffer 和 offset 语义
- draw / indexed draw 的完整参数，以及 dispatch / indirect / copy / resolve 调用；dispatch
  会保留 thread group 三元组，DXR `DispatchRays` 会把 shader table GPU VA 重定位到 replay
  resource 语义
- `ID3D12Resource::Map` / `Unmap` 的范围和可落盘 buffer 资产

这些语义会进入 D3D12 replay 状态跟踪与校验；Map/Unmap 写入资产会挂回对应 replay
resource，native replay 重新创建资源和 descriptor 后按原始 command stream 录制 GPU 命令。
当前覆盖默认 D3D12 验证矩阵所需的 draw / indexed draw / dispatch / indirect / copy /
resolve 路径；DXR、mesh shader 和更多 descriptor 维度仍应在扩展覆盖时按相同规则补齐。

D3D12 retrace 内部会按原始 sequence 构建 replay command stream，并把每条 command-list
语义挂回对应的 command list 状态。`ExecuteCommandLists` 会形成 completed submission batch，
并校验被提交的 command list 已经 Close，同时把 command list 的 allocator 和 descriptor heap
依赖快照并入 batch。queue wait 会作为对应 queue 下一次 submission 的前置 fence 依赖保存；
后续的 queue signal 和 Present boundary 会继续标注到对应 queue 最近一次 submission batch 上。
这些字段只保存应用 API 顺序语义，不引入 retrace 自己的时间控制。这样后续 native D3D12 command
re-emit 可以直接消费已整理好的 command stream 和 submission batch，而不是重新扫描
`callstream.jsonl`。

## 与转译层的关系

- replay 应当能够把记录下来的 D3D 调用直接送入转译层
- native replay 很重要，因为转译层可能并不需要整条链路都经过 Wine
- 如果仍然需要 Wine-hosted replay，也应复用同一套 replay library 行为

## Metal trace / retrace 集成

Metal 侧需要支持独立的 trace 和 retrace，但这条链路是给转译层 debug 用的。

它不走 Windows override，也不替代主 D3D retrace。

它的基本事实是：

- Metal trace 看不到原始 D3D 调用入口
- Metal trace 只能看到转译后的 Metal 调用
- 如果想把两边对齐，必须由转译层自己提供并消费关联信息

因此这里应有两条相关能力：

- `Metal trace`：
  由转译层显式调用 library API，记录转译后的 Metal 调用和边界
- `Metal retrace`：
  重放上述 Metal trace，用于后端级调试和对比

### 需要由转译层自己负责的信息

- D3D trace 与 Metal trace 的链接策略
- 关联字段的 schema
- 关联字段的生产与消费逻辑

`apitrace` 只负责把这些关联载荷原样记录下来，不负责解释或校验它们。

### 与主 retrace 的关系

- 主 retrace 仍然消费原始 D3D callstream
- Metal retrace 消费的是转译后的辅助 trace
- 这两条路径应可对照，但不要混成同一种存档语义

### 双 retrace 路径关系图

```text
D3D trace bundle
  └─ retrace / retrace.exe
       └─ 原始 D3D replay
            └─ native D3D12 或 Wine-hosted DXMT

Metal trace bundle
  └─ retrace --metal
       └─ Metal replay backend
            └─ 只用于转译层后端级调试
```

两条路径共享 replay session 框架，但输入 bundle、状态恢复粒度和验收目标不同。

### translation link 字段语义

`analysis/translation-links.jsonl` 现在至少包含这些字段：

- `d3d_sequence`
- `frame_id`
- `metal_sequence_begin`
- `metal_sequence_end`
- `scope_kind`
- `record_type`
- `payload`

`scope_kind` 三态分别表示：

- `command_buffer`：把一个 D3D 调用映射到整段 command buffer 生命周期
- `encoder`：把一段 render / compute / blit encoder 生命周期映射回当前 D3D 调用
- `draw_to_metal_calls`：把单条 draw / dispatch / resource / present 等具体 Metal 调用映射回当前 D3D 调用

### DXMT 接入步骤指针

DXMT 侧的静态链接边界、session 生命周期和 facade 用法，分别见：

- `docs/CAPTURE.md` 的「转译层接入流程」
- `src/metal/README.md` 的 facade 使用指南
- `docs/TRACE_LAYOUT.md` 的 Metal callstream / translation link 磁盘布局约束
