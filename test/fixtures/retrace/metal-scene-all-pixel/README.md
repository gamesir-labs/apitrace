# Metal scene-all pixel retrace fixture

这份 fixture 是 native Metal `--scene all` 经过 `retrace --metal` + `APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1` 生成的像素基准 bundle，供 `scripts/validate-metal-native.sh` 做逐帧 present 结果对比。

保留内容：

- `metal-scene-all-pixel.apitrace/`

该 bundle 的 `callstream.jsonl` 包含 `MetalPresentFrame` debug 资产，像素内容为 BGRA8 原始纹理字节；对比脚本会在比较前统一转换到 RGBA。

刷新 fixture 时，先生成一份新的 scene-all trace，再执行：

```sh
APITRACE_TRACE_BUNDLE=/tmp/metal-scene-all-pixel-update.apitrace \
APITRACE_METAL_RETRACE_CAPTURE_PRESENT_FRAMES=1 \
build/cmake/retrace --metal /tmp/metal-scene-all.apitrace
```

随后只同步 `metal-scene-all-pixel.apitrace/`，不要把 `build/`、`test/artifacts/`、临时 trace bundle 或人工调试日志带进版本库。
