# WaterStructureCpp

WaterStructureCpp 是一个面向 Minecraft 建筑结构文件的 C++23 解析、转换与
Bedrock 世界读写库。项目注册了 37 种结构格式，并提供静态库、命令行工具、
测试和性能基准；格式兼容性仍在通过真实样本进行逐项验证。

## 上游项目与致谢

本项目基于 [HuaGong54188/WaterStructure](https://github.com/HuaGong54188/WaterStructure)
进行独立的 C++ 迁移与实现。原 Go 项目是格式行为、兼容规则以及差分验证结果的
主要参考实现和 oracle。感谢原项目作者及贡献者对各类 Minecraft 建筑格式所做的
整理与实现。

WaterStructureCpp 不是原项目的官方 C++ 版本。除非另有说明，本仓库中的 C++
代码、构建系统和辅助工具由本项目独立维护。

## 构建

项目使用 [xmake](https://xmake.io/)：

```powershell
xmake f -m release
xmake build
```

主要目标：

- `water_structure`：静态库
- `water_structure_cli`：命令行工具
- `water_structure_tests`：测试程序
- `water_structure_bench`：性能基准
- `cpp_manifest`：Go/C++ 差分验证工具

运行测试：

```powershell
xmake run water_structure_tests
```

## CLI

```text
inspect <input>
to-world <input> <world-or-mcworld>
convert <input> --format <target> --output <path>
```

仅当目标格式存在已实现且已验证的 writer 时，`convert` 才允许输出。

## 性能与内存

MCWorld 世界导出采用流式路径，避免把整个世界或完整 `BlockData` 一次性放入
内存。以下是本地 Release 构建对真实大型地图
`2701 x 176 x 2701`（约 12.84 亿方块、28,561 个 chunk 柱）的观测值；结果会
随 CPU、磁盘和压缩库版本变化，仅用于比较优化前后的量级：

| 阶段 | 转换耗时 | 峰值私有内存 | 说明 |
| --- | ---: | ---: | --- |
| 早期完整缓存路径 | 约 206 秒 | 会出现数 GB 瞬时占用 | 作为历史问题基线 |
| 条带流式 Schem 写入 | 约 33.1 秒 | 约 201 MiB | 输出 SHA-256 与基线一致 |
| BWO 范围/批量 subchunk 读取 | 约 29.3 秒 | 约 175 MiB（工作集约 348 MiB） | 当前默认路径 |

当前 profile（`$env:WATER_STRUCTURE_PROFILE = "1"`）显示该样本的主要阶段约为：

- `get_chunks`：12.34 秒
- BlockData 编码：7.95 秒
- 条带合并：4.60 秒
- 169 个 Z 条带，BlockData 原始字节约 1.29 GB

已落地的性能优化包括：

- Schem BlockData 按 Z 条带写入临时文件，最后按 `y -> z -> x` 顺序合并，避免
  分配完整 BlockData 数组。
- MCWorld chunk 缓存按条带释放，并根据结构的 Y 范围裁剪 subchunk 读取。
- 对齐的完整 16x16 chunk 直接复制 subchunk；边界、负坐标和 offset 仍回退到
  通用逐方块路径，保持兼容性。
- Schem writer 使用 layer0-only 读取，避免无用的第二方块层初始化和扫描。
- varint 编码复用预分配行缓冲，减少热路径上的临时分配。
- BWO 通过一次范围查询读取 chunk 的 subchunk payload，再进行解码，减少逐层
  LevelDB 调用和重复查找。

解析器逐项优化、基准命令、重复测试结果和未完成格式清单记录在
[docs/parser_optimization.md](docs/parser_optimization.md)。

可用以下命令查看阶段耗时：

```powershell
$env:WATER_STRUCTURE_PROFILE = "1"
water_structure_cli.exe convert <input.mcworld> --format SchemV1 --output <output.schem>
```

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE)。使用或分发时也请
遵守上游项目的许可证及署名要求。
