# D3D11 triangle retrace fixture

这份 fixture 是 `scripts/validate-d3d11-wine.sh` 生成的最小 D3D11 trace 样本，供 `retrace` 回放测试使用。

保留内容：

- `triangle-d3d11.apitrace/`
- `triangle-d3d11-visual.png`

更新时只同步这两个产物，不要把 Wine prefix、build tree 或 proxy 安装目录带进来。
