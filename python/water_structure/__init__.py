"""Cross-platform Python bindings for WaterStructureCpp.

Platform wheels bundle the native library and runtime mapping assets, so normal
users only need ``pip install water-structure``.
"""

from ._binding import Context, Error, StructureInfo, abi_version, version

__all__ = [
    "Context",
    "Error",
    "StructureInfo",
    "abi_version",
    "version",
]
