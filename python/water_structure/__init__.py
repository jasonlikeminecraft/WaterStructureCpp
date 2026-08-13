"""Python bindings for WaterStructureCpp.

The PyPI wheel bundles the native Windows DLL and the runtime mapping assets,
so normal users only need ``pip install water-structure``.
"""

from ._binding import Context, Error, StructureInfo, abi_version, version

__all__ = [
    "Context",
    "Error",
    "StructureInfo",
    "abi_version",
    "version",
]
