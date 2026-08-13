# WaterStructureCpp

## Library targets and Python bindings

The project now provides three consumption layers:

- `water_structure`: the existing C++23 static library for applications that
  want the native `WaterStructure/*` API.
- `water_structure_shared`: a Windows DLL with the stable C ABI declared in
  [`include/WaterStructure/c_api.h`](include/WaterStructure/c_api.h). The ABI
  uses opaque handles, UTF-8 paths, integer result codes, and a context-owned
  error string; C++ STL types and exceptions never cross the DLL boundary.
- `python/`: a dependency-free `ctypes` wrapper. The PyPI wheel bundles the
  DLL and assets; source-tree users can set `WATER_STRUCTURE_LIBRARY` to a
  locally built DLL. Use `Context.inspect()`, `Context.convert()`, and
  `Context.to_world()`.

Build the native targets with:

```text
xmake build -m release water_structure water_structure_shared water_structure_cli
```

Install the prebuilt Windows x64 wheel from PyPI with:

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

To build and validate a wheel locally, install `build` and `twine`, then run:

```powershell
python -m pip install build twine
.\python\build_wheel.ps1
python -m twine check .\dist\python\*.whl
```

Use `.\python\publish.ps1 -TestPyPI` for a TestPyPI upload and
`.\python\publish.ps1` only after the TestPyPI installation test succeeds.
The first release supports Python 3.9+ on Windows x64. Package and native
versions must be updated together in `python/pyproject.toml` and `ws_version()`.

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
方块的 `fill` 命令清空结构范围以保留空气和尺寸。由于 MCFunction 没有统一的
Bedrock 方块实体协议，遇到方块实体 NBT 时会明确报错，不会静默丢弃。
MCFunction 默认使用 2 个编码线程；`--threads 1` 可关闭并行，显式指定更高线程数
可用于按目标机器重新测量，但内存带宽受限的大型世界并不一定更快。

MCFunction reader 同样采用有界流式路径：逐行解析后只保留紧凑的
`setblock`/`fill` 命令，不会把大 `fill` 展开为逐方块数组；下游按需请求 chunk 时
才生成该 chunk 的交集。因此导入内存随命令数增长，而不是随填充体积增长。

## 性能与内存

流式处理是所有转换器的默认行为：reader 按命令、chunk 或 subchunk 读取，writer
按批次写出；不会因为输入地图很大而把完整方块数组长期留在内存中。下面是当前
Release 构建在本机实测的端到端结果，数字用于比较转换器量级，不是硬件保证值。

| 转换方向 | 测试样本 | 输出 | 耗时 | 峰值私有内存 |
| --- | --- | --- | ---: | ---: |
| MCWorld → SchemV1 | 乌托邦，`2701×176×2701`，约 2.86 万 chunk 柱 | `.schem` | 约 29.3 秒 | 约 175 MiB（工作集约 348 MiB） |
| MCWorld → MCFunction | 同上 | `.mcfunction` | 约 10.79 秒（写真实文件） | 约 154 MiB |
| MCWorld → BDX | Kuudra，`188×175×185`，270.6 万非空气方块 | `.bdx` | 约 1.49 秒 | 约 152 MiB |
| BDX → MCWorld | 同一 Kuudra BDX | 世界目录/`.mcworld` | 约 1.68 秒 | 约 159 MiB |
| Schematic → MCWorld | `519×256×519`，1089 个 chunk | 世界目录/`.mcworld` | 约 3.6 秒 | 约 412 MiB |
| SchemV1 → MCWorld | Flight，`2610×282×2615` | 世界目录 | 约 29 秒 | 约 161 MiB |

MCFunction 编码默认使用 2 个线程；在乌托邦样本上，1/2/3/4/8 个编码线程约为
17.06/9.55/11.31/11.05/11.28 秒。该负载在 2 个线程后受内存带宽限制，因此
线程越多不一定越快。其他 writer 当前主要受源文件解码、压缩或 LevelDB 写入速度
限制，`--threads` 不会自动让所有格式线性加速。

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
