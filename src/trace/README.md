# trace

## 作用

`src/trace/` 负责 trace bundle 的核心类型、目录布局、索引语义，以及 bundle 读写接口。

## 负责的内容

- API / event / object 等基础类型
- bundle 根目录布局
- 资产索引和校验索引
- bundle reader / writer 的接口与最小骨架
- 给转译层使用的辅助 link writer 骨架

## 不负责的内容

- 不负责把目标进程 hook 进来
- 不负责 replay backend 调度
- 不负责平台相关的 D3D11 / D3D12 / Metal 细节

## 当前实现边界

- `api_types.hpp`：API 与 trace 元信息
- `object_types.hpp`：对象、blob 和资源身份
- `event_types.hpp`：事件、边界和 callsite 语义
- `bundle_layout.hpp`：bundle 根布局和固定入口文件名
- `asset_index.hpp`：资产索引语义
- `checksum_index.hpp`：校验索引语义
- `trace_bundle_io.hpp`：bundle reader / writer 接口
- `translation_link_writer.hpp`：转译层辅助链接流写入接口
- `src/`：reader / writer 分离的实现骨架

## 头文件导航

如果你要读 `src/trace/`，建议按下面顺序进入：

1. `api_types.hpp`
   - 看格式版本、API 种类、metadata
2. `object_types.hpp`
   - 看 object id、blob id、对象关系
3. `event_types.hpp`
   - 看 callstream 里一条事件现在长什么样
4. `bundle_layout.hpp`
   - 看 bundle 根目录入口和固定文件名
5. `asset_index.hpp`
   - 看 shader / texture / buffer 等资产索引怎么表达
6. `checksum_index.hpp`
   - 看校验索引怎么表达
7. `trace_bundle_io.hpp`
   - 看 bundle 读写接口和 reader / writer 的边界
8. `translation_link_writer.hpp`
   - 看转译层如何通过库 API 写辅助链接流，而不是自己操作 trace 文件

如果你要改代码，建议先判断是改：

- “语义类型”：
  从 `api_types.hpp` / `object_types.hpp` / `event_types.hpp` 入手
- “目录和索引”：
  从 `bundle_layout.hpp` / `asset_index.hpp` / `checksum_index.hpp` 入手
- “读写过程”：
  从 `trace_bundle_io.hpp` 和 `src/trace/src/` 入手
- “转译层辅助 sideband”：
  从 `translation_link_writer.hpp` 和 `src/trace/src/translation_link_writer.cpp` 入手

## 当前阶段说明

- 这里优先固定结构和命名
- 读写逻辑仍以 TODO 为主
- 可读 callstream、checksums 和资产目录边界应视为当前设计约束
- 转译层如果要写 D3D <-> Metal 的辅助链接流，应通过 `TranslationLinkWriter`，而不是直接碰 bundle 文件布局

## 相关文档

- [总架构文档](../../docs/ARCHITECTURE.md)
- [目录布局文档](../../docs/TRACE_LAYOUT.md)
- [仓库文档索引](../../docs/README.md)
