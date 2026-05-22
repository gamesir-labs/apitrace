# 架构与数据模型

## 总体分层

建议把项目拆成五层。

### 1. Capture Frontend

职责：

- 在目标进程里拦截 D3D11 / D3D12 调用
- 采集参数、资源引用、返回对象和错误码
- 在必要时捕获资源实际内容
- 记录帧边界、提交边界、同步点和调试标记

Capture Frontend 不负责格式定义，也不负责后端回放。

### 2. Trace Core

职责：

- 定义 trace 文件容器
- 定义事件记录格式
- 定义资源 blob 格式
- 管理版本兼容
- 管理对象 id、资源 id、依赖关系和引用计数
- 为 replay 提供调度信息
- 为转译层提供辅助 sideband 写入 API，而不是让它直接操作 bundle 文件

### 3. Replay Core

职责：

- 按 trace 顺序恢复对象生命周期
- 重新创建资源和管线状态
- 维护 D3D 语义的调用顺序
- 驱动不同后端执行同一份 trace
- 进行校验和差异检测

### 4. Backend Adapters

职责：

- D3D11 replay backend
- D3D12 replay backend
- Metal trace backend
- Metal retrace backend
- 后续可扩展的其他 API backend

每个 backend 只关心如何把同一份 trace 映射成自己的 API 调用。

### 5. Translation-Layer Bridge

职责：

- 被转译层显式调用
- 标记 frame / command buffer 边界
- 标记 render / compute / blit encoder 边界
- 输出转译后的 Metal trace 记录
- 透传转译层自定义的关联载荷
- 在 D3D -> Metal 路径上输出额外调试信息

这层不是独立 trace 格式，只是一个 runtime bridge。

这里需要明确：

- 主 trace 语义仍然是 D3D
- Metal trace 是辅助调试流
- Metal retrace 回放的是辅助调试流，而不是主 D3D callstream
- 两条流之间的链接语义由转译层自己定义，不由 apitrace core 定义
- facade API 应优先贴近 DXMT 这类现有转译层的 command-buffer 和 encoder 生命周期，而不是逼调用方理解 bundle 落盘细节

## trace 数据模型

建议 trace 文件至少分成四类内容。

### 调用流

记录顺序执行的 API 调用事件。

每个事件应至少包含：

- API 类型
- 函数名或调用编号
- 参数
- 返回值
- 错误码
- 调用时序位置

### 对象图

记录创建出来的对象及其关系：

- device
- context / command queue
- resource
- view
- shader
- pipeline state
- descriptor / heap / layout / signature

对象图不是为了展示，而是为了 retrace 时重建语义。

### 资源 blob

记录真正需要保存的资源内容：

- texture 初始数据
- buffer 初始数据
- shader bytecode
- pipeline 相关的可序列化数据
- 必要的元数据和布局信息

资源 blob 建议和调用流分离，避免 trace 文件膨胀后不好管理。

### 同步和边界

至少要能表达：

- frame boundary
- command list / command buffer boundary
- flush / submit / present
- fence / event / barrier
- 关键调试标记

这些信息直接影响 replay 的可复现性。

## 目录和模块建议

建议当前仓库维持下面这种方向：

```text
src/api/               umbrella API and shared public entry headers
src/trace/             trace types and bundle layout
src/capture/           capture runtime and trace session orchestration
src/retrace/           replay session orchestration
src/d3d11/             D3D11-specific proxy surface
src/d3d12/             D3D12-specific proxy surface
src/metal/             Metal trace / retrace bridge for translation-layer debugging
src/tools/             command line tools
scripts/               build helpers
docs/                  design notes and format docs
```

对应职责：

- `src/<module>/include/` 放该模块的对外接口
- `src/<module>/src/` 放该模块的实现
- `src/tools/` 放测试和调试命令行

对应的模块说明可直接参考源码目录内的 README：

- [src/api/README.md](../src/api/README.md)
- [src/trace/README.md](../src/trace/README.md)
- [src/capture/README.md](../src/capture/README.md)
- [src/retrace/README.md](../src/retrace/README.md)
- [src/d3d11/README.md](../src/d3d11/README.md)
- [src/d3d12/README.md](../src/d3d12/README.md)
- [src/metal/README.md](../src/metal/README.md)
- [src/tools/README.md](../src/tools/README.md)
- [src/tools/retrace/README.md](../src/tools/retrace/README.md)

## 构建系统

### 语言

- C++

### 构建入口

- CMake

当前只保留 CMake，原因是：

- 现阶段项目规模还不需要维护两套构建入口
- CMake 已足够覆盖 IDE、CI 和本地开发场景
- 可以把维护成本集中到一套目标定义和一套构建脚本

### 构建目标

- core library
- CLI / helper tool
- Windows proxy DLLs
- Metal backend library
