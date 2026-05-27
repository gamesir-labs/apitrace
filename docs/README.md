# apitrace 开发文档

本目录存放 `apitrace` 的技术文档。根目录 README 面向使用入口；本目录说明
trace 格式、capture / retrace 语义、模块边界和构建验证流程。

## 文档索引

- [APITRACE.md](APITRACE.md)：capture -> trace -> retrace 的总体工作流
- [OVERVIEW.md](OVERVIEW.md)：项目定位、设计原则、目标与非目标
- [TECHNICAL_DETAILS.md](TECHNICAL_DETAILS.md)：当前功能、运行链路、环境变量和验证方式
- [ARCHITECTURE.md](ARCHITECTURE.md)：分层架构、trace 数据模型、模块职责
- [TRACE_LAYOUT.md](TRACE_LAYOUT.md)：trace bundle 的目录布局、根索引文件和资产拆分规则
- [CAPTURE.md](CAPTURE.md)：Windows 侧 capture 入口与实现约束
- [RETRACE.md](RETRACE.md)：retrace 语义、库层实现和可执行入口
- [BUILD.md](BUILD.md)：本仓库的构建流程、host build 与 Windows 交叉编译

## 阅读顺序

1. 先读 [OVERVIEW.md](OVERVIEW.md)，确认项目边界和非目标。
2. 再读 [TECHNICAL_DETAILS.md](TECHNICAL_DETAILS.md)，了解当前可用产物、命令和验证方式。
3. 再读 [TRACE_LAYOUT.md](TRACE_LAYOUT.md)，确认磁盘布局和索引入口。
4. 根据任务进入 [CAPTURE.md](CAPTURE.md)、[RETRACE.md](RETRACE.md) 或模块 README。

进入具体代码前，阅读对应模块目录下的 `README.md`，确认该模块的实现边界。
