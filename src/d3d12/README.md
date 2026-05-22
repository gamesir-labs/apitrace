# d3d12

## 作用

`src/d3d12/` 负责 D3D12 相关的模块表面，目前主要是 proxy 入口骨架。

## 负责的内容

- D3D12 proxy 描述信息
- D3D12 capture hook 骨架
- D3D12 replay backend 骨架
- D3D12 对象状态注册表骨架
- D3D12 submission tracker 骨架

## 不负责的内容

- 不负责通用 capture runtime
- 不负责 trace bundle 读写
- 不负责 D3D11 或 Metal 逻辑

## 当前实现边界

- `d3d12_proxy.hpp` / `d3d12_proxy.cpp`
- `d3d12_capture.hpp` / `d3d12_capture.cpp`
- `d3d12_replay.hpp` / `d3d12_replay.cpp`
- `d3d12_state.hpp` / `d3d12_state.cpp`
- `d3d12_submission.hpp` / `d3d12_submission.cpp`

## 头文件与实现导航

如果你要读 `src/d3d12/`，建议按下面顺序进入：

1. `d3d12_proxy.hpp`
   - 看 D3D12 proxy surface 的最小入口描述
2. `d3d12_capture.hpp`
   - 看 capture 侧想拦哪些 D3D12 surface
3. `d3d12_state.hpp`
   - 看 D3D12 对象状态注册表骨架
4. `d3d12_submission.hpp`
   - 看 queue / command list / batch 的提交骨架
5. `d3d12_replay.hpp`
   - 看 D3D12 replay backend 的最小生命周期

如果你要改代码，建议先判断是改：

- “入口表面”：
  从 `d3d12_proxy.*` 入手
- “capture hook”：
  从 `d3d12_capture.*` 入手
- “对象状态”：
  从 `d3d12_state.*` 入手
- “提交边界与批次”：
  从 `d3d12_submission.*` 入手
- “replay backend”：
  从 `d3d12_replay.*` 入手

## 当前阶段说明

- 这里已经从单一 proxy 文件扩成了 capture / replay / state / submission 四条骨架
- 后续如果增加 command queue、descriptor、barrier 等 D3D12 专属逻辑，应继续在本模块内部细拆，不要塞回通用模块

## 相关文档

- [总架构文档](../../docs/ARCHITECTURE.md)
- [capture 设计文档](../../docs/CAPTURE.md)
- [retrace 设计文档](../../docs/RETRACE.md)
