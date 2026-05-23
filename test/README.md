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
```

如果需要配合 apitrace 的 Windows proxy DLL，再把根项目的 Windows cross-build 安装到同一个测试目录即可。

## CLI

固定入口参数：

- `--dx <dx11|dx12>`：当前实现 `dx11`；`dx12` 只保留接口并返回 `not implemented` 风格失败。
- `--scene <name|all>`：单场景调试或顺序跑完整 `dx11 core` 集合。
- `--list-scenes`：列出指定 `dx mode` 下当前可运行的 scene 名称。

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
- 非零：参数错误、`dx12` 占位模式、scene 校验失败、或运行时初始化失败。

## DX11 scenes

当前 `dx11 core` scene 顺序与覆盖点：

1. `smoke_triangle`
   覆盖 swap chain、RTV、input layout、VS/PS、VB、CB、`Map/Unmap`、`Draw`、`Present`。
2. `indexed_instancing`
   覆盖 IB、第二输入槽、instance data、`DrawIndexed`、`DrawIndexedInstanced`。
3. `textured_quad`
   覆盖 `Texture2D`、`ShaderResourceView`、`SamplerState`、`UpdateSubresource`、`PSSetShaderResources`、`PSSetSamplers`，并从 `bin/assets/dx11/` 读取打包纹理资产后上传到 GPU。
4. `depth_blend_scissor`
   覆盖 depth texture、DSV、`ClearDepthStencilView`、`OMSetDepthStencilState`、`OMSetBlendState`、`RSSetState`、`RSSetScissorRects`。
5. `offscreen_copy_composite`
   覆盖离屏 RT、SRV/RTV 复用、`CopyResource`、二次合成到 backbuffer。

当前 5 个 `dx11 core` scene 都不是静态单帧图，而是“前段动画、后段收敛到稳定终帧”的确定性序列：

- 这样顺序回放时能直接观察 draw 顺序、状态切换、离屏 copy 与最终 present 的连续效果。
- 同时 visual fixture 仍可稳定截到一致终帧，不会因为截图时机不同而频繁漂移。

结构预留但未实现：

- `mip_sampling`
- `msaa_resolve`

每个已实现 scene 都会把结果复制到 staging 资源，并在 CPU 侧对固定像素点做阈值校验。

关于“资产是否需要跟着 trace”：

- 测试 demo 现在会从外部打包资产目录读取纹理，再通过 D3D11 API 上传。
- capture 侧的职责不是在 replay 时重新访问这些原始源文件，而是把进入 D3D11 的资源内容与元数据完整记录到 trace bundle。
- 因此 replay 只依赖 trace bundle 内的 `textures/`、`buffers/`、`shaders/`、`pipelines/` 等资产，不依赖原始应用安装目录。

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

当前用于 `retrace` 测试的固定素材按 scene 拆分放在：

- `test/fixtures/retrace/d3d11-smoke_triangle/smoke_triangle-d3d11.apitrace/`
- `test/fixtures/retrace/d3d11-smoke_triangle/smoke_triangle-d3d11-visual.png`
- `test/fixtures/retrace/d3d11-indexed_instancing/indexed_instancing-d3d11.apitrace/`
- `test/fixtures/retrace/d3d11-indexed_instancing/indexed_instancing-d3d11-visual.png`
- `test/fixtures/retrace/d3d11-textured_quad/textured_quad-d3d11.apitrace/`
- `test/fixtures/retrace/d3d11-textured_quad/textured_quad-d3d11-visual.png`
- `test/fixtures/retrace/d3d11-depth_blend_scissor/depth_blend_scissor-d3d11.apitrace/`
- `test/fixtures/retrace/d3d11-depth_blend_scissor/depth_blend_scissor-d3d11-visual.png`
- `test/fixtures/retrace/d3d11-offscreen_copy_composite/offscreen_copy_composite-d3d11.apitrace/`
- `test/fixtures/retrace/d3d11-offscreen_copy_composite/offscreen_copy_composite-d3d11-visual.png`

兼容保留的早期 smoke fixture：

- `test/fixtures/retrace/d3d11-triangle/triangle-d3d11.apitrace/`
- `test/fixtures/retrace/d3d11-triangle/triangle-d3d11-visual.png`

其中 `test/artifacts/` 和 `test/build/` 只保留为临时生成物，不应作为测试输入依赖。要刷新 fixture，先重新跑 `scripts/validate-d3d11-wine.sh`，再把 `test/artifacts/windows-x86_64/demo/bin/dx11-core-scene-traces/` 下对应 scene 的 bundle（包括 `pipelines/` sideband 资产），以及 `test/artifacts/windows-x86_64/demo/bin/dx11-core-scene-logs/` 下对应 scene 的 `*-visual.png` 同步到上述目录。
