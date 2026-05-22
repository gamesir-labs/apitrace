# 项目定位与设计原则

## 项目定位

这个项目不是简单的 API logger，而是一个面向图形转译层研发的调试工具。

核心目标是：

- 在 Windows 上捕获 D3D11 / D3D12 调用
- 保存调用序列、对象生命周期、资源内容和必要的同步信息
- 在另一台 Windows 机器，或在 macOS / Linux 的转译层上重放同一份 D3D trace
- 在 macOS 上配合转译层输出 Metal trace，把 D3D -> Metal 的转译后调用过程也记录下来
- 未来可以扩展到其他图形 API，但当前设计先围绕 D3D 和 Metal

它的主要使用者是开发者，而不是终端用户。典型场景包括：

- 复现转译层渲染错误
- 对比原始 API 和后端 API 的行为差异
- 定位资源绑定、状态机、同步和命令提交问题
- 辅助做兼容性验证和回归测试

## 设计原则

### 主 trace 语义必须保留原始 API

对 D3D trace 来说，磁盘上的主格式就是原始 D3D 调用流。

retrace 时，输入给转译层的仍然是原样的 D3D 调用，而不是一个先抽象再还原的通用 IR。

### IR 只作为内部辅助层

IR 的边界只限制在：

- 资源组织
- 依赖分析
- 调度
- 回放辅助

它不承担 retrace 的语义还原，也不作为对外公开的主 trace 格式。

### 入口方式可以多种组合，但核心运行时只有一套

Windows capture 不只依赖单一 DLL override。可以并存多种入口：

- proxy DLL
- launcher / injector
- attach 到已运行进程
- child 进程传播
- 进程内 LoadLibrary hook 补充机制

这些入口最终都要进入同一套进程内 runtime 和 hook 注册逻辑，避免实现分叉。

### capture 和 replay 必须解耦

录制侧负责采集，回放侧负责重建。

不要让 capture 代码直接承担 replay 逻辑，也不要让 replay 代码假设自己一定来自某个特定的 capture 入口。

### 构建和平台适配要分层

源码层保持 C++ 为主，平台细节通过适配层隔离。

构建系统统一采用 CMake，方便：

- 现有开发环境快速接入
- 未来 CI 和 IDE 兼容
- 保持目录结构和产物命名稳定
- 降低维护两套构建脚本的额外成本

## 目标

- 记录 D3D11 / D3D12 调用
- 记录资源内容
- 记录对象关系和生命周期
- 记录必要的同步和提交边界
- 回放到原生 D3D 后端
- 回放到转译层后端
- 在转译层调试或回放过程中启用 Metal trace / retrace

## Metal 调试链路的额外原则

Metal 侧的 trace 和 retrace 是为了调试转译层，不是为了替代主 D3D trace。

这意味着：

- 主 trace 仍然保存原始 D3D 调用语义
- Metal trace 只记录转译后的 Metal 调用与边界
- D3D trace 与 Metal trace 的链接语义由转译层自己定义
- 不能指望单靠 Metal API 反推出原始 D3D 调用

## 当前非目标

- 不做一个通用图形 IR 作为唯一存档格式
- 不优先支持全局系统级 hook
- 不优先支持给原生 Metal 应用提供自己的替代 capture 入口
- 不追求一开始就覆盖所有 API
- 不追求一开始就和 RenderDoc 完全同构
