# apitrace 开发文档

本目录存放 `apitrace` 仓库的设计和开发文档。

## 文档索引

- [APITRACE.md](APITRACE.md)：capture -> trace -> retrace 的总体工作流
- [OVERVIEW.md](OVERVIEW.md)：项目定位、设计原则、目标与非目标
- [ARCHITECTURE.md](ARCHITECTURE.md)：分层架构、trace 数据模型、目录建议
- [TRACE_LAYOUT.md](TRACE_LAYOUT.md)：trace bundle 的目录布局、根索引文件和资产拆分规则
- [CAPTURE.md](CAPTURE.md)：Windows 侧 capture 入口与实现约束
- [RETRACE.md](RETRACE.md)：retrace 语义、库层实现和可执行入口
- [PLAN.md](PLAN.md)：实施阶段、里程碑和主要风险

## 阅读顺序建议

1. 先读 [OVERVIEW.md](OVERVIEW.md)，确认项目边界。
2. 再读 [ARCHITECTURE.md](ARCHITECTURE.md)，看清模块分层。
3. 再读 [TRACE_LAYOUT.md](TRACE_LAYOUT.md)，确认磁盘布局和索引入口。
4. 根据当前任务进入 [CAPTURE.md](CAPTURE.md) 或 [RETRACE.md](RETRACE.md)。
5. 实施推进时参考 [PLAN.md](PLAN.md)。

进入具体代码前，建议再读对应模块目录下的 `README.md`，确认该模块的实现边界。
