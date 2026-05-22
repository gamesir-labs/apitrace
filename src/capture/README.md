# capture

## 作用

`src/capture/` 负责 capture 侧的运行时组织、hook 计划、session 编排，以及 bundle 输出侧的接入点。

## 负责的内容

- capture 入口选项
- runtime hook 安装骨架
- hook plan 和 runtime state
- trace session 生命周期编排
- capture 输出与 bundle writer 的对接

## 不负责的内容

- 不定义 trace bundle 格式
- 不实现 retrace
- 不承载 D3D11 / D3D12 proxy 自身的模块表面
- 不实现 Metal bridge

## 当前实现边界

- `capture_options.hpp`：capture 配置
- `capture_runtime.hpp`：对外 runtime 包装
- `trace_session.hpp`：对外 session 包装
- `src/`：
  - `capture_hook_plan.*`
  - `capture_runtime_state.*`
  - `trace_session_state.*`
  - 以及面向外部接口的薄转发实现

## 头文件与实现导航

如果你要读 `src/capture/`，建议按下面顺序进入：

1. `capture_options.hpp`
   - 看 entry mode、target 和 capture 级配置
2. `capture_runtime.hpp`
   - 看 runtime 对外暴露什么能力
3. `trace_session.hpp`
   - 看 session 对外生命周期
4. `src/capture_hook_plan.*`
   - 看 capture 想装哪些 hook surface
5. `src/capture_runtime_state.*`
   - 看 runtime 当前状态和 hook plan 如何保存
6. `src/trace_session_state.*`
   - 看 runtime、bundle sink、session 生命周期如何编排

如果你要改代码，建议先判断是改：

- “capture 配置与模式”：
  从 `capture_options.hpp` 入手
- “hook 安装计划”：
  从 `capture_hook_plan.*` 入手
- “runtime 状态”：
  从 `capture_runtime_state.*` 入手
- “session 编排”：
  从 `trace_session_state.*` 入手

## 当前阶段说明

- 这里优先拆清 runtime、session、bundle sink、hook planning 的边界
- 暂不追求真正的 hook 或事件采集行为
- 后续新增实现时，应继续优先补阶段对象和 TODO，而不是把逻辑重新塞回 public 包装层

## 相关文档

- [总架构文档](../../docs/ARCHITECTURE.md)
- [capture 设计文档](../../docs/CAPTURE.md)
- [仓库文档索引](../../docs/README.md)
