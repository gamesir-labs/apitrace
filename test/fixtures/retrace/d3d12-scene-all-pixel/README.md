# D3D12 scene-all pixel retrace fixture

这份 fixture 是 `scripts/validate-d3d12-wine.sh` 生成的 D3D12 `--scene all` 小帧数 trace 样本，供 retrace 逐帧像素对比测试使用。

保留内容：

- `dx12-scene-all-pixel.apitrace/`

该 bundle 显式启用了 `APITRACE_D3D12_CAPTURE_PRESENT_FRAMES=1`，因此包含 `D3D12PresentFrame` debug 资产。它只作为预览和像素对比基准使用；retrace 不允许直接播放这些帧。

刷新 fixture 时执行：

```sh
APITRACE_D3D12_UPDATE_PIXEL_FIXTURE=1 scripts/validate-d3d12-wine.sh
```

默认使用 `APITRACE_D3D12_PIXEL_COMPARE_FRAMES=3`，即每个默认 D3D12 scene 记录 3 帧。不要把 Wine prefix、build tree、`test/artifacts/` 下的临时 retrace bundle 或长时间人工观察 trace 同步进 fixture。
