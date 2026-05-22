# Capture 设计

## Windows 入口策略

Windows 侧建议把入口分成四个模式，再统一进同一个 runtime。

### proxy DLL

适用场景：

- 你自己控制程序启动
- 需要快速验证 DLL override 是否可行
- 需要简单的 app-local 替换

特点：

- 最容易理解
- 最容易做小范围测试
- 对启动链路要求最高

限制：

- 只适合能控制 DLL 搜索顺序的场景
- 对复杂 launcher / 子进程链不够稳

### launcher / injector

适用场景：

- 指定某个 exe 进行 capture
- 需要比 proxy DLL 更稳定的入口控制
- 需要支持附加运行参数、工作目录和环境变量

特点：

- 由外部 launcher 启动或附加目标进程
- 目标进程里加载 bootstrap runtime
- 是主 capture 路径的候选实现

### attach

适用场景：

- 目标进程已经启动
- 开发者想临时接管当前运行中的程序

特点：

- 对调试最方便
- 不依赖程序自身启动路径
- 需要更清晰的进程注入和权限处理

### child 进程传播

适用场景：

- 启动器和真实渲染进程不是同一个 exe
- Steam / launcher / 游戏壳会再拉起子进程

特点：

- 可以让 capture 跟着子进程走
- 对复杂启动链是必需项

### LoadLibrary hook 补充机制

这不是独立入口，而是进程内 runtime 的补充能力。

用途：

- 捕获新加载的模块
- 继续给动态加载的 D3D / DXGI / 其他相关模块补 hook
- 避免只在进程启动时 hook 一次就结束

## 入口选择策略

建议默认按下面顺序考虑：

1. `attach`，适合已运行进程
2. `launcher / injector`，适合指定 exe
3. `child`，适合 launcher 链路
4. `proxy DLL`，适合自己控制的测试程序

这不是互斥关系，可以在同一个产品里并存。

## 进程内 runtime 的职责

一旦进入目标进程，后续逻辑都统一交给 runtime：

- 注册 hook
- 采集 D3D 调用
- 维护对象表
- 处理动态加载模块
- 发出 capture 所需的元信息

## 不建议的方式

- 全局系统级 hook
- `AppInit_DLLs` 之类的老式注入手段
- 让不同入口各写一套 capture 逻辑

这些方式会让调试和维护成本快速失控。

## D3D11 capture 重点

- `D3D11CreateDevice`
- `D3D11CreateDeviceAndSwapChain`
- device / context / swap chain 的包装
- shader、resource、view、sampler、rasterizer state 的记录

### 需要特别注意的点

- immediate context 和 deferred context
- 多线程调用顺序
- 隐式资源状态
- swap chain 与 present 时机
- shader 资源绑定和动态常量

## D3D12 capture 重点

- `D3D12CreateDevice`
- command queue / allocator / list
- root signature / pipeline state
- descriptor heap / descriptor table
- resource barrier
- fence / submit / present

### 需要特别注意的点

- D3D12 更显式，状态更碎
- command list 重放必须保留提交顺序
- barrier 和资源状态必须精确
- descriptor 数据通常要单独持久化

## Metal trace 重点

Metal 侧这里不应再叫 capture frontend。

原因是：

- 它看不到原始 D3D 调用入口
- 它只能看到转译层已经发出的 Metal 调用
- 如果缺少转译层自己维护的关联信息，单靠 Metal API 无法恢复原始 D3D 语义

因此 Metal 侧更准确的定位是“辅助 trace backend”。

它至少要能记录：

- 转译后的 Metal 调用名
- frame / command buffer / encoder 边界
- 转译层选择附带的任意关联载荷

### 实现位置约束

- 具体 trace 注入点必须放在转译层侧配合实现
- `apitrace` 侧负责接口、记录结构和落盘边界
- 不要假设 `apitrace` 或 Metal runtime 自己能补全这些关联信息

### 植入边界建议

参考 DXMT 这类已有延迟编码架构的转译层，优先考虑下面三类植入点：

- command chunk / command buffer 提交边界
- render / compute / blit encoder 开始与结束边界
- 现有 marker / annotation API，例如 `BeginEvent` / `EndEvent` / `SetMarker`

这样可以尽量复用转译层现有的命令组织方式，而不是强迫它直接感知 trace bundle 的落盘细节。
