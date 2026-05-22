# 实施计划

## 阶段划分

### 阶段 1：工程骨架

- 目录结构固定
- 头文件和核心库骨架定下来
- CMake 能稳定构建

### 阶段 2：D3D11 最小 capture

- 只做最少的创建和提交路径
- 能记录调用流
- 能输出一个可读 trace

### 阶段 3：资源持久化

- texture / buffer / shader 保存
- 对象表和依赖关系补齐

### 阶段 4：D3D12 capture

- root signature / descriptor / barrier / fence 补齐

### 阶段 5：retrace

- 先实现原生 D3D replay
- 再接转译层 replay

### 阶段 6：Metal trace / retrace bridge

- 让转译层显式调用 Metal trace API
- 输出可附带转译层自定义关联载荷的转译后 Metal trace
- 提供对应的 Metal retrace 调试路径

## 主要风险

### 资源爆炸

纹理和缓冲区很快会把 trace 文件撑大。

需要：

- blob 分离
- 压缩
- 去重
- 按需保存

### 动态加载

很多程序不会在启动时一次性加载完所有图形相关模块。

需要：

- LoadLibrary hook
- 重新扫描新模块
- 继续补 hook

### 子进程链

launcher 和实际渲染进程可能不是一个进程。

需要：

- child propagation
- 目标进程路径匹配
- 启动链跟踪

## 当前推进建议

如果目标是尽快拿到第一条可验证链路，建议按下面顺序推进：

1. 固定目录和构建骨架
2. 先打通 D3D11 最小 capture
3. 补资源持久化
4. 做最小 retrace CLI
5. 再扩展 D3D12 和 Metal trace / retrace bridge
