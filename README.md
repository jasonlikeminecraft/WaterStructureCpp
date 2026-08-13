# WaterStructureCpp

## Library targets and Python bindings

The project now provides three consumption layers:

- `water_structure`: the existing C++23 static library for applications that
  want the native `WaterStructure/*` API.
- `water_structure_shared`: a Windows DLL with the stable C ABI declared in
  [`include/WaterStructure/c_api.h`](include/WaterStructure/c_api.h). The ABI
  uses opaque handles, UTF-8 paths, integer result codes, and a context-owned
  error string; C++ STL types and exceptions never cross the DLL boundary.
- `python/`: a dependency-free `ctypes` wrapper. Set
  `WATER_STRUCTURE_LIBRARY` to the built DLL, or place the DLL beside the
  module, then use `Context.inspect()`, `Context.convert()`, and
  `Context.to_world()`.

Build the native targets with:

```text
xmake build -m release water_structure water_structure_shared water_structure_cli
```

The Python wrapper expects the runtime mapping assets. Pass the assets
directory explicitly when it is not under the current working directory:

```python
from water_structure import Context

ctx = Context(r"D:\Projects\WaterStructureCpp\assets")
info = ctx.inspect(r"D:\import\input.bdx")
ctx.convert(r"D:\import\input.bdx", "SchemV1", r"D:\import\output.schem")
```

For CMake consumers, configure with `-DWATER_STRUCTURE_BUILD_SHARED=ON`, then
install the package. The install tree exports `WaterStructure::water_structure`
(static) and `WaterStructure::water_structure_shared` (DLL plus C ABI):

```text
cmake -S . -B build/cmake-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake-release --config Release
cmake --install build/cmake-release --prefix dist/install
```

The repository also contains `cmake/copy_runtime.ps1` for a relocatable
Windows runtime directory, `python/build_wheel.ps1` for a wheel that bundles
the DLL and mapping assets, `nuget/pack.ps1` for the native NuGet package, and
an overlay port under `ports/water-structure` for vcpkg integration.

The current source tree verifies xmake, DLL loading, C ABI calls, and Python
source-tree usage. CMake/NuGet/vcpkg packaging requires their respective host
tools and dependency providers; the scripts fail early if those tools are not
installed rather than silently producing an incomplete package.

The C ABI is versioned independently with `ws_abi_version()`. ABI version 1
currently exposes reader inspection, validated format conversion, and the
streaming `to_world` path. New functions will be appended without changing
existing structures or ownership rules.

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
convert <input> --format <target> --output <path> [--threads <count>]
```

仅当目标格式存在已实现且已验证的 writer 时，`convert` 才允许输出。

MCFunction writer 按 chunk 批次流式读取，只输出非空气方块，并用不超过 32,768
方块的 `fill` 命令清空结构范围以保留空气和尺寸。由于 MCFunction 没有统一的
Bedrock 方块实体协议，遇到方块实体 NBT 时会明确报错，不会静默丢弃。
MCFunction 默认使用 2 个编码线程；`--threads 1` 可关闭并行，显式指定更高线程数
可用于按目标机器重新测量，但内存带宽受限的大型世界并不一定更快。

MCFunction reader 同样采用有界流式路径：逐行解析后只保留紧凑的
`setblock`/`fill` 命令，不会把大 `fill` 展开为逐方块数组；下游按需请求 chunk 时
才生成该 chunk 的交集。因此导入内存随命令数增长，而不是随填充体积增长。

## 性能与内存

流式处理是本库的核心约束，不是只针对个别大型样本的可选优化。reader、writer 和
世界适配层不得以完整结构体积为比例保留方块数组、命令流或解压结果；必须使用有界
缓冲、chunk/subchunk 批次或临时文件。新增格式和优化只有在保持此约束、错误兼容性
及 canonical manifest 不变时才可合入。

MCWorld 世界导出采用流式路径，避免把整个世界或完整 `BlockData` 一次性放入
内存。以下是本地 Release 构建对真实大型地图
`2701 x 176 x 2701`（约 12.84 亿方块、28,561 个 chunk 柱）的观测值；结果会
随 CPU、磁盘和压缩库版本变化，仅用于比较优化前后的量级：

| 阶段 | 转换耗时 | 峰值私有内存 | 说明 |
| --- | ---: | ---: | --- |
| 早期完整缓存路径 | 约 206 秒 | 会出现数 GB 瞬时占用 | 作为历史问题基线 |
| 条带流式 Schem 写入 | 约 33.1 秒 | 约 201 MiB | 输出 SHA-256 与基线一致 |
| BWO 范围/批量 subchunk 读取 | 约 29.3 秒 | 约 175 MiB（工作集约 348 MiB） | 当前默认路径 |

同一乌托邦样本输出 MCFunction 时，固定线程池将独立 chunk 批次并行合并，主线程
仍按原任务顺序写出。经整数格式化、磁盘 palette 状态缓存和 subchunk 零拷贝读取
优化后，Release 写入 `NUL` 实测 1 线程约 17.06 秒、2 线程约 9.55 秒；真实文件
写入约 10.79 秒。输出为 897,044,964 字节，SHA-256 为
`359696D912A4969C935CCFDEEF7A90509C7AD2A53951C22675019ED7D5BC2492`，峰值私有
内存约 154 MiB。3/4/8 线程分别约 11.31/11.05/11.28 秒，表明该负载在 2 个编码
线程后已受内存带宽和缓存竞争限制，因此默认值固定为 2，而不是逻辑核心数。

BDX 双向转换也使用有界流式路径。Kuudra 样本（`188 x 175 x 185`，实际写入
2,705,661 个非空气方块）从 MCWorld 输出 BDX 由约 27.15 秒、278 MiB 降至约
1.49 秒、152 MiB；BDX 写回 MCWorld 由约 2.96 秒、169 MiB 降至约 1.68 秒、
159 MiB。writer 使用 Brotli quality 6，因此该样本压缩文件由 143,935 字节增加
到 173,178 字节。新旧 BDX 的 canonical chunk manifest 完全一致。

大型 Schem reader 使用稀疏行检查点直接访问 varint BlockData，不再生成数 GB 的
定宽索引临时文件。本地 Flight 样本（`2610 x 282 x 2615`）的完整 benchmark
由约 84 秒降至约 40 秒，峰值私有内存约 161 MiB，checksum 保持一致。所有
1 MiB I/O 缓冲均在堆上分配，兼容 Windows 默认线程栈大小。

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
- BWO 对重复的磁盘 block-state NBT 使用最多 8 MiB 的 runtime ID 缓存；命中时
  只扫描 NBT 边界，不再重复分配属性 map 或执行 upgrade schema。
- BWO 解码后的 4096 项方块层通过只读 span 交给世界适配层，并在内部移动接管
  解码数组，消除两次完整 layer 拷贝。
- Schem reader 在解压阶段建立行和分段偏移，按实际行长度读取并直接解码
  varint；超宽行的 16 位检查点溢出时自动回退到行首扫描。
- MCFunction writer 使用有界固定线程池，世界读取保持单线程；每个工作线程复用
  扫描工作区和 Java 状态缓存，任务结果超过 2 MiB 时自动溢写临时文件，避免
  高并发输出导致内存失控，并按任务序号确定性合并。命令整数使用 `to_chars`
  写入连续 staging buffer，避免热路径上的 iostream 格式化开销。
- BDX reader 直接从 64 KiB Brotli 解压窗口解析命令，并缓存 constant/pool palette
  的 runtime ID；writer 以 32 个 chunk 为上限读取世界，使用 256 KiB staging
  buffer 流式压缩，不再同时保留完整 chunk 集、命令流和压缩输出。
- BDX 导入世界时不保存完整方块列表。首轮只计算游标边界和非空气数量，不构造
  runtime state 或方块实体 NBT；第二轮使用有界 chunk 缓存直接写入世界。缓存容量
  覆盖 writer 的 X 批次，避免正常 BDX 输出在 Y 层切换时反复重载同一批 chunk；首轮
  的原始 NBT 使用深度受限的 typed skip，不创建 libnbt 对象树。
- BDX 解压输出增加 64 KiB 用户态读取窗口，避免每个命令字段调用一次 `istream`
  读取；世界写入按 16 个 chunk 聚合 `saveSubChunksBatch`，profile 模式会报告
  `save_batches`、`reloads` 和 `save_ms`，用于区分命令解析与 LevelDB 写入瓶颈。
- vector-backed reader 的 chunk 索引使用带容量检查的 32 位编号，将 BDX→MCWorld
  每方块索引开销由 8 字节降为 4 字节。

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
