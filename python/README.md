# water-structure

Python bindings for the streaming Minecraft structure conversion library
[WaterStructureCpp](https://github.com/jasonlikeminecraft/WaterStructureCpp).

The Windows x64 wheel bundles the native DLL and runtime mapping assets. It does
not require a separate C++ installation.

```python
from water_structure import Context

with Context() as converter:
    info = converter.inspect("building.schem")
    print(info.width, info.height, info.length, info.non_air_blocks)
    converter.convert("building.schem", "BDX", "building.bdx")
    converter.to_world("building.bdx", "world.mcworld")
```

Supported writer names include `Schematic`, `SchemV1`, `SchemV2`, `Litematic`,
`MCStructure`, `BDX`, `AxiomBP`, `IBImport`, `FuHongV4`, `FuHongV5`, and
`MCFunction`. See the project README for the complete reader capability table.

This initial PyPI release supports 64-bit Windows and Python 3.9 or newer.
