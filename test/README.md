# test modules

`test/` 存放可执行 demo、scene matrix、运行时封装和模块级验证脚本。当前测试入口按后端拆成四个独立模块：

- `scripts/test-d3d11.sh`
- `scripts/test-d3d12.sh`
- `scripts/test-metal.sh`
- `scripts/test-d3d-metal-link.sh`

四个脚本都从源码构建需要的 demo / retrace 产物，并把输出写入 `test/artifacts/`。旧的 `validate-*.sh`、`compare-*.py` 和 `test/fixtures/retrace/` 静态像素 fixture 不再作为测试入口或验收基准。

## Demo 契约

测试 demo 是零参数程序：

- Windows D3D11：`apitrace_test_d3d11.exe`
- Windows D3D12：`apitrace_test_d3d12.exe`
- macOS Metal：`apitrace_test_metal`

每个 demo 启动后只创建一个窗口，按固定 scene 顺序运行该后端的全部默认 scene，完成后自动退出。demo 不再提供 `--dx`、`--scene`、`--list-scenes` 或单 scene 选择入口；单 scene 调试应在源码内临时断点或本地调试分支完成，不进入测试契约。

标准输出日志接口固定为：

- `scene start: <name>`
- `scene pass: <name>`
- `scene skip: <name> reason=<text>`
- `scene fail: <name> reason=<text>`
- `summary: passed=X failed=Y skipped=Z`

退出码：

- `0`：本次默认 scene 全部通过或按能力显式 skip。
- 非零：初始化失败、scene 校验失败、trace / retrace / compare 失败。

`APITRACE_TRIANGLE_MAX_FRAMES` 仍表示每个 scene 的帧预算。默认 all-scenes 流程按“每个场景各自拥有同样帧预算”解释。

## 统一场景矩阵

`dx11` 和 `dx12` 共用同一份场景矩阵，源码入口是：

- `test/src/scenes/shared/include/scenes/shared/scene_matrix.hpp`
- `test/src/scenes/shared/src/scene_matrix.cpp`

矩阵顺序就是默认 demo 运行顺序。临时排除项保留在矩阵内用于文档和能力探测，但不参与默认 D3D12 all-scenes 流程。

| scene | tier | 目标 API 覆盖断言 | 目标视觉断言 | dx11 | dx12 |
| --- | --- | --- | --- | --- | --- |
| `smoke_triangle` | core | `swapchain` / `backbuffer` / `RTV` / `viewport` / `Draw` / `Present` | 清屏背景上出现居中的着色三角形，角落保留 clear color | 已实现 | 已实现 |
| `indexed_instancing` | core | `vertex buffer` / `index buffer` / `instance buffer` / `DrawIndexed` / `DrawIndexedInstanced` | 多个 indexed quad 以不同 instance 偏移和稳定颜色落位 | 已实现 | 已实现 |
| `textured_quad` | core | `Texture2D` / `ShaderResourceView` / `SamplerState` / `UpdateSubresource` / shader resource binding | 上传后的纹理在最终 quad 上被稳定采样并保留 tint 结果 | 已实现 | 已实现 |
| `depth_blend_scissor` | core | depth texture / `DSV` / depth state / blend state / rasterizer state / scissor | 终帧同时可见 depth、alpha blend、scissor 排除区 | 已实现 | 已实现 |
| `offscreen_copy_composite` | core | offscreen `RT` / `CopyResource` / `SRV`-`RTV` 复用 / 二次合成 | 离屏内容被复制并回合成到 swapchain，颜色分层保持稳定 | 已实现 | 已实现 |
| `mip_sampling` | extended | mip chain / mip sampling / mip view and sampling path | 不同区域命中不同 mip 级别，并呈现可预测的颜色过渡 | 已实现 | 已实现 |
| `msaa_resolve` | extended | `MSAA RT` / resolve / MSAA 到单采样输出链路 | resolve 后边缘保持抗锯齿，覆盖区与背景区稳定 | 已实现 | 已实现 |
| `barrier_state_transitions` | core | `ResourceBarrier` / split barrier / render-target to SRV transition coverage | split barrier keeps two cleared offscreen targets visible as stable left/right colors in the final composite | N/A | 已实现 |
| `descriptor_root_signature_rebind` | core | descriptor heap / root signature / CBV and SRV rebinding | the same pipeline rebinds two SRVs and two tint constants to produce distinct left/right quads | N/A | 已实现 |
| `indirect_draw` | core | `ExecuteIndirect` / draw argument buffer / count buffer | indirect command submission draws two distinct quads with stable left/right colors | N/A | 已实现 |
| `compute_uav_writeback` | core | compute dispatch / UAV writeback / SRV sampling after state transition | compute writes a four-quadrant texture that survives the UAV-to-SRV transition into the final quad | N/A | 已实现 |
| `resource_lifecycle` | core | resource create / release / recreate / descriptor refresh | recreated texture resources keep producing the final solid color after repeated lifetime churn | N/A | 已实现 |
| `dxr_smoke` | extended | raytracing tier probe / AS build-update / SBT / DispatchRays smoke entry | raytracing support is probed explicitly; currently excluded from default D3D12 all-scenes while the D3DMetal translation layer is unstable | N/A | 手动调试保留 |
| `mesh_shader_smoke` | extended | mesh shader tier probe / task-mesh PSO switch / DispatchMesh smoke entry | mesh shader support is probed explicitly and the scene stays skip-safe when the device or compiler path is unavailable | N/A | 已实现 |

默认 D3D12 demo 仍跳过 `dxr_smoke`。`mesh_shader_smoke` 按设备和编译器能力显式 pass 或 skip。

Metal demo 使用独立 scene 集：

- `metal_smoke_triangle`
- `metal_indexed_instancing`
- `metal_textured_quad`
- `metal_compute_uav`
- `metal_indirect_draw`
- `metal_argument_buffer`
- `metal_multi_pass`
- `metal_present_smoke`

## 运行规则

- D3D11、D3D12 和 Metal demo 都必须单窗口顺序复用。同一次运行里只创建一次 runtime 和窗口，不允许为每个 scene 单独重建窗口。
- 场景切换只允许重置必要 GPU 状态；窗口、设备和主执行链路保持连续，用于暴露状态残留问题。
- 每个默认 scene 都必须同时拥有像素断言和 API 覆盖断言。
- 新增 scene 时先让测试红灯，再补实现；不要把未实现路径静默改成 pass。

当前代码行为：

- `dx11`：在一个 `Dx11Runtime` 上顺序执行所有已实现 scene，场景末尾调用 `clear_state()` 清理状态。
- `dx12`：在一个 `Dx12Runtime` 上顺序执行默认 scene，场景末尾调用 `clear_state()` 清理状态；`dxr_smoke` 不进入默认运行。
- `metal`：使用真实 `NSWindow` / `CAMetalLayer`，单窗口跑完全部 Metal scene 后自动关闭。

## PresentFrame 比对

逐帧画面对比统一使用：

```sh
python3 scripts/lib/present_frame_compare.py \
  --api <d3d11|d3d12|metal> \
  --baseline <trace-bundle> \
  --candidate <retrace-bundle> \
  --tile 100 \
  --tile-pixel-threshold 0.95
```

比较规则固定为：

- tile size 与步进都是 `100`。
- tile 不重叠，右边缘和下边缘允许出现小于 `100` 的尾 tile。
- 每个 tile 必须至少 `95%` 像素完全匹配。
- 任一 tile 不达标则该帧失败，所有 tile 必须通过。
- 不引入 per-channel tolerance；`max_channel_delta` 只作为诊断输出。

D3D11 和 D3D12 trace / retrace 都写 `D3D*PresentFrame` debug 资产。Metal trace / retrace 写 `MetalPresentFrame` debug 资产。PresentFrame 只用于测试对比和诊断，不能替代 retrace 的真实重渲染语义。

## 模块脚本

`scripts/test-d3d11.sh`：

- 构建根项目 Windows proxy / retrace 和 D3D11 demo。
- staging DXMT builtin runtime，并显式设置 Wine DLL override。
- 运行零参数 `apitrace_test_d3d11.exe` 生成 trace bundle。
- 用 Windows `retrace.exe` 真实回放该 bundle，并捕获 retrace PresentFrame。
- 用 `present_frame_compare.py --api d3d11 --tile 100 --tile-pixel-threshold 0.95` 比对 trace 与 retrace。

`scripts/test-d3d12.sh`：

- 构建根项目 Windows proxy / retrace 和 D3D12 demo。
- staging DXMT builtin runtime，并显式设置 Wine DLL override。
- 运行零参数 `apitrace_test_d3d12.exe` 生成 trace bundle。
- 确认默认流程没有启动 `dxr_smoke`。
- 用 Windows `retrace.exe` 真实回放该 bundle，并捕获 retrace PresentFrame。
- 用 `present_frame_compare.py --api d3d12 --tile 100 --tile-pixel-threshold 0.95` 比对 trace 与 retrace。

`scripts/test-metal.sh`：

- 构建 macOS native retrace 和 Metal demo。
- 运行零参数 `apitrace_test_metal`，真实窗口跑完全部 Metal scene 并写 trace bundle。
- 用 native `retrace --metal` 回放该 bundle，并捕获 retrace PresentFrame。
- 用 `present_frame_compare.py --api metal --tile 100 --tile-pixel-threshold 0.95` 比对 trace 与 retrace。

`scripts/test-d3d-metal-link.sh`：

- 单独生成 link test 专用 D3D12-to-Metal trace bundle。
- 校验 `analysis/translation-links.jsonl` 中每个非零 `d3d_sequence` 至少有 `encoder` 和 `draw_to_metal_calls` scope。
- 用 native `retrace --metal` 回放该 bundle，并捕获 Metal PresentFrame。
- 对比 D3D12 trace PresentFrame 和 Metal retrace PresentFrame，tile 规则同上。

`scripts/test-cross-api-smoke.sh`：

- 生成合成 D3D12 + Metal 双侧 bundle。
- 用 `bundle-check` 只校验 `checksums.json` 列出的文件 hash 和 manifest `bundle_hash`。
- 由 `apitrace_test_cross_api_bundle_closure`、D3D validate-only 和 Metal validate-only 校验双侧 replay closure 与 translation link 语义。
- 分别执行 D3D validate-only 与 Metal validate-only。
- 对同一 bundle 内的 D3D12 PresentFrame 和 Metal PresentFrame 执行 `d3d12-to-metal` tile compare。
- 构建并运行 `apitrace_test_metal_native_replay_smoke`。该 smoke 使用真实 `metallib`、离屏 render target
  和非 validate-only `ReplaySession` 跑 native Metal retrace，再把 retrace 捕获帧与原生 Metal 直接
  渲染出的基准帧比较；随后用相同 Metal 命令流但错误 `MetalPresentFrame` 的 poisoned trace 再跑一次，
  要求 retrace 捕获帧仍等于真实基准且不等于 poisoned 记录帧，防止 retrace 退化成直接播放记录帧。
  如果当前环境没有 Metal device，测试以 77 跳过。

`scripts/test-d3d12-native-smoke.sh`：

- 优先构建并运行 macOS native `apitrace_d3d12_native_asset_dump`，通过 D3DMetal bundled
  `libdxcompiler.dylib` 生成真实 fullscreen-triangle VS/PS DXIL 和 serialized root signature
  资产；如果 native DXC 输入不可用，再回退到 Wine `apitrace_d3d12_asset_dump.exe` +
  `d3d12.dll` + `d3dcompiler_47.dll` 生成 DXBC 资产。
- 生成最小但 pipeline-dependent 的 D3D12 bundle：swapchain back buffer、RTV、root signature、
  graphics PSO、viewport/scissor、OMSetRenderTargets、DrawInstanced、state transition、Present。
- 用 `bundle-check` 只校验 bundle 文件完整性；D3D pipeline / shader / root-signature 资产和 draw
  元数据闭包由 native `retrace --validate-only` 和 D3D replay 语义测试负责。
- 在 DXMT ABI probe 前先运行 ARM64 Metal native probe 并保存诊断日志；该 probe 只用于区分当前
  shell/GUI 环境下的 Metal 可用性，不再作为 x86_64 DXMT native replay 的硬性 gate。
- 先运行 native DXMT ABI smoke，确认当前进程能枚举 Metal-backed DXGI adapter。
- 用 native macOS `retrace` 真实回放 D3D12 bundle，并捕获 retrace PresentFrame。
- 用 `present_frame_compare.py --api d3d12 --tile 4 --tile-pixel-threshold 1.0` 比对 trace 与 native retrace。
- 同时生成 poisoned-baseline bundle：D3D12 命令流完全相同，但输入 `D3D12PresentFrame` 故意写成错误颜色。
  native retrace 的捕获帧必须和这个 poisoned baseline 不匹配，用来防止 retrace 退化成直接播放记录帧。
- 如果当前进程没有 Metal-backed DXGI adapter，测试以 77 跳过；设置
  `APITRACE_REQUIRE_D3D_NATIVE_REPLAY=1` 可把该 skip 提升为失败。

## 构建入口

推荐直接运行模块脚本。需要手动构建 demo 时可使用：

```sh
cmake -S test -B test/build/windows-x86_64-d3d11 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=test/toolchains/windows-x86_64-mingw.cmake
cmake --build test/build/windows-x86_64-d3d11
cmake --install test/build/windows-x86_64-d3d11 --prefix test/artifacts/d3d11
```

Metal demo 构建：

```sh
cmake -S test -B test/build/macos-native-arm64 -G Ninja \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DAPITRACE_BUILD_METAL_DEMO=ON \
  -DAPITRACE_ROOT_BUILD_DIR=build/cmake-metal-arm64
cmake --build test/build/macos-native-arm64
cmake --install test/build/macos-native-arm64 --prefix test/artifacts/metal
```
