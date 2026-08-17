# WaterStructureCpp

## Library targets and Python bindings

The project now provides three consumption layers:

- `water_structure`: the existing C++23 static library for applications that
  want the native `WaterStructure/*` API.
- `water_structure_shared`: a platform shared library (`.dll`, `.so`, or
  `.dylib`) with the stable C ABI declared in
  [`include/WaterStructure/c_api.h`](include/WaterStructure/c_api.h). The ABI
  uses opaque handles, UTF-8 paths, integer result codes, and a context-owned
  error string; C++ STL types and exceptions never cross the DLL boundary.
- `python/`: a dependency-free `ctypes` wrapper. Each platform wheel bundles
  its native library and assets; source-tree users can set
  `WATER_STRUCTURE_LIBRARY` to a locally built shared library. Use
  `Context.inspect()`, `Context.convert()`, and
  `Context.to_world()`.

Build the native targets with:

```text
xmake build -m release water_structure water_structure_shared water_structure_cli
```

Install a published platform wheel from PyPI with:

```text
python -m pip install water-structure
```

The installed package automatically locates its bundled runtime assets:

```python
from water_structure import Context

with Context() as ctx:
    info = ctx.inspect(r"D:\import\input.bdx")
    ctx.convert(r"D:\import\input.bdx", "SchemV1", r"D:\import\output.schem")
```

To build a wheel on Windows, Linux, or macOS, install `build` and `twine`, then
run the platform-neutral builder:

```text
python -m pip install build twine
python python/build_wheel.py
python -m twine check dist/python/*.whl
```

The builder lets xmake choose its default parallelism; it does not force a
thread count. Windows users may continue to call `python/build_wheel.ps1`,
which is a thin wrapper around the same Python builder.

Use `.\python\publish.ps1 -TestPyPI` for a TestPyPI upload and
`.\python\publish.ps1` only after the TestPyPI installation test succeeds.
The bindings support Python 3.9+ on Windows, Linux, and macOS. Package and
native versions must be updated together in `python/pyproject.toml` and
`ws_version()`.

For CMake consumers, configure with `-DWATER_STRUCTURE_BUILD_SHARED=ON`, then
install the package. The install tree exports `WaterStructure::water_structure`
(static) and `WaterStructure::water_structure_shared` (DLL plus C ABI):

```text
cmake -S . -B build/cmake-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake-release --config Release
cmake --install build/cmake-release --prefix dist/install
```

The repository also contains `cmake/copy_runtime.ps1` for a relocatable
Windows runtime directory, `python/build_wheel.py` for a wheel that bundles
the platform library and mapping assets, `nuget/pack.ps1` for the native NuGet package, and
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

直接双击或无参数运行 `water_structure_cli.exe` 会进入中文交互向导，和 Go 版一样
逐步提示源文件、自动检测格式、选择目标格式、输出路径、线程数以及世界坐标。路径
可以直接从资源管理器拖入控制台；任何步骤输入 `q` 都可以退出。

```text
water_structure_cli formats [--writers-only]
water_structure_cli inspect <input>
water_structure_cli convert <input> <output> [--format <target>] [--threads <count>]
water_structure_cli to-world <input> <world-or-mcworld> [--start <x,y,z>]
```

CLI 参考 Go 版任意结构转换流程：自动识别输入格式、检查目标 writer capability、
创建输出目录并报告耗时。目标扩展名唯一时可以省略 `--format`：

转换时默认显示实时进度、已用时间和预计剩余时间（ETA）。对于暂未提供细粒度
回调的 writer，CLI 会在写入阶段显示旋转指示器并每 250ms 刷新一次真实已用时间；
这类阶段的 ETA 会标记为 `n/a`，不会伪造 50% 之类的进度。`--quiet` 可以关闭这些
终端输出。

```powershell
water_structure_cli convert input.bdx output.mcstructure
water_structure_cli convert input.mcworld output.bdx --threads 4
# .schem 同时对应 V1/V2，必须明确版本：
water_structure_cli convert input.bdx output.schem --format SchemV1
water_structure_cli to-world input.schem output.mcworld --start 0,-4,0
```

旧的 `convert <input> --format <target> --output <path>` 参数形式继续兼容。仅当目标
格式存在已实现且已验证的 writer 时，`convert` 才允许输出；`formats --writers-only`
可以查看当前可用目标。

MCFunction writer 按 chunk 批次流式读取，只输出非空气方块，并用不超过 32,768
方块的 `fill` 命令清空结构范围以保留空气和尺寸。MCFunction 没有统一的
Bedrock 方块实体协议，因此方块实体 NBT 会按约定跳过，不会静默改写。
MCFunction 默认使用 2 个编码线程；`--threads 1` 可关闭并行，显式指定更高线程数
可用于按目标机器重新测量，但内存带宽受限的大型世界并不一定更快。

MCWorld 输入走专用的 palette 流式路径：库新增了 `visit_chunk_palettes()`
（`SubChunkPaletteData`：子区块 palette + 4096 个 native `(x,y,z)` 索引）和
`BedrockWorldAdapter::load_subchunk_palette()`。MCFunction writer 直接从
palette 读取方块状态——每个不同状态只升级并格式化一次（转换期内按状态签名
缓存），再按 indices 扫描生成 `fill`/`setblock`，完全跳过内部 runtime ID 映射和
逐方块状态反查；其他格式仍走通用的 `get_chunks_layer0()` 路径，行为不变。
MCWorld 输入的加载（LevelDB 读取 + 子区块解码）由独立线程预取，与编码线程
流水线重叠，避免单线程解码成为整条链路的瓶颈。

MCFunction reader 同样采用有界流式路径：逐行解析后只保留紧凑的
`setblock`/`fill` 命令，不会把大 `fill` 展开为逐方块数组；下游按需请求 chunk 时
才生成该 chunk 的交集。因此导入内存随命令数增长，而不是随填充体积增长。

IBImport writer 的命令段和命令方块 NBT 段均逐 chunk 读取并立即 XOR 写出；段长度
在结束时回填，不缓存完整命令文本、全部 chunk 坐标或全部方块实体。每个 chunk
处理完成后主动释放 reader 缓存，峰值工作集受自适应读取批次和有界编码队列约束。
方块命令会在单个 chunk 内做有界三维贪心合并：孤立方块使用 `setblock`，连续同状态
区域使用不超过 32,768 方块的 `fill`。reader 对 XOR 段逐块解码，只紧凑保存
`setblock`/`fill` 命令，并在下游请求 chunk 时物化交集；不再有旧版 256 MiB 段限制，
也不会按 `fill` 体积展开常驻方块数组。

IBImport 的有界编码线程池同时用于 palette 和通用 `ChunkData` 路径，因此 Schem、
BDX、MCStructure 等任意 reader 都可通过 `--threads` 并行执行三维合并与命令编码。
reader 加载仍固定在调用线程，避免并发访问格式内部缓存；编码结果按 chunk 原始顺序
写入，所以不同线程数生成的 IBI 字节完全一致。若某种格式的 reader 解码本身占主导，
通用路径会按结构高度在约 96 MiB 预算内扩大 X 批次，让 Schem 等行式格式尽量一次
解码完整行，避免固定32 chunk批次反复读取同一 BlockData。Schem 还会直接输出
结构级共享 palette 和原生索引，绕过 runtime ID `ChunkData`；空气 subchunk 不分配，
palette 升级与文本编码仅执行一次。乌托邦 Schem 实测由最初约90.8秒降至54.0秒，
再降至约44–46秒（3线程），输出818.4 MiB且各路径 SHA-256 一致。

## 性能与内存

流式处理是所有转换器的默认行为：reader 按命令、chunk 或 subchunk 读取，writer
按批次写出；不会因为输入地图很大而把完整方块数组长期留在内存中。下面是当前
Release 构建在本机实测的端到端结果，数字用于比较转换器量级，不是硬件保证值。

| 转换方向 | 测试样本 | 输出 | 耗时 | 峰值私有内存 |
| --- | --- | --- | ---: | ---: |
| MCWorld → SchemV1 | 乌托邦，`2701×176×2701`，约 2.86 万 chunk 柱 | `.schem` | 约 29.3 秒 | 约 175 MiB（工作集约 348 MiB） |
| MCWorld → MCFunction | 同上 | `.mcfunction` | 约 13 秒（palette 流水线，本机复测；generic 路径约 16 秒） | 约 154 MiB |
| MCWorld → IBImport | chenshi，`2716×342×2245` | `.ibi` | 约 10–12 秒（palette 流水线，3 个编码线程） | 私有内存约 136 MiB（工作集约 290 MiB） |
| Schem → IBImport | 乌托邦，`2701×176×2701` | `.ibi`（818.4 MiB） | 约 44–46 秒（共享 palette 流水线，3 个编码线程） | 私有内存/工作集约 144 MiB |
| MCWorld → BDX | Kuudra，`188×175×185`，270.6 万非空气方块 | `.bdx` | 约 1.49 秒 | 约 152 MiB |
| BDX → MCWorld | 同一 Kuudra BDX | 世界目录/`.mcworld` | 约 1.68 秒 | 约 159 MiB |
| Schematic → MCWorld | `519×256×519`，1089 个 chunk | 世界目录/`.mcworld` | 约 3.6 秒 | 约 412 MiB |
| SchemV1 → MCWorld | Flight，`2610×282×2615` | 世界目录 | 约 29 秒 | 约 161 MiB |

MCFunction 编码默认使用 2 个线程；在乌托邦样本上，1/2/3/4/8 个编码线程约为
17.06/9.55/11.31/11.05/11.28 秒。该负载在 2 个线程后受内存带宽限制，因此
线程越多不一定越快。其他 writer 当前主要受源文件解码、压缩或 LevelDB 写入速度
限制，`--threads` 不会自动让所有格式线性加速。

palette 流水线路径（MCWorld → MCFunction）相对通用路径的收益与输入状态多样性
相关：状态单调的世界两者接近，状态多样的世界差距明显——chenshi 样本
（`2716×278×2245`）上 palette 路径约 14 秒，而通用路径约 55 秒。

IBImport 的同一 chenshi 样本在纯 `setblock` 版本中耗时约 289.4 秒、输出约
5.35 GiB；启用 chunk 内三维 `fill` 后输出降至约 386.6 MiB。当前 MCWorld 输入
进一步直接消费 palette/packed indices，以 32 个 chunk 为读取批次，并用有界任务
队列保持三线程编码结果的确定顺序，端到端约 10–12 秒；相对最初版本速度约提升
24–29 倍、文件体积减少约 93%。优化前后输出 SHA-256 一致，峰值内存基本不变。

已经支持的 JSON、MessagePack、NBT、BDX、IBImport、MCFunction 等格式都走相同的
有界 chunk/subchunk 管线；但没有大型真实样本的格式目前只有最小 fixture 的测试
耗时，不能与上表的大地图转换时间直接比较。详细的测试命令、样本和阶段 profile
记录在 [docs/parser_optimization.md](docs/parser_optimization.md)。

可用以下命令查看阶段耗时：

```powershell
$env:WATER_STRUCTURE_PROFILE = "1"
water_structure_cli.exe convert <input.mcworld> --format SchemV1 --output <output.schem>
```

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE)。使用或分发时也请
遵守上游项目的许可证及署名要求。
