# metal

## 作用

`src/metal/` 提供两件事：

- 面向 DXMT 这类转译层的 Metal trace facade
- 面向 `retrace --metal` 的 native Metal replay backend

这里记录的是“转译后的 Metal 语义”，不是主 bundle 里的原始 D3D 调用流。

## 运行期接入顺序

DXMT 侧建议按下面顺序接入：

1. native `winemetal` / `nativemetal` 静态链接 `libapitrace_platform_apple_metal.a` 与 `libapitrace_core.a`
2. 进程或 queue 生命周期开始时打开 session
3. 每次进入新的 D3D 调用前更新 `d3d_sequence`
4. 在 command buffer / encoder begin-end / 具体 Metal 调用边界调用 facade
5. 生命周期结束时关闭 session

## DXMT 视角伪代码

```c
apitrace_metal_session_t *s =
    apitrace_metal_session_open(getenv("APITRACE_METAL_BUNDLE"));

apitrace_metal_set_current_d3d_sequence(s, d3d_seq);

uint64_t cb_begin = apitrace_metal_command_buffer_begin(
    s, command_buffer_id, frame_id, label_utf8);

uint64_t enc_begin = apitrace_metal_render_encoder_begin(
    s, encoder_id, command_buffer_id, render_pass_json);

apitrace_metal_set_render_pipeline_state(s, encoder_id, pipeline_state_id);
apitrace_metal_draw_primitives(
    s, encoder_id, primitive_type, vertex_start, vertex_count,
    instance_count, base_instance);

uint64_t enc_end = apitrace_metal_current_metal_sequence(s);
apitrace_metal_render_encoder_end(s, encoder_id);
apitrace_metal_emit_link(
    s,
    APITRACE_METAL_SCOPE_ENCODER,
    d3d_seq,
    enc_begin,
    enc_end,
    "{\"encoder_id\":123}");

apitrace_metal_command_buffer_commit(s, command_buffer_id);
apitrace_metal_session_close(s);
```

## facade 分类

常用入口按职责分成：

- session 生命周期：`apitrace_metal_session_open` / `apitrace_metal_session_close`
- D3D 对齐：`apitrace_metal_set_current_d3d_sequence`
- command buffer：`apitrace_metal_command_buffer_begin` / `apitrace_metal_command_buffer_commit`
- encoder 生命周期：`apitrace_metal_render_encoder_begin`、`apitrace_metal_compute_encoder_begin`、`apitrace_metal_blit_encoder_begin` 及对应 `*_end`
- draw / dispatch / copy / resource 记录：`apitrace_metal_draw_*`、`apitrace_metal_dispatch_*`、`apitrace_metal_copy_*`
- link sideband：`apitrace_metal_emit_link`

## 文件导航

- `metal_capi.h`：给转译层直接调用的 C ABI
- `metal_trace.*`：Metal callstream 记录
- `translation_trace_recorder.*`：DXMT 风格边界与 link 写入
- `metal_state.*`：Metal 对象状态注册
- `metal_replay.*` / `metal_replay_native.mm`：native Metal replay
- `metal_diagnostics.*`：额外诊断记录

## 相关文档

- [capture 设计文档](../../docs/CAPTURE.md)
- [retrace 设计文档](../../docs/RETRACE.md)
- [trace 布局文档](../../docs/TRACE_LAYOUT.md)
