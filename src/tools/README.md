# tools

## 作用

`src/tools/` 存放用户可执行工具的代码。

## 负责的内容

- 各个 CLI 的入口组织
- 每个工具自己的参数解析和进程级行为

## 不负责的内容

- 不承载通用库逻辑
- 不重复实现 capture / trace / retrace 核心能力

## 当前实现边界

- 每个工具应使用 `src/tools/<tool-name>/src/` 的布局
- 共享逻辑应回收到对应模块，而不是在工具目录里复制

## 当前阶段说明

- 当前保留 `retrace` 和 `bundle-finalize` 工具
- 以后如果增加 inspect、validate 等工具，也应各自独立成子目录

## 相关文档

- [总架构文档](../../docs/ARCHITECTURE.md)
- [仓库文档索引](../../docs/README.md)
