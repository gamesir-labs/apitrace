# Retrace 设计

## 核心语义

retrace 的核心要求是：

- 输入仍是原始 D3D 调用
- 输出行为要尽量接近捕获时的时序和资源状态
- replay 不改变 API 语义层级

retrace 不是把原始 D3D 调用先改写成统一 IR，再从 IR 倒推回放。

## retrace 过程

1. 读取 trace 容器
2. 建立对象表
3. 恢复资源 blob
4. 按调用顺序重放 D3D 调用
5. 将同一份调用流送给目标后端
6. 做必要的校验和状态对比

## 为什么不把 replay 建成统一 IR 还原

我们的目标不是把 API 变成一个抽象命令集再倒回来，而是保留原始 D3D 语义。

IR 只能在内部帮助：

- 组织资源
- 安排回放
- 做差异分析
- 做后端转换前的中间准备

它不能替代原始调用流。

## 库层实现约束

- retrace 的核心逻辑应当放在 library 或 runtime 层
- 平台可执行文件只负责参数解析、进程启动和结果返回
- 不允许在 `retrace.exe` 和 `retrace` 之间各自维护一套独立 replay 实现

## 可执行入口

用户侧保留两个命令行入口：

- Windows：`retrace.exe`
- macOS：`retrace`

两者都应复用同一套 replay library。

## Wine 与 native 的回放路径

- Wine 回放：直接用 Wine 启动 `retrace.exe`
- native 回放：直接运行 macOS 下的 `retrace`

这两条路径是不同的进程入口，但不应分叉底层 replay 逻辑。

## CLI 契约

命令行工具只做一件事：重放 trace。

因此它应保持最小契约：

- 输入一个 trace 文件
- 执行 replay
- 输出成功、失败和必要的诊断信息

不要把 capture、convert 或其他无关调试能力继续塞进这个 CLI。

## D3D11 replay 重点

- 先恢复 device/context
- 再恢复资源和状态对象
- 最后按事件流提交 `draw` / `dispatch` / `present`

## D3D12 replay 重点

- 恢复设备和 queue
- 恢复 PSO、root signature、descriptor 绑定关系
- 复原每一段 command list 的边界
- 尽量保持和捕获时一致的提交节奏

## 与转译层的关系

- replay 应当能够把记录下来的 D3D 调用直接送入转译层
- native replay 很重要，因为转译层可能并不需要整条链路都经过 Wine
- 如果仍然需要 Wine-hosted replay，也应复用同一套 replay library 行为

## Metal trace / retrace 集成

Metal 侧需要支持独立的 trace 和 retrace，但这条链路是给转译层 debug 用的。

它不走 Windows override，也不替代主 D3D retrace。

它的基本事实是：

- Metal trace 看不到原始 D3D 调用入口
- Metal trace 只能看到转译后的 Metal 调用
- 如果想把两边对齐，必须由转译层自己提供并消费关联信息

因此这里应有两条相关能力：

- `Metal trace`：
  由转译层显式调用 library API，记录转译后的 Metal 调用和边界
- `Metal retrace`：
  重放上述 Metal trace，用于后端级调试和对比

### 需要由转译层自己负责的信息

- D3D trace 与 Metal trace 的链接策略
- 关联字段的 schema
- 关联字段的生产与消费逻辑

`apitrace` 只负责把这些关联载荷原样记录下来，不负责解释或校验它们。

### 与主 retrace 的关系

- 主 retrace 仍然消费原始 D3D callstream
- Metal retrace 消费的是转译后的辅助 trace
- 这两条路径应可对照，但不要混成同一种存档语义
