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

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE)。使用或分发时也请
遵守上游项目的许可证及署名要求。
