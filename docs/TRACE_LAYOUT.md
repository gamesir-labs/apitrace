# Trace Bundle 目录布局

本文档只定义磁盘布局，不定义 capture hook、replay runtime 或后端行为。

仓库内有一份对应的结构样例，可参考：

- `examples/sample-trace-bundle/sample.apitrace/`

目标是三件事：

- 保证磁盘格式解耦，可拆分，可替换
- 让 shader、纹理、buffer 等资产以独立文件存在
- 让 retrace 在只读取根目录索引文件的前提下找到完整调用语义和所有资产

## 可读性原则

除了原始资产这类二进制文件以外，其它文件都应优先做到语义可读。

这里的“其它文件”包括：

- 调用语义入口
- 校验索引
- 对象关系索引
- pipeline 辅助信息
- 各类 analysis 输出

例外主要是原始资产本体，例如：

- DXBC
- DXIL
- serialized root signature
- 纹理原始内容
- buffer 原始内容

即使 DXIL 这类内容可以反汇编，它仍然属于原始资产，优先按原样保存。

## 总体原则

### 不采用分片式大文件

不使用下面这种布局：

```text
foo.trace.1
foo.trace.2
foo.trace.3
```

这种布局的主要问题是：

- 文件命名不表达语义
- shader、纹理、调用流被迫耦合在同一套切片规则里
- 后续替换、校验、去重、按类型分析都很别扭

### 一类语义，一类文件

磁盘上的每类数据都应当按语义拆开：

- 调用语义单独存放
- shader 单独存放
- 纹理单独存放
- buffer 单独存放
- pipeline 相关对象单独存放

### 根目录只保留索引入口

一个 trace bundle 的根目录只保留少量入口文件。

其中两个文件是必需的：

- `checksums.json`：校验索引
- `callstream.jsonl`：调用语义入口

如果没有 `callstream.jsonl`，就无法做 retrace。  
如果没有 `checksums.json`，就无法可靠校验 bundle 是否完整。

## 推荐目录结构

```text
sample.apitrace/
  checksums.json
  callstream.jsonl

  shaders/
    2f1c8e9a.dxbc
    a93e3f11.dxil
    b7710210.rootsig

  textures/
    c8321ab2.texture.zst

  buffers/
    91aa77f0.buffer.zst

  pipelines/
    18de92a1.pipeline.json

  objects/
    objects.json

  analysis/
    shader-reflection.json
    translation-links.jsonl
```

## 根目录必需文件

### `callstream.jsonl`

这是 retrace 的主入口文件。

它必须保存：

- 原始 D3D 调用顺序
- 调用类型和函数标识
- 关键参数
- 返回值和错误码
- object id / resource id / shader id 等引用
- frame / submit / present / barrier / fence 等边界语义

它不负责内嵌大资产本体，只负责引用它们。

推荐采用逐行记录的文本格式，例如 JSON Lines。这样有几个好处：

- 人可以直接阅读和 review
- 工具可以顺序流式处理
- 单条事件损坏时更容易定位问题
- 需要时可以单独抽取某一段调用语义

换句话说：

- `callstream.jsonl` 保存“要做什么”
- 资产目录保存“这些调用引用了什么”

### `checksums.json`

这是 bundle 的完整性校验入口。

它至少应当覆盖：

- `callstream.jsonl`
- 所有 shader 文件
- 所有纹理文件
- 所有 buffer 文件
- 所有 pipeline 文件
- 其他被 `callstream.jsonl` 直接引用的文件

推荐字段：

```json
{
  "format_version": 1,
  "bundle_hash": "sha256:...",
  "files": {
    "callstream.jsonl": "sha256:...",
    "shaders/2f1c8e9a.dxbc": "sha256:...",
    "textures/c8321ab2.texture.zst": "sha256:..."
  }
}
```

`checksums.json` 的职责是校验，不是重放。  
retrace 不应从这里恢复调用语义。

## 资产目录约束

### `shaders/`

shader 必须以原始 blob 形式独立保存。

推荐规则：

- DXBC：`.dxbc`
- DXIL container：`.dxil`
- serialized root signature：`.rootsig`

不要把 shader 塞进 `callstream.jsonl`，也不要只保存反射结果。

### `textures/`

纹理内容独立保存，推荐压缩：

- 文件后缀可用 `.texture.zst`

存储重点是原始内容和必要布局，而不是展示友好的可视化格式。

### `buffers/`

buffer 初始数据或快照独立保存，推荐压缩：

- 文件后缀可用 `.buffer.zst`

### `pipelines/`

pipeline 相关结构可以先独立保存为便于 review 的中间格式，例如：

- `.pipeline.json`

但要注意：

- 这是辅助资产
- 不能替代 `callstream.jsonl` 中的调用语义

### `objects/`

对象图可以集中存放在：

- `objects/objects.json`

它用于加速对象关系恢复，但不是 retrace 的唯一依据。

### `analysis/`

`analysis/` 用于存放可读的派生信息和辅助 sideband 流。

例如：

- `shader-reflection.json`
- `translation-links.jsonl`

其中 `translation-links.jsonl` 的定位是：

- 可选文件
- 给转译层 debug 使用的辅助链接流
- 由 `apitrace` 提供的库 API 写入
- `apitrace` 不解释其中的链接语义，只负责保存

## 文件引用规则

`callstream.jsonl` 中不直接内嵌大块资产内容，而是通过稳定 id 或路径引用：

- `shader_id`
- `texture_id`
- `buffer_id`
- `object_id`

这些引用最终解析到具体文件，例如：

- `shader_id -> shaders/2f1c8e9a.dxil`
- `texture_id -> textures/c8321ab2.texture.zst`

## 命名规则

文件名应该表达内容类型，而不是表达“第几片”。

推荐：

- `2f1c8e9a.dxil`
- `91aa77f0.buffer.zst`
- `18de92a1.pipeline.json`

不推荐：

- `trace.1`
- `trace.2`
- `bundle.part3`

更稳妥的方式是使用内容 hash 或稳定 id 作为文件名主体。

## 派生数据和原始数据分离

原始数据和派生分析结果必须分开。

例如：

- 原始 DXIL / DXBC 放在 `shaders/`
- shader reflection 放在 `analysis/`

不要让派生数据覆盖原始输入。

## retrace 的最小读取路径

一个最小的 retrace 实现至少需要读取：

1. `callstream.jsonl`
2. `checksums.json`
3. `callstream.jsonl` 引用到的资产文件

`analysis/translation-links.jsonl` 这类辅助流不属于最小 retrace 必需输入。
它们存在时可用于 debug、对比和转译层自定义分析。

其中：

- `callstream.jsonl` 提供调用语义
- `checksums.json` 提供完整性校验
- 资产目录提供 shader、纹理、buffer 等真实载荷

## 第一版实施建议

第一版先不要把所有东西都做复杂。

建议优先固定这几部分：

1. `callstream.jsonl`
2. `checksums.json`
3. `shaders/`
4. `textures/`
5. `buffers/`

等 retrace 链路跑通后，再逐步补：

- `pipelines/`
- `objects/`
- `analysis/`

## 与现有代码命名的关系

当前代码里的 `TraceArchiveWriter/Reader` 更像单体容器命名。

如果按本文档推进，后续更合理的名字是：

- `TraceBundleWriter`
- `TraceBundleReader`

因为这里的物理存储单元不再是假定的单文件 archive，而是一个目录 bundle。
