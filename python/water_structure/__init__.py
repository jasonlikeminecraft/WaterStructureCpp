"""Cross-platform Python bindings for WaterStructureCpp.

Platform wheels bundle the native library and runtime mapping assets, so normal
users only need ``pip install water-structure``.
"""

from ._binding import (
    Context,
    Error,
    FormatCapabilities,
    Progress,
    StructureInfo,
    abi_version,
    formats,
    version,
)

__all__ = [
    "Context",
    "Error",
    "FormatCapabilities",
    "Progress",
    "StructureInfo",
    "abi_version",
    "formats",
    "version",
]
