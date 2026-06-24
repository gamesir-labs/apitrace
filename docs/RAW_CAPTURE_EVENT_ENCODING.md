# Raw Capture 事件编码契约

本文档记录 Phase 1b 的代表性 raw event 二进制载荷契约。该契约只定义
capture 与 finalize 之间的便宜结构化数据，不定义最终 bundle 的文本格式。

最终 bundle 仍由 `bundle-finalize` 生成：

- `callstream.jsonl`
- `assets.json`
- 规范化后的 `buffers/`、`shaders/`、`pipelines/` 等资产文件
- `checksums.json`

capture 侧 raw payload 不写 JSON、hash、canonical path 或 dedup 结果。
这些都是 finalize-neutral 工作，必须留给离线 finalize 阶段处理。

## 通用规则

- 文件格式版本：`kRawCaptureFormatVersion = 1`
- 事件契约版本：`kRawEventContractVersion = 1`
- 整数编码：little-endian
- 字符串编码：`u32 byte_length` 后跟原始 UTF-8/字节内容
- 每条事件外层仍使用 Phase 1a 的 `RawEventHeader`：
  - `u64 sequence`
  - `u64 thread_id`
  - `u64 timestamp_or_monotonic_counter`
  - `u32 opcode`
  - `u32 result_or_flags`
  - `u64 payload_len`
- blob 字节存放在 `raw/blobs.bin`，索引存放在 `raw/blobs.idx`。
- raw blob id 只在 raw capture 内稳定；`raw_to_final` 会分配最终 `BlobId`。

## Raw Blob Kind

| 值 | 名称 | 最终资产类型 |
|---:|---|---|
| 0 | `Unknown` | 不允许用于需要资产类型的事件 |
| 1 | `Buffer` | `AssetKind::Buffer` |
| 2 | `ShaderDxbc` | `AssetKind::ShaderDxbc` |
| 3 | `ShaderDxil` | `AssetKind::ShaderDxil` |
| 4 | `RootSignature` | `AssetKind::RootSignature` |

## Opcode Payload

### `0x0101 ResourceCreate`

载荷：

```text
u64 device_object_id
u64 resource_object_id
u64 dimension
u64 width
u32 height
u16 depth_or_array_size
u16 mip_levels
u32 format
u32 flags
u32 initial_state
str debug_name
```

finalize 输出：

- `EventKind::ObjectCreate`
- `object_kind = Resource`
- `object_id = resource_object_id`
- `parent_object_id = device_object_id`
- payload 保存 resource desc 结构字段

### `0x0102 ResourceUnmap`

载荷：

```text
u64 resource_object_id
u64 raw_blob_id
u64 written_begin
u64 written_end
```

约束：

- `raw_blob_id` 必须指向 `RawBlobKind::Buffer`。

finalize 输出：

- `ID3D12Resource::Unmap`
- `object_refs = [resource_object_id]`
- `blob_refs = [final_buffer_blob_id]`
- payload 包含 `written_range` 和最终 buffer path

### `0x0201 GraphicsPipelineCreate`

载荷：

```text
u64 device_object_id
u64 root_signature_object_id
u64 pipeline_state_object_id
u64 vs_raw_blob_id
u64 vs_bytecode_size
u64 ps_raw_blob_id
u64 ps_bytecode_size
u32 node_mask
u32 flags
u32 payload_flags
```

约束：

- `vs_raw_blob_id` 和 `ps_raw_blob_id` 必须指向 shader raw blob。
- `payload_flags & 1` 表示 `requires_dxmt_backend`。

finalize 输出：

- `ID3D12Device::CreateGraphicsPipelineState`
- `object_refs = [device, root_signature, pipeline_state]`
- `blob_refs` 先包含 shader final blob ids
- payload 使用现有 `pso_raw_version:1` raw PSO schema
- 后续现有 `bundle-finalize` 阶段生成 `pipeline_path` 和 pipeline asset

### `0x0301 DrawInstanced`

载荷：

```text
u64 command_list_object_id
u32 vertex_count_per_instance
u32 instance_count
u32 start_vertex_location
u32 start_instance_location
```

finalize 输出：

- `ID3D12GraphicsCommandList::DrawInstanced`
- `object_refs = [command_list_object_id]`
- payload 保存四个 draw 参数

### `0x0302 Dispatch`

载荷：

```text
u64 command_list_object_id
u32 thread_group_count_x
u32 thread_group_count_y
u32 thread_group_count_z
```

finalize 输出：

- `ID3D12GraphicsCommandList::Dispatch`
- `object_refs = [command_list_object_id]`
- payload 保存三维 dispatch 参数

### `0x0401 PresentCall`

载荷：

```text
u64 swap_chain_object_id
u64 frame_index
u32 sync_interval
u32 flags
```

finalize 输出：

- `IDXGISwapChain::Present`
- `object_refs = [swap_chain_object_id]`
- payload 保存 `frame_index`、`sync_interval`、`flags`

### `0x0404 PresentBoundary`

载荷同 `PresentCall`。

finalize 输出：

- `BoundaryKind::Present`
- payload 必须与同一 `frame_index` 的 Present call 匹配

### `0x0402 FrameBegin`

载荷：

```text
u64 frame_index
```

finalize 输出：

- `BoundaryKind::Frame`
- payload: `{"frame_index":N,"label":"FrameBegin"}`

### `0x0403 FrameEnd`

载荷：

```text
u64 frame_index
```

finalize 输出：

- `BoundaryKind::Frame`
- payload: `{"frame_index":N,"label":"FrameEnd"}`

## 当前范围外

- live D3D12 capture encoder 接入
- 完整 D3D12 event 覆盖
- descriptor、barrier、copy/resolve、indirect、fence 等完整 replay 闭包
- 多 API/Metal sideband raw event 编码
- capture-time hash、dedup 或 canonical path
