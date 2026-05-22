# metal

## 作用

`src/metal/` 负责 Metal 原生后端的模块边界，服务于转译后 Metal 语义的 trace、对应的 native retrace 落点，以及后续与转译层之间的 Metal 适配面。

## 负责的内容

- Metal bridge 的最小公共接口
- Metal trace backend 骨架
- 面向转译层植入的 trace recorder facade 骨架
- Metal replay backend 骨架
- Metal 对象状态注册表骨架
- Metal diagnostics 记录骨架

## 不负责的内容

- 不负责原始 D3D capture
- 不负责 trace bundle 格式
- 不负责 replay session 编排

## 当前实现边界

- `metal_bridge.hpp` / `metal_bridge.cpp`
- `metal_trace.hpp` / `metal_trace.cpp`
- `translation_trace_recorder.hpp` / `translation_trace_recorder.cpp`
- `metal_state.hpp` / `metal_state.cpp`
- `metal_diagnostics.hpp` / `metal_diagnostics.cpp`
- `metal_replay.hpp` / `metal_replay.cpp`

## 头文件与实现导航

如果你要读 `src/metal/`，建议按下面顺序进入：

1. `metal_bridge.hpp`
   - 看 Metal 原生设备/队列桥接面的最小约束
2. `metal_trace.hpp`
   - 看 Metal 侧记录入口，以及转译层自带关联载荷的透传边界
3. `translation_trace_recorder.hpp`
   - 看面向 DXMT 这类转译层的 `command buffer / encoder / marker` 植入接口
4. `metal_state.hpp`
   - 看 Metal 对象状态注册表骨架
5. `metal_diagnostics.hpp`
   - 看运行期诊断如何暂存
6. `metal_replay.hpp`
   - 看原生 Metal replay backend 的最小生命周期

如果你要改代码，建议先判断是改：

- “原生桥接面”：
  从 `metal_bridge.*` 入手
- “转译后 Metal trace 生命周期”：
  从 `metal_trace.*` 入手
- “转译层植入 facade”：
  从 `translation_trace_recorder.*` 入手
- “对象状态”：
  从 `metal_state.*` 入手
- “诊断记录”：
  从 `metal_diagnostics.*` 入手
- “replay backend”：
  从 `metal_replay.*` 入手

## 当前阶段说明

- 这里已经从单一 bridge/capture 文件扩成了 bridge / trace / state / diagnostics / replay 五条骨架
- 当前仍然是架构设计阶段，所有实现都只保留最小占位和明确 TODO
- 这里的 trace 记录的是“转译后的 Metal 语义”，不是主存档里的原始 D3D 调用流
- D3D 记录与 Metal 记录的链接语义不由本模块定义，而由转译层自己决定和消费
- 当前 facade 的 API 形状刻意贴近 DXMT 这类转译层已有的 `chunk -> encoder -> marker` 边界，避免要求调用方自己拼 bundle 细节
- 后续真正接转译层 trace 注入或原生 replay 时，应继续在本模块内部细拆，而不要把平台细节塞回通用模块

## 相关文档

- [总架构文档](../../docs/ARCHITECTURE.md)
- [capture 设计文档](../../docs/CAPTURE.md)
- [retrace 设计文档](../../docs/RETRACE.md)
- [仓库文档索引](../../docs/README.md)
