# trace demo

这里放最小可运行的 Windows trace demo。

目标产物：

- `apitrace_test_demo.exe`

推荐构建：

```sh
cmake -S test -B test/build/windows-x86_64 -DCMAKE_TOOLCHAIN_FILE=test/toolchains/windows-x86_64-mingw.cmake
cmake --build test/build/windows-x86_64
cmake --install test/build/windows-x86_64 --prefix test/artifacts/windows-x86_64/demo
```

运行时可用 Wine 直接启动：

```sh
wine apitrace_test_demo.exe --dx dx11 --scene smoke_triangle
wine apitrace_test_demo.exe --dx dx11 --scene all
wine apitrace_test_demo.exe --list-scenes --dx dx11
wine apitrace_test_demo.exe --dx dx12 --scene smoke_triangle
wine apitrace_test_demo.exe --list-scenes --dx dx12
```

如果需要配合 apitrace 的 Windows proxy DLL，再把根项目的 Windows cross-build 安装到同一个测试目录即可。

## 统一场景矩阵

`dx11` 和 `dx12` 共用同一份场景矩阵，源码入口是：

- `test/src/scenes/shared/include/scenes/shared/scene_matrix.hpp`
- `test/src/scenes/shared/src/scene_matrix.cpp`

矩阵顺序就是 `--scene all` 的顺序。这个顺序必须在 `dx11` / `dx12` 间保持一致，后续新增实现也不应调整名称或顺位。

| scene | tier | 目标 API 覆盖断言 | 目标视觉断言 | dx11 | dx12 |
| --- | --- | --- | --- | --- | --- |
| `smoke_triangle` | core | `swapchain` / `backbuffer` / `RTV` / `viewport` / `Draw` / `Present` | 清屏背景上出现居中的着色三角形，角落保留 clear color | 已实现 | 已实现 |
| `indexed_instancing` | core | `vertex buffer` / `index buffer` / `instance buffer` / `DrawIndexed` / `DrawIndexedInstanced` | 多个 indexed quad 以不同 instance 偏移和稳定颜色落位 | 已实现 | 已实现 |
| `textured_quad` | core | `Texture2D` / `ShaderResourceView` / `SamplerState` / `UpdateSubresource` / shader resource binding | 上传后的纹理在最终 quad 上被稳定采样并保留 tint 结果 | 已实现 | 已实现 |
| `depth_blend_scissor` | core | depth texture / `DSV` / depth state / blend state / rasterizer state / scissor | 终帧同时可见 depth、alpha blend、scissor 排除区 | 已实现 | 已实现 |
| `offscreen_copy_composite` | core | offscreen `RT` / `CopyResource` / `SRV`-`RTV` 复用 / 二次合成 | 离屏内容被复制并回合成到 swapchain，颜色分层保持稳定 | 已实现 | 已实现 |
| `mip_sampling` | extended | mip chain / mip sampling / mip view and sampling path | 不同区域命中不同 mip 级别，并呈现可预测的颜色过渡 | 已实现 | 已实现 |
| `msaa_resolve` | extended | `MSAA RT` / resolve / MSAA 到单采样输出链路 | resolve 后边缘保持抗锯齿，覆盖区与背景区稳定 | 已实现 | 已实现 |

## CLI 契约

固定入口参数：

- `--dx <dx11|dx12>`：选择当前 scene 后端。
- `--scene <name|all>`：单场景调试，或按统一矩阵顺序顺跑全部 scene。
- `--list-scenes`：列出指定 `dx mode` 下注册的 scene 名称。

保留环境变量：

- `APITRACE_TRIANGLE_MAX_FRAMES`

语义：

- 单场景运行时，它是该场景的帧预算。
- `--scene all` 时，按“每个场景各自拥有同样帧预算”解释。

标准输出日志接口固定为：

- `dx mode: <mode>`
- `scene start: <name>`
- `scene pass: <name>`
- `scene fail: <name> reason=<text>`
- `summary: passed=X failed=Y skipped=Z`

退出码：

- `0`：本次运行涉及的 scene 全部通过。
- 非零：参数错误、占位场景失败、scene 校验失败，或运行时初始化失败。

## 场景运行规则

- `--scene all` 必须单窗口顺序复用。同一次运行里只创建一次 runtime 和窗口，不允许为每个 scene 单独重建窗口。
- 场景切换只允许重置必要 GPU 状态；窗口、设备和主执行链路应保持连续，便于压出状态残留问题。
- 每个 scene 都必须同时拥有像素断言和 API 覆盖断言。
- 新增 scene 时先让测试红灯，再补实现，不要先把占位失败静默改成跳过。

当前代码行为：

- `dx11`：在一个 `Dx11Runtime` 上顺序执行所有已实现 scene，场景末尾调用 `clear_state()` 清理状态。
- `dx12`：在一个 `Dx12Runtime` 上顺序执行全部 7 个 scene，场景末尾调用 `clear_state()` 清理状态。

## DX11 现状

当前已实现的 `dx11` scene：

1. `smoke_triangle`
2. `indexed_instancing`
3. `textured_quad`
4. `depth_blend_scissor`
5. `offscreen_copy_composite`
6. `mip_sampling`
7. `msaa_resolve`

这 7 个 scene 都不是静态单帧图，而是“前段动画、后段收敛到稳定终帧”的确定性序列：

- 顺序回放时可直接观察 draw 顺序、状态切换、离屏 copy 与最终 present 的连续效果。
- visual fixture 仍可稳定截到一致终帧，不会因为截图时机不同而频繁漂移。

## DX12 现状

当前 `dx12` 已有：

- scene 注册
- `--list-scenes`
- 单场景执行入口
- `--scene all` 单窗口顺序执行骨架
- 真实 runtime：`device` / `queue` / `allocator` / `command list` / `swapchain` / `fence` / backbuffer readback
- 已实装 scene：`smoke_triangle`、`indexed_instancing`、`textured_quad`、`depth_blend_scissor`、`offscreen_copy_composite`、`mip_sampling`、`msaa_resolve`
- 与 `dx11` 对齐的像素断言和 API 覆盖断言
- `CopyResource`、mip upload 与采样、MSAA resolve、depth/blend/scissor 的实装路径

最小验证脚本预留为：

- `scripts/validate-d3d12-wine.sh`

## 资产与 trace bundle

测试 demo 会从外部打包资产目录读取纹理和几何输入，再通过 D3D API 上传到 GPU。

capture 的职责不是在 replay 时重新访问这些原始源文件，而是把进入 D3D 的资源内容与元数据完整记录到 trace bundle。因此 replay 只依赖 bundle 内的 `textures/`、`buffers/`、`shaders/`、`pipelines/` 等资产，不依赖原始应用安装目录。

## D3D11 MVP 验收

仓库内的 D3D11 MVP 验收脚本：

```sh
scripts/validate-d3d11-wine.sh
```

它会：

- 构建 apitrace 的 Windows `d3d11.dll`
- 构建 `apitrace_test_demo.exe`
- 把 proxy DLL 和 DXMT 运行时放到同一目录
- 在 Wine 下启动 `--dx dx11 --scene smoke_triangle`
- 校验 trace bundle 内的 `callstream.jsonl` / `checksums.json` / `objects.json` / `pipelines/`
- 校验 shader / buffer 资产引用存在且非空

在 macOS 桌面环境下，脚本默认还会：

- 用 Wine 虚拟桌面承载 demo，避免前台窗口被其他应用遮挡
- 生成 `test/artifacts/windows-x86_64/demo/bin/triangle-d3d11-visual.png`
- 对截图做最小像素检查，确认窗口里出现了彩色三角形，而不是纯黑或空白窗口

## retrace fixture

当前 retrace fixture 按 scene 拆分放在：

- `test/fixtures/retrace/d3d11-smoke_triangle/`
- `test/fixtures/retrace/d3d11-indexed_instancing/`
- `test/fixtures/retrace/d3d11-textured_quad/`
- `test/fixtures/retrace/d3d11-depth_blend_scissor/`
- `test/fixtures/retrace/d3d11-offscreen_copy_composite/`
- `test/fixtures/retrace/d3d11-mip_sampling/`
- `test/fixtures/retrace/d3d11-msaa_resolve/`

兼容保留的早期 smoke fixture：

- `test/fixtures/retrace/d3d11-triangle/`

刷新 fixture 时：

- 先重新跑 `scripts/validate-d3d11-wine.sh`，把 `test/artifacts/windows-x86_64/demo/bin/dx11-core-scene-traces/` 下对应 scene 的 bundle 同步到 fixture 目录。
- 如果需要刷新 retrace visual reference，再显式执行 `APITRACE_VISUAL_CHECK=1 APITRACE_ACCEPT_VISUAL_SNAPSHOT=1 scripts/validate-d3d11-retrace-wine.sh`，把 `test/artifacts/windows-x86_64/retrace/bin/retrace-d3d11-logs/` 下对应 scene 的 `*-visual.png` 同步到 fixture 目录。
- `scripts/validate-d3d11-retrace-wine.sh` 默认只校验 replay 统计、bundle 资产引用和运行成功；visual compare 作为显式 opt-in，避免 macOS 上 Wine 窗口截图抖动把常规回归跑挂。
