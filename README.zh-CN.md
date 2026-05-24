# apitrace

[English](README.en.md)

`apitrace` 当前提供两类 Windows 侧产物：

- `retrace.exe`：trace bundle 的回放入口。
- `d3d11.dll` / `d3d12.dll`：给 Wine/Windows override 模式使用的 capture proxy DLL。

开发文档入口见 [docs/README.md](docs/README.md)。

## 使用方式

### 1. capture: Wine override 模式

把 `override/` 下的 DLL 放到目标程序同目录，然后按需要设置 override。

DX11 典型示例：

```sh
WINEDLLOVERRIDES="d3d11=n,b;mscoree,mshtml=d" wine game.exe
```

如果要覆盖 DX12 路径，则把 `d3d12.dll` 一并放到同目录，并追加对应 override。

### 2. retrace: Wine + downstream runtime

`retrace.exe` 本身只是 replay CLI。它运行时需要真正的 downstream D3D runtime，例如 DXMT 提供的：

- `d3d11.dll`
- `dxgi.dll`
- `winemetal.dll`

因此回放时不要把 CI artifact 里的 capture proxy `d3d11.dll` 放在 `retrace.exe` 旁边替代 downstream runtime。

更直接的做法是：

1. 把 `retrace/retrace.exe` 放到一个单独目录。
2. 再把 DXMT 或其他目标 translation-layer 提供的 runtime DLL 放到同目录。
3. 用 Wine 启动 `retrace.exe <trace-bundle>`。

DXMT 路径下的示例：

```sh
WINEDLLOVERRIDES="d3d11,dxgi,winemetal=n,b;mscoree,mshtml=d" \
wine retrace.exe path/to/scene.apitrace
```

`retrace.exe` 的验证流程可参考 [scripts/validate-d3d11-retrace-wine.sh](scripts/validate-d3d11-retrace-wine.sh)。
