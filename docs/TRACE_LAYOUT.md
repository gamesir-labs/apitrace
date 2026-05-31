# Trace Bundle 目录布局

本文档只定义磁盘布局，不定义 capture hook、replay runtime 或后端行为。

仓库内有一份对应的结构样例，可参考：

- `examples/sample-trace-bundle/sample.apitrace/`

目标是三件事：

- 保证磁盘格式解耦，可拆分，可替换
- 让 shader、纹理、buffer 等资产以独立文件存在
- 让 retrace 在只读取根目录索引文件的前提下找到完整调用语义和所有资产

## 可读性原则

除了原始资产这类二进制文件以外，其它文件都应优先做到语义可读。

这里的“其它文件”包括：

- 调用语义入口
- 校验索引
- 对象关系索引
- pipeline 辅助信息
- 各类 analysis 输出

例外主要是原始资产本体，例如：

- DXBC
- DXIL
- serialized root signature
- 纹理原始内容
- buffer 原始内容

即使 DXIL 这类内容可以反汇编，它仍然属于原始资产，优先按原样保存。

## 总体原则

### 不采用分片式大文件

不使用下面这种布局：

```text
foo.trace.1
foo.trace.2
foo.trace.3
```

这种布局的主要问题是：

- 文件命名不表达语义
- shader、纹理、调用流被迫耦合在同一套切片规则里
- 后续替换、校验、去重、按类型分析都很别扭

### 一类语义，一类文件

磁盘上的每类数据都应当按语义拆开：

- 调用语义单独存放
- shader 单独存放
- 纹理单独存放
- buffer 单独存放
- pipeline 相关对象单独存放

### 根目录只保留索引入口

一个 trace bundle 的根目录只保留少量入口文件。

其中三个文件是当前格式的必需入口：

- `callstream.jsonl`：调用语义入口
- `assets.json`：资源索引入口
- `checksums.json`：校验索引

如果没有 `callstream.jsonl`，就无法做 retrace。  
如果没有 `assets.json`，retrace 只能从事件流临时推断资源闭包，无法可靠检查 `blob_refs`
是否指向了正确资源。
如果没有 `checksums.json`，就无法可靠校验 bundle 是否完整。

## 推荐目录结构

```text
sample.apitrace/
  callstream.jsonl
  assets.json
  checksums.json

  shaders/
    2f1c8e9a.dxbc
    a93e3f11.dxil
    b7710210.rootsig

  textures/
    c8321ab2.texture.zst

  buffers/
    91aa77f0.buffer.zst

  pipelines/
    18de92a1.pipeline.json

  objects/
    objects.json

  analysis/
    shader-reflection.json
    translation-links.jsonl
```

## 根目录必需文件

### `callstream.jsonl`

这是 retrace 的主入口文件。

它必须保存：

- 原始 D3D 调用顺序
- 调用类型和函数标识
- 关键参数
- 返回值和错误码
- object id / resource id / shader id 等引用
- frame / submit / present / barrier / fence 等边界语义
- 显式 debug capture 需要时，指向 present-frame 资产的 `resource_blob`

它不负责内嵌大资产本体，只负责引用它们。

推荐采用逐行记录的文本格式，例如 JSON Lines。这样有几个好处：

- 人可以直接阅读和 review
- 工具可以顺序流式处理
- 单条事件损坏时更容易定位问题
- 需要时可以单独抽取某一段调用语义

换句话说：

- `callstream.jsonl` 保存“要做什么”
- `assets.json` 保存“每个 blob id 对应哪个资源文件”
- 资产目录保存“这些调用引用的真实载荷”

### `assets.json`

这是 bundle 的资源索引入口。它把 API 调用流里的 `blob_refs` 映射到 API 无关或
API/后端相关的资源文件路径。

它必须覆盖：

- `callstream.jsonl` 引用的所有资源 blob
- `metal-callstream.jsonl` 引用的所有 Metal 资源 blob
- pipeline 描述文件内嵌引用的 shader、root signature、library 等资产

推荐字段：

```json
{
  "format_version": 1,
  "assets": [
    {
      "blob_id": 17,
      "path": "buffers/sha256-....buffer",
      "kind": "Buffer",
      "metal": false,
      "binary_payload": true,
      "byte_size": 1048576,
      "debug_name": "upload-buffer",
      "content_hash": "a3e1..."
    }
  ]
}
```

`assets.json` 是重放资源闭包的权威索引。事件流仍然保留完整调用元数据和每次调用的
`blob_refs`；reader 必须校验事件中出现的路径与 `assets.json` 中同一 `blob_id` 的路径一致。
如果后台写入先使用单调临时路径，bundle 关闭时必须先把事件流和 `assets.json` 都重写到最终
hash 路径，再生成 `checksums.json`。

`content_hash` 是资产内容的稳定身份。`fast_fingerprint` 只用于捕获期快速去重和后台 hash
排队优化，不写入最终 `assets.json`，也不能替代最终 hash。`byte_size`、`binary_payload` 和 `debug_name` 用于 retrace
预检、诊断和资源审计；API 调用参数仍然保存在 `callstream.jsonl` / `metal-callstream.jsonl`，
不能移动到资源索引里。

### `checksums.json`

这是 bundle 的完整性校验入口。

它至少应当覆盖：

- `callstream.jsonl`
- `assets.json`
- 所有 shader 文件
- 所有纹理文件
- 所有 buffer 文件
- 所有 pipeline 文件
- 其他被 `callstream.jsonl` 直接引用的文件

推荐字段：

```json
{
  "format_version": 1,
  "bundle_hash": "sha256:...",
  "files": {
    "callstream.jsonl": "sha256:...",
    "shaders/2f1c8e9a.dxbc": "sha256:...",
    "textures/c8321ab2.texture.zst": "sha256:..."
  }
}
```

`checksums.json` 的职责是校验，不是重放。  
retrace 不应从这里恢复调用语义。

## 资产目录约束

### `shaders/`

shader 必须以原始 blob 形式独立保存。

推荐规则：

- DXBC：`.dxbc`
- DXIL container：`.dxil`
- serialized root signature：`.rootsig`

不要把 shader 塞进 `callstream.jsonl`，也不要只保存反射结果。

D3D12 `CreateRootSignature` 必须记录 `node_mask`、`bytecode_size` 和
`root_signature_path`。retrace 读取时要把 root signature object id 映射到 serialized root
signature asset、`blob_refs` 和 bytecode bytes，并校验 asset 大小等于 `bytecode_size`。

### `textures/`

纹理内容独立保存，推荐压缩：

- 文件后缀可用 `.texture.zst`

存储重点是原始内容和必要布局，而不是展示友好的可视化格式。

D3D12 默认 trace 不保存 Present 前 RGBA 帧。主 retrace 路径必须根据记录到的 D3D12 调用语义
重建资源、descriptor、pipeline、command list 和 submission，并重新发出 GPU 命令。

`D3D12PresentFrame` 只用于显式 debug capture。设置
`APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1` 时，测试 runtime 可以把 Present 前 RGBA 帧保存为
`.texture` 资产；设置 `APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES=1` 时，D3D12 retrace
可以把自己真实 command replay 后、Present 前的 back buffer 读回并保存为同类 `.texture` 资产。
这些资产会在 `callstream.jsonl` 中用 `D3D12PresentFrame` resource blob 引用：

- `frame_index`：对应 Present 边界的帧序号
- `width` / `height`：帧尺寸
- `row_pitch`：每行字节数
- `sync_interval` / `flags`：捕获时应用传给 `IDXGISwapChain::Present` 的参数
- `format`：当前为 `rgba8`
- `frame_path`：指向 `textures/*.texture`

这类资产不替代原始 D3D12 调用语义，也不计入默认 D3D12 retrace 完成度。retrace 不允许把
trace 中的 `D3D12PresentFrame` 贴回 swapchain，否则 replay 会退化成视频播放。retrace 会用
`IDXGISwapChain::Present` call payload 作为 Present 参数的权威来源；如果 debug bundle 中存在
present-frame asset，retrace 只校验它与 Present call / boundary 的 `frame_index`、
`sync_interval` 和 `flags` 一致。present-frame asset 的文件大小必须等于
`row_pitch * height`。

逐帧画面对比应在 replay 之外完成，例如用 `scripts/lib/present_frame_compare.py --api d3d12`
比较第一次 trace 和“对 retrace 再 trace”得到的两个 bundle。该对比工具按 `frame_index`
读取 `D3D12PresentFrame` raw RGBA 资产，使用 `100` 像素非重叠 tile 做验收；每个 tile
至少 `95%` 像素完全匹配，任一 tile 不达标则该帧失败。工具可选输出 failed-tile mask PNG
供人工检查，但 PNG 不作为仓库内静态 fixture。

### `buffers/`

buffer 初始数据或快照独立保存，推荐压缩：

- 文件后缀可用 `.buffer.zst`

D3D12 的 `ID3D12Resource::Unmap` 如果带有非空 written range，capture 会把该范围保存为
buffer 资产，并在对应调用 payload 中写入 `buffer_path`。这与 D3D11 的 Map/Unmap 资源内容
记录语义保持一致。retrace 会按 resource object id 把 `written_begin` / `written_end`、
`buffer_path`、`blob_refs` 和资产 bytes 挂回对应资源状态，并校验资产大小等于 written range
长度；D3D12 后续仍需要按资源布局和 descriptor 重定位恢复到真实 replay 资源。

D3D12 buffer resource 的 `CreateCommittedResource` payload 应记录 `gpu_virtual_address`。
command-list payload 中的 `buffer_location` 仍保存原始 API 传入的 GPU VA，但 retrace 必须用
`gpu_virtual_address + resource_desc.width` 把它重定位为 `resource object id + offset`，而不是
直接复用捕获进程里的绝对地址。

`CreateCommittedResource` 的 `resource_desc` 必须保留 D3D12 resource desc 的完整基础字段：
`dimension`、`alignment`、`width`、`height`、`depth_or_array_size`、`mip_levels`、`format`、
`sample_count`、`sample_quality`、`layout` 和 `flags`。retrace 必须把这些字段挂回 resource
状态，供真实 resource 重新创建使用。
如果应用传入 `optimized_clear_value`，trace 必须记录 `format`、`color[4]`、`depth` 和
`stencil`；没有传入时记录为 `null`。retrace 必须把该对象挂回 resource 创建状态。

`CreateHeap` 必须记录 `size_in_bytes`、`alignment`、`heap_type` 和 `flags`，并把 heap object
作为后续 placed resource 的父语义。`CreatePlacedResource` 必须记录 `heap_object_id`、
`heap_offset`、`initial_state`、`gpu_virtual_address`、完整 `resource_desc` 和
`optimized_clear_value`。retrace 必须先重建 heap，再用 `CreatePlacedResource` 重建 resource；
不能把 placed resource 简化为 committed resource，否则 aliasing 和 heap 语义会改变。

root descriptor 绑定也遵循同一规则。`SetGraphicsRootConstantBufferView`、
`SetComputeRootConstantBufferView`、`SetGraphicsRootShaderResourceView`、
`SetComputeRootShaderResourceView`、`SetGraphicsRootUnorderedAccessView`、
`SetComputeRootUnorderedAccessView` 必须记录 `root_parameter_index` 和原始 `buffer_location`；
retrace 再把 GPU VA 重定位为 resource object id + offset，并按 graphics/compute 与 CBV/SRV/UAV
分别挂回 command list 状态。

root 32-bit constants 不经过 GPU VA 重定位，但仍属于 command list 绑定状态。
`SetGraphicsRoot32BitConstant`、`SetComputeRoot32BitConstant`、
`SetGraphicsRoot32BitConstants`、`SetComputeRoot32BitConstants` 必须记录
`root_parameter_index`、`constant_count`、`dst_offset` 和 `values[]`。单值 API 记录为
`constant_count = 1` 且 `values` 只有一个元素。retrace 必须校验 `values[]` 长度与
`constant_count` 一致，并按 graphics/compute 分别挂回 command list 状态。

clear 操作必须保存应用传入的数据。`ClearRenderTargetView` 记录 descriptor、`color[4]`、
`rect_count` 和 `rects[]`；`ClearDepthStencilView` 记录 descriptor、`clear_flags`、`depth`、
`stencil`、`rect_count` 和 `rects[]`。`first_rect` 只作为旧 bundle 兼容字段。retrace 必须把
descriptor 重定位并把这些值挂回 command list 状态，不能只把 clear 当作计数。

多槽绑定必须保存完整数组而不是只保存首项。D3D12 `IASetVertexBuffers` 应写入 `views[]`，
每项包含 `buffer_location`、`size_in_bytes`、`stride_in_bytes`；`OMSetRenderTargets`
应写入 `render_targets[]`，每项为原始 CPU descriptor handle。`first` / `first_rtv`
只能作为兼容旧 bundle 的冗余字段，不能作为长期 replay 语义。

raster state 同样属于 draw 前状态。`RSSetViewports` 应记录 `viewport_count` 和
`viewports[]`，每项包含 `x`、`y`、`width`、`height`、`min_depth`、`max_depth`；
`RSSetScissorRects` 应记录 `rect_count` 和 `rects[]`，每项包含 `left`、`top`、`right`、
`bottom`。`first` 只作为旧 bundle 兼容字段。`IASetPrimitiveTopology` 记录
`primitive_topology`。retrace 必须把这些字段挂回 command list 状态，供后续真实 command
重新录制消费。

`CopyTextureRegion` 必须记录结构化 `dst` / `src` copy location。每个 location 至少包含
`resource_object_id` 和 `type`；subresource location 记录 `subresource_index`，placed footprint
location 记录 `offset`、`format`、`width`、`height`、`depth`、`row_pitch`。旧的 `dst_type` /
`src_type` 只保留为兼容字段，不足以驱动真实 texture copy replay。

`CopyResource` 必须保留 dst / src resource object id。`ResolveSubresource` 必须保留 dst / src
resource object id、`dst_subresource`、`src_subresource` 和 `format`。retrace 应把这些记录挂回
对应 command list 状态，并校验被引用的 resource 已经由前序 create 语义建立。

同步语义必须来自应用自身的 D3D12 调用，而不是 retrace 额外插入的时间控制。
`CreateFence` 记录 `initial_value` 和 `flags`；`ID3D12CommandQueue::Signal`、
`ID3D12CommandQueue::Wait`、`ID3D12Fence::Signal`、`ID3D12Fence::SetEventOnCompletion`
记录 `fence_value` 和 fence object 引用。`ID3D12Fence::GetCompletedValue` 记录
`completed_value`，作为应用观察到的 fence 完成状态。retrace 用这些记录重建 queue/fence
顺序状态；不得把 wall-clock timestamp、固定 sleep 或额外帧间隔当作 replay 输入。

`CreateCommandSignature` 必须记录 indirect argument schema。payload 至少包含
`byte_stride`、`argument_count`、`node_mask` 和 `arguments[]`；每个 argument 记录 `type`。当
argument 是 `VERTEX_BUFFER_VIEW` 时记录 `slot`；当 argument 是 `CONSTANT` 时记录
`root_parameter_index`、`dest_offset_in32bit_values`、`num32bit_values_to_set`；当 argument 是
root descriptor view 时记录对应的 `root_parameter_index`。`ExecuteIndirect` payload 保留原始
`max_command_count`、`arg_buffer_offset`、`count_buffer_offset`，object refs 负责指向 command
signature、argument buffer 和可选 count buffer。retrace 需要用这些语义重建 indirect 调用，
不能只把它当作普通 draw 计数。

`Dispatch` / `DispatchMesh` 必须保存原始 thread group 三元组：
`thread_group_count_x`、`thread_group_count_y`、`thread_group_count_z`。`DispatchRays` 除
`width`、`height`、`depth` 外，还需要记录 ray generation、miss、hit group、callable shader
table 的 `start_address`、`size_in_bytes` 和适用时的 `stride_in_bytes`。这些 GPU VA 字段和
VB / IB / root CBV 一样，retrace 必须重定位为 resource object id + offset，不能直接复用捕获
进程里的绝对地址。

draw 调用必须保存应用传入的完整 draw 参数。`DrawInstanced` 记录
`vertex_count_per_instance`、`instance_count`、`start_vertex_location` 和
`start_instance_location`；`DrawIndexedInstanced` 记录 `index_count_per_instance`、
`instance_count`、`start_index_location`、`base_vertex_location` 和
`start_instance_location`。retrace 必须把这些字段挂回 command list 状态，不能只累计 draw
数量。

descriptor view 创建不能只保存 descriptor handle。`CreateShaderResourceView`、
`CreateUnorderedAccessView`、`CreateRenderTargetView`、`CreateDepthStencilView` 应保存
`format`、`view_dimension` 和结构化 `view` 子对象。`view` 子对象按具体 view dimension
记录 mip、array slice、plane slice、buffer element、structure stride 等字段；RTAS SRV 还要记录
`location`，并在 retrace 时按 GPU VA 规则重定位。SRV 还需要保存
`shader_4_component_mapping`，DSV 还需要保存 `flags`。当应用传入空 desc、由 D3D12 默认推导 view
时，`view` 记录为 `null`，这仍然是有效语义。非空 desc 的字段是后续真实 descriptor heap 重建的输入。
`CreateConstantBufferView` 记录 `buffer_location` 和 `size_in_bytes`，retrace 必须把
`buffer_location` 重定位为 resource object id + offset 后再挂回 descriptor 状态。
`CreateSampler` 必须记录 `descriptor`、`filter`、`address_u`、`address_v`、`address_w`、
`mip_lod_bias`、`max_anisotropy`、`comparison_func`、`border_color[4]`、`min_lod` 和 `max_lod`。
descriptor handle 只保存在 payload 中；它不是 D3D object，不能写入 `object_refs`。retrace
必须通过 descriptor heap metadata 把 raw descriptor handle 重定位为 `heap object id +
descriptor index` 后重建 sampler。

### D3D12 descriptor heap metadata

`D3D12CreateDevice` 必须记录创建出的 device object id 和 `minimum_feature_level`。retrace
用这个记录建立后续 queue / allocator / resource / descriptor / pipeline 创建的根对象语义，并把
失败的 device 创建作为不可 replay 的 trace 处理。

D3D11 `IDXGISwapChain::Present` call 和 Present boundary 也必须记录同一个连续递增的
`frame_index`，并保留 `sync_interval` / `flags`。D3D11 retrace 直接把这些原始 Present 参数传给
replay swapchain，不使用额外 sleep、timestamp 或固定帧间隔。

`ID3D12Device::CreateDescriptorHeap` 除了 heap type、descriptor 数量和 flags，还需要记录：

- `descriptor_size`：同类型 descriptor handle 的增量
- `cpu_start`：heap 起始 CPU descriptor handle
- `gpu_start`：shader-visible heap 的起始 GPU descriptor handle；非 shader-visible heap 为 `0`

这些字段只用于把捕获进程里的 raw descriptor handle 转成 `heap object id + descriptor index`
这类可重定位语义。retrace 不应直接复用捕获进程的绝对 handle。
retrace 还必须校验 shader-visible heap 只出现在 CBV/SRV/UAV 或 sampler heap 类型上，
shader-visible heap 必须有 `gpu_start`，非 shader-visible heap 不应有 `gpu_start`。
`SetDescriptorHeaps` 记录的 heap 数量必须和 object refs 一致，并且同一调用中不能绑定重复 heap
类型；root descriptor table 只能引用当前 command list 已绑定的 shader-visible heap。

### `pipelines/`

pipeline 相关结构独立保存为便于 review 的中间格式，例如：

- `.pipeline.json`

但要注意：

- 这是辅助资产
- 不能替代 `callstream.jsonl` 中的调用语义

retrace 读取 `CreateGraphicsPipelineState` / `CreateComputePipelineState` 时必须把 pipeline object id
映射到对应 `pipeline_path`、`blob_refs` 和关键 metadata。graphics pipeline 至少保留
`root_signature_object_id`、`input_layout`、`blend_state`、`rasterizer_state`、`depth_stencil_state`、
`sample_mask`、`sample_desc`、`primitive_topology_type`、`num_render_targets`、`rtv_formats[]`、
`dsv_format`、`ib_strip_cut_value`、`node_mask`、`flags` 以及 VS/PS/DS/HS/GS shader asset 引用；
compute pipeline 至少保留 `root_signature_object_id`、`node_mask`、`flags` 和 CS shader asset 引用。
这些字段必须足以重建 D3D12 PSO；不能只保存摘要或反射结果。

### `objects/`

对象图可以集中存放在：

- `objects/objects.json`

它用于加速对象关系恢复，但不是 retrace 的唯一依据。

### `analysis/`

`analysis/` 用于存放可读的派生信息和辅助 sideband 流。

例如：

- `shader-reflection.json`
- `translation-links.jsonl`

其中 `translation-links.jsonl` 的定位是：

- 可选文件
- 给转译层 debug 使用的辅助链接流
- 由 `apitrace` 提供的库 API 写入
- `apitrace` 不解释其中的链接语义，只负责保存

## Metal callstream 与资产

当 bundle 同时携带转译后的 Metal 辅助 trace 时，根目录额外包含：

- `metal-callstream.jsonl`
- `metal/`
- `analysis/translation-links.jsonl`

### `metal-callstream.jsonl`

这是转译层显式调用 Metal facade 后落下的逐行事件流，推荐字段如下：

- `metal_sequence`
- `d3d_sequence`
- `frame_id`
- `call_kind`
- `function`
- `object_id`
- `object_refs`
- `blob_refs`
- `payload`

其中：

- `command_buffer_begin` / `command_buffer_commit` 表示 command buffer 生命周期
- `render_encoder_begin` / `compute_encoder_begin` / `blit_encoder_begin` 表示 encoder 生命周期
- `draw_*` / `dispatch_*` / `copy_*` / `present_*` 表示真实转译后的 Metal 调用。
  `copy_buffer_to_texture` 这类 blit 操作必须作为正式 `call_kind` 记录，不能用 debug
  signpost 的 label 承载真实 replay 语义。
- 如果 blit 操作依赖 CPU 可见 staging buffer 中的临时内容，payload 应引用
  `source_asset_path` / `source_asset_size`。该资产写入顶层 `buffers/`，retrace 在发出
  Metal blit 前恢复 source buffer 内容；这仍是资源状态输入，不是记录并播放 Present 帧。

### `metal/` 目录命名规则

`metal/` 下只保存不能跨 API 复用的 Metal 原生资产：

- `metal/libraries/*.metallib`
- `metal/pipelines/*.pipeline.json`

约束与主 bundle 保持一致：

- 文件名优先使用内容哈希
- 行语义文件只引用路径，不内嵌二进制资产本体
- pipeline JSON 必须能独立提供函数引用、像素格式、sample count 等 replay 所需字段
- buffer 和 texture 是 API 无关资源，统一写入顶层 `buffers/`、`textures/`，D3D 与
  Metal 两侧事件都通过路径/blob id 引用同一份资源。

### `MetalPresentFrame` resource blob

如果 Metal trace 或 Metal retrace 显式打开 present-frame 读回，`callstream.jsonl` /
`metal-callstream.jsonl` 中允许出现 `debug_name = "MetalPresentFrame"` 的 resource blob。推荐字段：

- `frame_index`
- `width`
- `height`
- `row_pitch`
- `format`
- `frame_path`

其文件大小必须等于 `row_pitch * height`，且只用于像素级对比，不替代 replay 输入语义。
`MetalPresentFrame` 还必须能按 `frame_index` 匹配同一 bundle 的 `PresentDrawable` Metal
call；`width`、`height`、`sync_interval` 和 `flags` 必须一致。

### `analysis/translation-links.jsonl`

translation link 现在至少使用这些字段：

- `d3d_sequence`
- `frame_id`
- `metal_sequence_begin`
- `metal_sequence_end`
- `scope_kind`
- `record_type`
- `payload`

`scope_kind` 约束为：

- `command_buffer`
- `encoder`
- `draw_to_metal_calls`

每个非零 `d3d_sequence` 至少应能聚合到一条 `encoder` 和一条 `draw_to_metal_calls` 记录，供
`scripts/test-d3d-metal-link.sh` 内置覆盖校验使用。

## 文件引用规则

`callstream.jsonl` 中不直接内嵌大块资产内容，而是通过稳定 id 或路径引用：

- `shader_id`
- `texture_id`
- `buffer_id`
- `object_id`

这些引用最终解析到具体文件，例如：

- `shader_id -> shaders/2f1c8e9a.dxil`
- `texture_id -> textures/c8321ab2.texture.zst`

## 命名规则

文件名应该表达内容类型，而不是表达“第几片”。

推荐：

- `2f1c8e9a.dxil`
- `91aa77f0.buffer.zst`
- `18de92a1.pipeline.json`

不推荐：

- `trace.1`
- `trace.2`
- `bundle.part3`

更稳妥的方式是使用内容 hash 或稳定 id 作为文件名主体。

## 派生数据和原始数据分离

原始数据和派生分析结果必须分开。

例如：

- 原始 DXIL / DXBC 放在 `shaders/`
- shader reflection 放在 `analysis/`

不要让派生数据覆盖原始输入。

## retrace 的最小读取路径

一个最小的 retrace 实现至少需要读取：

1. `callstream.jsonl`
2. `assets.json`
3. `checksums.json`
4. `callstream.jsonl` 引用到的资产文件

`analysis/translation-links.jsonl` 这类辅助流不属于最小 retrace 必需输入。
它们存在时可用于 debug、对比和转译层自定义分析。

其中：

- `callstream.jsonl` 提供调用语义
- `assets.json` 提供 `blob_refs` 到资源路径的权威映射
- `checksums.json` 提供完整性校验
- 资产目录提供 shader、纹理、buffer 等真实载荷

默认 retrace 只校验它将要读取的最小闭包，以免大 trace 启动前重复扫描所有资源。
需要完整检查 bundle 时，使用 `bundle-check <bundle>`；它会读取 `checksums.json`
中的全部条目，确认文件存在、digest 匹配，并校验 manifest 级 `bundle_hash` 是否与文件列表一致。

双侧 D3D→Metal 调试 bundle 可以打开严格项：

```sh
bundle-check --strict-cross-api path/to/scene.apitrace
```

双侧 bundle 的严格检查必须同时覆盖 D3D 原始语义闭包和 Metal 原生闭包；translation link 只能
说明两条时间线的对应关系，不能替代 D3D 侧 pipeline、shader、root signature 等原始重放输入。
当 Metal 侧出现 draw/dispatch workload 时，`draw_to_metal_calls` link 还必须指向 D3D 侧的
pipeline-dependent API 记录，例如 `SetPipelineState`、`Draw*` 或 `Dispatch*`；不能只链接到
`ExecuteCommandLists`、frame boundary 或其他调度壳层记录。

对 D3D12 原始重放闭包可以增加：

```sh
bundle-check --require-d3d --require-d3d-replay-closure --require-d3d-native-readiness --require-d3d-present-frames path/to/d3d12.apitrace
```

这个检查要求 D3D callstream 至少引用独立的 pipeline、shader 和 root signature 资产。它只检查
D3D 原始语义闭包，不把 Metal 转译产物当作 D3D 侧资产的替代品。
`--require-d3d-native-readiness` 会把 callstream 喂给 D3D12 replay backend 并执行
validate-only 语义预检；它不创建图形设备，但会拒绝当前只能由 DXMT 转译后端支持、不能直接
native replay 的语义。`--require-d3d-present-frames` 还要求 `D3D11PresentFrame` 或
`D3D12PresentFrame` debug 资产能逐帧匹配 captured Present call 和 Present boundary，用于
后续 tile compare 输入检查。

对 Metal 原生重放闭包可以增加：

```sh
bundle-check --require-metal --require-metal-replay-closure --require-metal-present-frames path/to/metal.apitrace
retrace --metal --validate-only path/to/metal.apitrace
```

这个检查要求 Metal callstream 引用独立的 metallib 和 pipeline 资产，并包含 pipeline bind 与
draw/dispatch 调用。`--validate-only` 不创建 `MTLDevice`，用于大 trace 在真实 Metal replay 前的
闭包预检。`--require-metal-present-frames` 要求 `MetalPresentFrame` debug 资产能按 `frame_index`
匹配同一 bundle 的 `PresentDrawable` 调用。

严格共享资源检查要求 buffer/texture 存在于 API 无关的 `buffers/` 或 `textures/` 目录，
并要求 D3D callstream 和 Metal callstream 通过路径引用同一个资源资产。该检查会分别统计
buffer 和 texture 的跨侧共享路径；如果两侧都引用某类资源，就必须至少有一个同类共享路径。
PresentFrame debug 资产只用于画面对比，不计入游戏资源共享统计。shader、pipeline 和 Metal
library 这类 API/后端相关资产仍保持各自目录。

合成双侧 smoke 可用以下脚本固定验证这些条件：

```sh
scripts/test-cross-api-smoke.sh
```

该脚本会生成同一个 bundle 内的 D3D12 与 Metal 调用流，运行全 strict `bundle-check`、D3D/Metal
validate-only，并对 D3D12 PresentFrame 与 Metal PresentFrame 执行 `d3d12-to-metal` tile compare。
在 Apple Metal 可用环境中，该脚本还会运行 `apitrace_test_metal_native_replay_smoke`：fixture 写入真实
`metallib`、pipeline、buffer 和离屏 render target 元数据，用非 validate-only `retrace --metal` 执行
native Metal 命令并捕获 retrace 帧。验收基准来自同一 `metallib` 和几何数据直接执行的原生 Metal
离屏渲染输出；捕获帧只用于比较，不能作为 retrace 输入或 frame-player 路径。

## 最小可用 bundle

最小可用 bundle 必须包含：

1. `callstream.jsonl`
2. `assets.json`
3. `checksums.json`
4. `callstream.jsonl` 直接引用到的资产文件

`pipelines/`、`objects/` 和 `analysis/` 可以按 bundle 实际内容出现。它们提供可读的结构化资产、
对象关系索引和辅助分析流，但不能替代 `callstream.jsonl` 里的原始调用语义。

## 与现有代码命名的关系

当前代码里的 `TraceArchiveWriter/Reader` 更像单体容器命名。

如果按本文档推进，后续更合理的名字是：

- `TraceBundleWriter`
- `TraceBundleReader`

因为这里的物理存储单元不再是假定的单文件 archive，而是一个目录 bundle。
