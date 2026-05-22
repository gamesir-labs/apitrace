# retrace

## 作用

`src/retrace/` 负责 replay 侧的公共选项、session 骨架，以及 bundle -> replay dispatch 的入口编排。

## 负责的内容

- replay 选项定义
- replay session 生命周期
- bundle reader 与 replay 过程之间的接入点

## 不负责的内容

- 不定义 trace bundle 的存储格式
- 不负责 capture
- 不直接承载 D3D11 / D3D12 / Metal backend 实现
- 不负责命令行参数解析细节

## 当前实现边界

- `replay_options.hpp`：backend 选择与 replay 选项
- `replay_session.hpp`：session 对外接口
- `src/replay_session.cpp`：最小读取和后续 backend 调度骨架

## 头文件与实现导航

如果你要读 `src/retrace/`，建议按下面顺序进入：

1. `replay_options.hpp`
   - 看 bundle_root、backend 和 replay 级策略
2. `replay_session.hpp`
   - 看 replay session 对外接口
3. `src/replay_session.cpp`
   - 看 bundle reader、校验、callstream 解析和 backend dispatch 预留点

如果你要改代码，建议先判断是改：

- “replay 输入与策略”：
  从 `replay_options.hpp` 入手
- “replay 生命周期和阶段”：
  从 `replay_session.hpp` / `replay_session.cpp` 入手
- “真正 backend replay”：
  后续新增更细模块，不要继续把逻辑堆进当前 session 文件

## 当前阶段说明

- 这里优先稳定 replay 入口的层级关系
- 真正的 backend replay 逻辑后续应继续往更细的模块拆
- 当前 TODO 主要聚焦在 bundle 校验、callstream 解析和 backend dispatch 之间的阶段划分

## 相关文档

- [总架构文档](../../docs/ARCHITECTURE.md)
- [retrace 设计文档](../../docs/RETRACE.md)
- [目录布局文档](../../docs/TRACE_LAYOUT.md)
- [仓库文档索引](../../docs/README.md)
