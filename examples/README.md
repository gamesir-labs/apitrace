# examples

本目录存放 `apitrace` 的结构样例。

当前提供：

- `sample-trace-bundle/sample.apitrace/`：一份完整 trace bundle 的目录结构示例

注意：

- 这些文件主要用于架构 review
- 目录结构和文件命名应被视为有效参考
- 资产文件内容、`checksums.json` 中的摘要值、以及 `callstream.jsonl` 中的调用参数都只是占位样例
- `analysis/translation-links.jsonl` 这类辅助 sideband 文件同样只是占位样例
- 这些样例不保证可以被真实 retrace 实现直接消费
