"""Package facade for the WaterStructure ctypes API.

The implementation remains in the top-level module so source-tree usage and
wheel usage share exactly the same ABI loader and error handling.
"""

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

_implementation_path = Path(__file__).resolve().parent.parent / "water_structure.py"
_spec = spec_from_file_location("_water_structure_impl", _implementation_path)
if _spec is None or _spec.loader is None:
    raise ImportError(f"cannot load {_implementation_path}")
_implementation = module_from_spec(_spec)
_spec.loader.exec_module(_implementation)

Context = _implementation.Context
Error = _implementation.Error
StructureInfo = _implementation.StructureInfo

__all__ = ["Context", "Error", "StructureInfo"]
