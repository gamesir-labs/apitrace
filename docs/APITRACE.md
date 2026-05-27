# API Trace 总体工作流

本文档只描述整体工作流，不展开实现细节。

## 核心路径

`apitrace` 的主链路分成三段：

1. `capture`：从目标 Windows 程序采集 D3D 调用
2. `trace`：把调用流、对象关系、资源内容和同步边界写入 trace 容器
3. `retrace`：读取 trace，在目标后端或转译层上重放

这三段是同一个产品链路，但职责必须分离。

另外，针对转译层 debug，还会有一条辅助链路：

- `metal trace`：记录转译后的 Metal 调用
- `metal retrace`：回放这条辅助 trace

这条辅助链路不能替代主 D3D trace。
这条辅助链路与主 D3D trace 的链接关系也不由 `apitrace` 定义。

## capture 的职责

- 进入目标进程
- 拦截 D3D11 / D3D12 调用
- 记录参数、返回值、错误码和调用顺序
- 在必要时保存资源内容
- 输出 `.trace`

capture 只负责“录”，不负责“放”。

## trace 的职责

- 保留原始 D3D 调用语义
- 维护对象 id、资源 id 和依赖关系
- 分离调用流与资源 blob
- 表达 frame、submit、present、barrier、fence 等边界

磁盘上的主格式就是 D3D trace，不是对外公开的通用 IR。

## retrace 的职责

- 读取 trace 容器
- 恢复对象生命周期
- 恢复资源和状态对象
- 按原始调用顺序回放 D3D 语义
- 将同一份 trace 送给不同后端或转译层

retrace 的核心实现应当放在库层，命令行工具只是薄封装。

## Metal trace / retrace 的职责

- 由转译层显式调用
- 记录转译后的 Metal 调用和边界
- 原样记录转译层附带的关联载荷，但不解释其语义
- 为转译层后端调试提供独立的 native replay 路径

## 转译层辅助链接 API 的职责

转译层如果需要把 D3D 侧记录和 Metal 侧记录对应起来，不应直接操作 trace bundle 文件。

`apitrace` 应提供一层库 API 来做这件事：

- 接收转译层提交的辅助链接记录
- 把这些记录写入 bundle 的可读 sideband 流
- 对 bundle 目录布局和文件命名细节做封装

这层 API 只负责“写进去”，不负责“看懂是什么意思”。

对于 DXMT 这类已有延迟编码架构的转译层，这层 API 还应尽量贴近现有植入边界，例如：

- command chunk / command buffer 提交边界
- render / compute / blit encoder 切换边界
- marker / annotation 注入点

## 执行形态

- Windows 回放入口：`retrace.exe`
- macOS 回放入口：`retrace`
- Wine 回放路径：直接用 Wine 启动 `retrace.exe`
- native 回放路径：直接运行 macOS 下的 `retrace`

两个可执行文件都应复用同一套 replay library。

## 关联文档

- 项目定位与原则见 [OVERVIEW.md](OVERVIEW.md)
- 分层和数据模型见 [ARCHITECTURE.md](ARCHITECTURE.md)
- capture 入口策略见 [CAPTURE.md](CAPTURE.md)
- retrace 设计约束见 [RETRACE.md](RETRACE.md)
- 当前功能和验证方式见 [TECHNICAL_DETAILS.md](TECHNICAL_DETAILS.md)
