# water-structure

Python bindings for the streaming Minecraft structure conversion library
[WaterStructureCpp](https://github.com/jasonlikeminecraft/WaterStructureCpp).

The Windows x64 wheel bundles the native DLL and runtime mapping assets, so it
does not require a separate C++ installation.

## 安装

```powershell
python -m pip install water-structure
```

要求：Windows x64、Python 3.9+。源码树调试时可以设置
`WATER_STRUCTURE_LIBRARY` 指向本地构建的
`build/windows/x64/release/water_structure_shared.dll`，无需安装 wheel。

## 快速开始

```python
from water_structure import Context

with Context() as ctx:
    info = ctx.inspect("building.schem")
    print(info.width, info.height, info.length, info.non_air_blocks)

    ctx.convert("building.schem", "BDX", "building.bdx", threads=0)
    ctx.to_world("building.bdx", "world_dir")
```

## API 教学（按功能逐个说明）

### 0. 版本信息

```python
from water_structure import version, abi_version

print(version())      # '0.1.1' —— 捆绑的原生库版本
print(abi_version())  # 1 —— C ABI 版本（不兼容时会直接导入失败）
```

### 1. Context —— 所有操作都挂在它上面

```python
from water_structure import Context

with Context() as ctx:        # 推荐：退出 with 自动释放原生资源
    ...                       # 也可手动 ctx.close()，可重复调用
```

`Context(assets_directory=None)` 默认使用 wheel 内置的方块映射资产；源码树
调试时可传入自定义资产目录。

### 2. format(path) —— 自动识别格式

```python
ctx.format("building.schem")   # -> 'SchemV1'
ctx.format("world.mcworld")    # -> 'MCWorld'
```

对 37 种已注册格式做文件头/内容探测，返回格式名。返回的名字可直接作为
`convert` 的目标格式名。

### 3. inspect(path) —— 读取结构信息（不转换）

```python
info = ctx.inspect("building.schem")
# 返回 StructureInfo（命名元组）：
info.format_id       # int，StructureId 枚举值
info.width           # 宽（方块）
info.height          # 高（方块）
info.length          # 长（方块）
info.offset_x/y/z    # 结构原点偏移
info.non_air_blocks  # 非空气方块总数
```

### 4. convert(input, target_format, output, *, threads=0) —— 任意格式互转

**输入**：37 种格式中除 SIBI 外的全部可读格式（MCWorld、SchemV1/V2、
Schematic、Litematic、MCStructure、BDX、MCFunction、AxiomBP、IBImport、
FuHong、各厂商 JSON 等），自动识别。

**输出**（11 个已验证 writer 名）：

| 目标格式名 | 扩展名 | 说明 |
| --- | --- | --- |
| `Schematic` | `.schematic` | 经典 Java 格式 |
| `SchemV1` | `.schem` | 需显式指定版本 |
| `SchemV2` | `.schem` | 需显式指定版本 |
| `Litematic` | `.litematic` | |
| `MCStructure` | `.mcstructure` | |
| `BDX` | `.bdx` | Brotli 压缩 |
| `AxiomBP` | `.bp` | |
| `MCFunction` | `.mcfunction` / `.txt` | Bedrock 命令流 |
| `IBImport` | `.ibi` | |
| `FuHongV4` | `.json` | |
| `FuHongV5` | `.fhbuild` | |

```python
ctx.convert(in_file, "SchemV1", "out.schem")          # .schem 有 V1/V2，必须指定
ctx.convert(in_file, "SchemV2", "out_v2.schem")
ctx.convert(in_file, "BDX", "out.bdx", threads=2)
ctx.convert(in_file, "MCStructure", "out.mcstructure")
ctx.convert(in_file, "MCFunction", "out.mcfunction")
```

**threads 参数**：并行编码阶段的 worker 数。

- `threads=0`（默认）：**自动选择**——输入极小（只有 1 个批次任务）时用
  1 个 worker 避免线程池开销；否则取 `min(CPU 核心数, 2)`。并行编码在
  2 个线程后受内存带宽限制（乌托邦样本实测 1/2/3/4/8 线程约为
  17.06/9.55/11.31/11.05/11.28 秒），因此默认上限是 2。
- `threads=1`：完全串行，适合可复现测量。
- `threads=N`：显式指定；对受内存带宽限制的大型转换，更大的 N 不一定更快。

### 5. to_world(input, world_path, *, start=(0, -4, 0)) —— 写回 Bedrock 世界

```python
ctx.to_world(in_file, "world_dir")                      # 目录世界（新建即可）
ctx.to_world(in_file, "world.mcworld")                  # 已有 .mcworld 模板
ctx.to_world(in_file, "world.mcworld", start=(3, -4, 5))  # 自定义起始子区块
```

注意两点：

- `start` 是**子区块坐标 `(x, 子区块Y, z)`**，不是方块坐标。默认 `(0,-4,0)`
  对应方块 Y=-64 那一层；子区块 Y 每 ±1 对应 16 个方块。
- 目标路径**不存在**时，无论扩展名是什么都会创建"目录世界"（内部是
  `db/`、`level.dat`）。要得到 `.mcworld` 压缩包，目标文件必须**已存在**
  （可先复制一份空模板），写入完成后会自动重新打包。

### 6. 错误处理

所有失败都抛 `water_structure.Error`（继承 `RuntimeError`），消息为原生
中文错误文本：

```python
from water_structure import Error

try:
    ctx.inspect("D:/不存在.mcstructure")
except Error as e:
    print(e)   # 文件不存在: D:/不存在.mcstructure
```

## 常见问题

- **VS Code 报"无法解析导入 water_structure"**：Pylance 选中的 Python 解释器
  与安装 wheel 的解释器不一致。`Ctrl+Shift+P` → `Python: Select Interpreter`
  选择与 `pip show water-structure` 输出一致的解释器。
- **导入时报"unable to load ... DLL"**：确认是 Windows x64 平台；源码树使用
  需要设置 `WATER_STRUCTURE_LIBRARY`。
- **支持的输入格式**：除 SIBI（规范缺失，两端均未实现）外的 36 种格式全部
  可读。详见项目根目录 README 的格式能力表。

## 从源码构建 wheel

```powershell
python -m pip install build twine
.\python\build_wheel.ps1          # 构建 wheel（会自动编译原生 DLL）
python -m twine check .\dist\python\*.whl
```
