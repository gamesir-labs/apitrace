# retrace tool

## 作用

`src/tools/retrace/` 是用户可执行的 `retrace` 命令行工具入口。

## 负责的内容

- 最小命令行参数解析
- 构造 `ReplayOptions`
- 可选调用同目录或 PATH 中的 `bundle-finalize`
- 调用 `ReplaySession`
- 返回进程退出码和基本错误输出

## 不负责的内容

- 不实现 replay 核心逻辑
- 不直接解析 bundle 内部格式
- 不承载 backend-specific replay 代码

## 当前实现边界

- `src/main.cpp` 是单入口
- 真正的 replay 行为必须继续留在 `src/retrace/`

## 当前阶段说明

- 当前目标是保持 CLI 足够窄：一个 bundle 输入，按需先离线最终化，再执行 replay
- 后续如果参数变多，也应先确保 CLI 仍只是薄包装，不要把逻辑灌进工具入口

## 相关文档

- [retrace 模块 README](../../retrace/README.md)
- [总架构文档](../../../docs/ARCHITECTURE.md)
- [retrace 设计文档](../../../docs/RETRACE.md)
