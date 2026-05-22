# trace demo

这里放最小可运行的 Windows trace demo。

目标产物：

- `apitrace_triangle_d3d11.exe`
- `apitrace_triangle_d3d12.exe`

推荐构建：

```sh
cmake -S test -B test/build/windows-x86_64 -DCMAKE_TOOLCHAIN_FILE=test/toolchains/windows-x86_64-mingw.cmake
cmake --build test/build/windows-x86_64
cmake --install test/build/windows-x86_64 --prefix test/artifacts/windows-x86_64/demo
```

运行时可用 Wine 直接启动：

```sh
wine apitrace_triangle_d3d11.exe
wine apitrace_triangle_d3d12.exe
```

如果需要配合 apitrace 的 Windows proxy DLL，再把根项目的 Windows cross-build 安装到同一个测试目录即可。

## D3D11 MVP 验收

仓库内的 D3D11 MVP 验收脚本：

```sh
scripts/validate-d3d11-wine.sh
```

它会：

- 构建 apitrace 的 Windows `d3d11.dll`
- 构建 `apitrace_triangle_d3d11.exe`
- 把 proxy DLL 和 DXMT 运行时放到同一目录
- 在 Wine 下启动 D3D11 demo
- 校验 trace bundle 内的 `callstream.jsonl` / `checksums.json` / `objects.json`
- 校验 shader / buffer 资产引用存在且非空

在 macOS 桌面环境下，脚本默认还会：

- 用 Wine 虚拟桌面承载 demo，避免前台窗口被其他应用遮挡
- 生成 `test/artifacts/windows-x86_64/demo/bin/triangle-d3d11-visual.png`
- 对截图做最小像素检查，确认窗口里出现了彩色三角形，而不是纯黑或空白窗口

## retrace fixture

当前用于 `retrace` 测试的固定素材放在：

- `test/fixtures/retrace/d3d11-triangle/triangle-d3d11.apitrace/`
- `test/fixtures/retrace/d3d11-triangle/triangle-d3d11-visual.png`

其中 `test/artifacts/` 和 `test/build/` 只保留为临时生成物，不应作为测试输入依赖。要刷新 fixture，先重新跑验收脚本，再把生成的 bundle 覆盖到上述路径。
