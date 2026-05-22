# d3d11

## 作用

`src/d3d11/` 负责 D3D11 相关的模块表面，目前已经包含 Wine app-local override 入口和最小 capture MVP。

## 负责的内容

- D3D11 proxy 描述信息
- D3D11 proxy 转发和下游 DLL 解析
- D3D11 最小 capture hook 和对象包装
- D3D11 replay backend 骨架
- D3D11 对象状态注册表骨架

## 不负责的内容

- 不负责通用 capture runtime
- 不负责 trace bundle 读写
- 不负责 D3D12 或 Metal 逻辑

## 当前实现边界

- `d3d11_proxy.hpp` / `d3d11_proxy.cpp`
- `d3d11_capture.hpp` / `d3d11_capture.cpp`
- `d3d11_capture_internal.hpp` / `d3d11_capture_internal.cpp`
- `d3d11_replay.hpp` / `d3d11_replay.cpp`
- `d3d11_state.hpp` / `d3d11_state.cpp`

当前 MVP 只保证 `test/src/d3d11_triangle.cpp` 经过的最小 D3D11 路径：

- 入口：
  `D3D11CreateDevice` / `D3D11CoreCreateDevice` / `D3D11CreateDeviceAndSwapChain` / `D3D11On12CreateDevice`
- 返回对象：
  `ID3D11Device` / `ID3D11DeviceContext` / `IDXGISwapChain`
- 已记录调用：
  `CreateBuffer`
  `CreateRenderTargetView`
  `CreateInputLayout`
  `CreateVertexShader`
  `CreatePixelShader`
  `GetImmediateContext`
  `Map` / `Unmap`
  `VSSetShader` / `PSSetShader`
  `VSSetConstantBuffers` / `PSSetConstantBuffers`
  `IASetInputLayout`
  `IASetVertexBuffers`
  `IASetPrimitiveTopology`
  `OMSetRenderTargets`
  `RSSetViewports`
  `ClearRenderTargetView`
  `Draw`
  `Present`
  `GetBuffer`

## 当前未覆盖项

这个 MVP 还不是完整 D3D11 capture。当前明确未做：

- deferred context
- 更复杂的 DXGI 工厂 / 适配器枚举路径
- 多线程顺序一致性
- 完整资源快照和去重策略
- DXGI 独立 override
- retrace 兼容性和回放正确性

## 头文件与实现导航

如果你要读 `src/d3d11/`，建议按下面顺序进入：

1. `d3d11_proxy.hpp`
   - 看 D3D11 proxy surface 的最小入口描述
2. `d3d11_capture.hpp`
   - 看 capture 侧想拦哪些 D3D11 surface
3. `d3d11_state.hpp`
   - 看 replay / capture 共用的对象注册表骨架
4. `d3d11_replay.hpp`
   - 看 D3D11 replay backend 的最小生命周期

如果你要改代码，建议先判断是改：

- “入口表面”：
  从 `d3d11_proxy.*` 入手
- “capture hook”：
  从 `d3d11_capture.*` 入手
- “状态对象”：
  从 `d3d11_state.*` 入手
- “replay backend”：
  从 `d3d11_replay.*` 入手

## 当前阶段说明

- 这里已经从单一 proxy 文件扩成了 capture / replay / state 三条骨架
- 后续如果增加 D3D11 专属 hook、对象包装或回放适配，应继续在本模块内部细拆，而不是回流到通用模块

## 相关文档

- [总架构文档](../../docs/ARCHITECTURE.md)
- [capture 设计文档](../../docs/CAPTURE.md)
- [retrace 设计文档](../../docs/RETRACE.md)
