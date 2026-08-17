# water-structure Python 教程

`water-structure` 是 [WaterStructureCpp](https://github.com/jasonlikeminecraft/WaterStructureCpp)
的 Python 绑定，用于识别、检查和转换 Minecraft Java/Bedrock 结构文件，以及把结构
流式写入 Bedrock 世界。平台 wheel 已包含原生动态库与方块映射资产，正常安装时不需要
另外部署 C++ 库。

> 当前项目处于 Alpha 阶段。转换真实世界前，请先备份目标世界。不要在 Minecraft
> 正在打开世界时写入同一个世界目录或 `.mcworld` 文件。

## 1. 安装

建议为每个项目创建独立虚拟环境。

### Windows PowerShell

```powershell
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install water-structure
```

### Linux / macOS

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install water-structure
```

### Android Termux（arm64-v8a）

```bash
pkg update
pkg install python
python -m pip install --upgrade pip
python -m pip install water-structure
```

访问手机共享存储前执行一次 `termux-setup-storage`，之后可以使用
`~/storage/downloads` 等路径。如果 pip 报 wheel 平台不兼容，先检查当前 pip
是否识别 Android 标签：

```bash
python -m pip debug --verbose | grep android
```

当前 Termux wheel 目标为 Android API 24+、`arm64-v8a`。

### 验证安装

```bash
python -c "import water_structure as ws; print(ws.version(), ws.abi_version())"
```

`version()` 是捆绑的原生库版本，`abi_version()` 是 C ABI 版本。当前 ABI 为 `1`；
Python 包和原生库 ABI 不匹配时会直接导入失败。

## 2. 四个核心接口

| 接口 | 用途 | 是否产生输出文件 |
| --- | --- | --- |
| `Context.format(path)` | 自动识别输入格式 | 否 |
| `Context.inspect(path)` | 获取尺寸、偏移和非空气数量 | 否 |
| `Context.convert(input, format, output)` | 转为一个已实现 writer 的结构格式 | 是 |
| `Context.to_world(input, world)` | 写入 Bedrock 目录世界或 `.mcworld` | 是 |

输入格式始终自动识别；输出格式必须明确指定。扩展名不会替你决定 `SchemV1` 还是
`SchemV2`。

所有操作都通过 `Context` 使用。它会加载 runtime registry 和升级映射，因此同一批
连续任务应复用一个 Context：

```python
from water_structure import Context

with Context() as ctx:
    print(ctx.format("building.schem"))
    print(ctx.inspect("building.schem"))
# 离开 with 后自动释放原生资源
```

不要让多个 Python 线程同时调用同一个 Context。需要并发隔离时，每个线程或进程
创建自己的 Context；大型转换通常受磁盘与内存带宽限制，并发任务过多反而更慢。

## 3. 第一次转换

下面示例自动识别输入，然后输出指定版本的 `.schem`：

```python
from pathlib import Path
from water_structure import Context

source = Path(r"D:\import\building.mcworld")
output = Path(r"D:\import\building-v2.schem")

with Context() as ctx:
    detected = ctx.format(source, streaming_world_import=True)
    info = ctx.inspect(source, streaming_world_import=True)

    print("格式：", detected)
    print("尺寸：", info.width, info.height, info.length)
    print("偏移：", info.offset_x, info.offset_y, info.offset_z)
    print("非空气：", info.non_air_blocks)

    ctx.convert(source, "SchemV2", output, threads=0)

print("已生成：", output)
```

路径参数接受 `str`、`pathlib.Path` 和其他 `os.PathLike`；中文路径无需手工编码。

## 4. 格式识别与结构信息

### `format()`

```python
with Context() as ctx:
    print(ctx.format("house.schem"))
    print(ctx.format("world.mcworld", streaming_world_import=True))
```

返回 registry 格式名，例如 `SchemV1`、`SchemV2`、`BDX` 或 `MCWorld`。同扩展名
格式会按兼容顺序读取文件头和内容试探，而不是只看扩展名。

### `inspect()`

```python
info = ctx.inspect("house.schem")

print(info.format_id)       # 内部 StructureId 数值
print(info.width)           # X 方向尺寸
print(info.height)          # Y 方向尺寸
print(info.length)          # Z 方向尺寸
print(info.offset_x)
print(info.offset_y)
print(info.offset_z)
print(info.non_air_blocks)
```

返回值为不可变的 `StructureInfo` 命名元组：

```python
StructureInfo(
    format_id: int,
    width: int,
    height: int,
    length: int,
    offset_x: int,
    offset_y: int,
    offset_z: int,
    non_air_blocks: int,
)
```

对目录世界或 `.mcworld` 调用 `format()`/`inspect()` 时，可以设置
`streaming_world_import=True`，避免为了读取元信息而构造完整内存结构。统计非空气数量
仍然需要扫描相关世界数据，所以“大文件低内存”不等于“无需读取”。该参数只属于
`format()` 和 `inspect()`；`convert()` 会由 C++ 转换器选择对应的流式路径。

## 5. `convert()`：结构格式互转

```python
ctx.convert(input_path, target_format, output_path, threads=0)
```

- `input_path`：自动识别的源结构文件、目录世界或 `.mcworld`。
- `target_format`：目标 writer 的 registry 名称；建议使用下表中的标准写法。
- `output_path`：目标文件路径，父目录应已存在。
- `threads`：编码 worker 数，必须大于等于 0。

当前已验证的 writer：

| `target_format` | 常用扩展名 | 说明 |
| --- | --- | --- |
| `Schematic` | `.schematic` | 经典 Java legacy 格式 |
| `SchemV1` | `.schem` | Sponge Schematic V1 |
| `SchemV2` | `.schem` | Sponge Schematic V2 |
| `Litematic` | `.litematic` | Litematica |
| `MCStructure` | `.mcstructure` | Bedrock Structure Block 格式 |
| `BDX` | `.bdx` | Brotli 压缩命令流 |
| `AxiomBP` | `.bp` | Axiom Blueprint |
| `MCFunction` | `.mcfunction` / `.txt` | Bedrock `fill`/`setblock` 命令流 |
| `IBImport` | `.ibi` | 命令段流式输出，支持 `fill` 合并 |
| `FuHongV4` | `.json` | FuHong V4 JSON |
| `FuHongV5` | `.fhbuild` | FuHong V5 二进制格式 |

有 reader 不代表有 writer。尝试输出到只有 reader 的格式会抛出 capability error，
不会猜测或伪造协议。

### `.schem` 必须指定 V1/V2

```python
ctx.convert("input.bdx", "SchemV1", "output-v1.schem")
ctx.convert("input.bdx", "SchemV2", "output-v2.schem")
```

不能把目标格式写成模糊的 `Schem`。

### 转为 BDX

```python
ctx.convert("building.mcworld", "BDX", "building.bdx", threads=2)
```

BDX 读取与写入使用流式命令处理；大型稀疏世界仍可能花费较长时间进行范围扫描、
Brotli 编码和世界数据读取。

### 转为 IBImport

```python
ctx.convert("building.schem", "IBImport", "building.ibi", threads=0)
```

IBImport writer 会把连续同类方块尽量合并为 `fill`，其命令坐标使用相对坐标。

### 转为 MCFunction

```python
ctx.convert("building.mcworld", "MCFunction", "building.mcfunction")
```

生成的是 Bedrock 方块状态命令。`fill` 和 `setblock` 坐标使用 `~x ~y ~z`，执行
函数时以执行位置为结构锚点。MCFunction 没有统一的 Bedrock 方块实体 NBT 协议，
因此 writer 会跳过方块实体 NBT，而不是让整个转换失败。

### `threads` 应如何选择

```python
ctx.convert(source, "MCFunction", output, threads=0)  # 推荐：自动
ctx.convert(source, "MCFunction", output, threads=1)  # 串行、便于基准对比
ctx.convert(source, "MCFunction", output, threads=2)  # 显式两个编码线程
```

- `threads=0`：自动选择；极小任务使用 1 个 worker，其他任务默认最多 2 个。
- `threads=1`：禁用 writer 编码并行。
- `threads=N`：显式设置编码 worker 数。

线程数只控制支持并行编码的阶段，不会让所有解析器和压缩步骤自动变成 N 线程。
大型转换常受磁盘、LevelDB、压缩或内存带宽限制，超过 2 个编码线程不保证更快。

## 6. `to_world()`：写入 Bedrock 世界

```python
ctx.to_world(input_path, world_path, start=(0, -4, 0))
```

`start` 是 `(chunk_x, subchunk_y, chunk_z)`，不是方块坐标。默认 Y 为 `-4`，对应
方块高度 `-64`；每个 chunk/subchunk 轴单位对应 16 个方块。

### 写入目录世界

```python
from pathlib import Path
from water_structure import Context

target = Path("output_world")
with Context() as ctx:
    ctx.to_world("building.schem", target)
```

目标不存在时会创建目录世界。目标是已有世界目录时会更新其 LevelDB 数据；请先备份，
并确保 Minecraft 没有占用该世界。

### 写入 `.mcworld`

`.mcworld` 输出需要一个已经存在的模板文件。建议复制空白世界，而不是直接覆盖唯一
原件：

```python
from pathlib import Path
import shutil
from water_structure import Context

template = Path("empty-template.mcworld")
target = Path("building.mcworld")
shutil.copy2(template, target)

with Context() as ctx:
    ctx.to_world("building.schem", target, start=(0, -4, 0))
```

如果目标路径不存在，即使文件名以 `.mcworld` 结尾，当前实现也会把它当作目录世界创建。
要得到压缩归档，必须先准备真实存在的 `.mcworld` 模板。

## 7. 批量转换

下面示例复用 Context，逐个转换目录中的文件，并让单个失败不影响后续任务：

```python
from pathlib import Path
from water_structure import Context, Error

source_dir = Path("inputs")
output_dir = Path("outputs")
output_dir.mkdir(parents=True, exist_ok=True)
sources = sorted(source_dir.glob("*.bdx"))

with Context() as ctx:
    for index, source in enumerate(sources, start=1):
        output = output_dir / f"{source.stem}.schem"
        print(f"[{index}/{len(sources)}] {source.name} -> {output.name}")
        if output.exists():
            print("  跳过：输出已存在")
            continue
        try:
            ctx.convert(source, "SchemV2", output, threads=0)
        except Error as exc:
            print(f"  失败：{exc}")
        else:
            print("  完成")
```

## 8. 进度显示

`convert()` 和 `to_world()` 都支持可选的 `progress` 回调。回调接收一个 `Progress`
命名元组：

```python
from water_structure import Context, Progress

def show_progress(value: Progress) -> None:
    if value.percent is None:
        print(f"[{value.stage}] {value.elapsed:.1f}s", flush=True)
        return
    eta = "n/a" if value.eta is None else f"{value.eta:.1f}s"
    print(
        f"[{value.stage}] {value.percent:5.1f}% "
        f"{value.completed}/{value.total} ETA {eta}",
        flush=True,
    )

with Context() as ctx:
    ctx.convert(
        "building.mcworld",
        "SchemV2",
        "building.schem",
        progress=show_progress,
    )
```

`Progress` 字段：

| 字段 | 含义 |
| --- | --- |
| `stage` | `open`、`read`、`encode`、`write` 或 `finalize` |
| `completed` | 当前阶段已完成的工作单位 |
| `total` | 当前阶段总工作单位；未知时为 `None` |
| `percent` | 当前阶段百分比；未知时为 `None` |
| `elapsed` | 当前阶段已用秒数 |
| `eta` | 当前阶段预计剩余秒数；无法计算时为 `None` |
| `indeterminate` | 是否应该显示 spinner 而不是百分比 |

转换进度按 chunk/batch 节点报告，不按方块回调。原生层至少间隔约 100ms 才通知一次，
因此正常回调不会显著影响性能；回调中不要执行耗时工作，也不要重入同一个 Context。
压缩和最终封装可能没有可靠总量，会显示 `percent=None`，这不是转换卡住。

回调异常无法从 ctypes 原生线程直接抛回，因此绑定会先让转换结束，再重新抛出保存的
异常。异常发生后输出文件可能已经生成，程序应自行决定是否删除它。

如果要更新 GUI，请在后台线程调用 `convert()`，回调只向线程安全队列写入快照；不要在
回调中直接操作大多数 GUI toolkit 的控件。

## 9. 错误处理

原生转换错误统一抛出 `water_structure.Error`，它继承自 `RuntimeError`：

```python
from water_structure import Context, Error

try:
    with Context() as ctx:
        ctx.convert("broken.bdx", "SchemV2", "output.schem")
except Error as exc:
    print("转换失败：", exc)
```

Python 参数错误仍使用标准异常。例如 `threads=-1` 会抛 `ValueError`；关闭 Context
后继续调用会抛 `Error("WaterStructure context is closed")`。

批处理程序应记录源路径、目标格式和完整消息。不要捕获所有异常后静默忽略，否则会
隐藏磁盘写满、格式截断或方块状态不兼容等问题。

## 10. 自定义资产与源码树调试

正常 wheel 已捆绑资产，不要传 `assets_directory`。只有开发或验证自定义 runtime
映射时才显式指定：

```python
with Context(assets_directory="path/to/assets") as ctx:
    ...
```

源码树调试时，可以让 Python 加载本地构建的动态库：

```powershell
# Windows
$env:WATER_STRUCTURE_LIBRARY = "D:\Projects\WaterStructureCpp\build\windows\x64\release\water_structure_shared.dll"
python your_script.py
```

```bash
# Linux / Termux；macOS 使用 .dylib
export WATER_STRUCTURE_LIBRARY=/absolute/path/to/libwater_structure_shared.so
python your_script.py
```

动态库架构、C ABI 和 Python 包内资产必须匹配。

## 11. 常见问题

### VS Code 显示“无法解析导入 water_structure”

```bash
python -m pip show water-structure
python -c "import sys; print(sys.executable)"
```

然后在 VS Code 执行 `Python: Select Interpreter`，选择同一个解释器。优先使用
`python -m pip`，不要用裸 `pip` 猜测安装位置。

### `unable to load WaterStructure native library`

检查 wheel 是否匹配操作系统和 CPU 架构、是否从正确的 Python 环境运行，以及
wheel 内 `.dll`/`.so`/`.dylib` 是否被安全软件删除。源码调试时还要检查
`WATER_STRUCTURE_LIBRARY`。

### `No matching distribution found`

PyPI 没有与当前平台标签匹配的 wheel。更新 pip 后重试；仍失败则需要使用对应平台的
GitHub Actions artifact 或从源码构建。

### 为什么 `.schem` 不能只写 `Schem`

V1 和 V2 共用扩展名但协议不同。输出必须显式选择 `SchemV1` 或 `SchemV2`。

### 为什么提高线程数没有加速

`threads` 只影响支持并行的 writer 编码阶段。读取世界、Brotli、NBT、LevelDB 或
磁盘写入可能才是瓶颈；线程增加后也可能受内存带宽和调度开销影响。

## 12. API 速查

```python
from water_structure import Context, Error, Progress, StructureInfo, version, abi_version

version() -> str
abi_version() -> int

Context(assets_directory=None)
Context.close() -> None
Context.format(path, *, streaming_world_import=False) -> str
Context.inspect(path, *, streaming_world_import=False) -> StructureInfo
Context.convert(input_path, target_format, output_path, *, threads=0, progress=None) -> None
Context.to_world(input_path, world_path, *, start=(0, -4, 0), progress=None) -> None
```

包内提供 `py.typed` 和 `.pyi`，类型检查器可以识别公开 API。

## 13. 从源码构建 wheel

先安装 xmake、C++ 工具链和 Python 构建工具，然后在仓库根目录执行：

```bash
python -m pip install build twine
python python/build_wheel.py
python -m twine check dist/python/*.whl
```

Windows 也可使用：

```powershell
.\python\build_wheel.ps1
```

构建脚本不指定线程数，由 xmake 使用默认并行度。生成的 wheel 在 `dist/python/`；
先在干净虚拟环境中安装并运行 smoke test，再上传 PyPI。
