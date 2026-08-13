"""Small ctypes wrapper for the WaterStructure C ABI.

The wrapper deliberately exposes paths as UTF-8 strings and keeps all C++
objects behind opaque handles. It works with either the shared DLL or a
loadable build copied next to this module.
"""

from __future__ import annotations

import ctypes
import os
from pathlib import Path


class StructureInfo(ctypes.Structure):
    _fields_ = [
        ("format_id", ctypes.c_uint8),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("length", ctypes.c_int32),
        ("offset_x", ctypes.c_int32),
        ("offset_y", ctypes.c_int32),
        ("offset_z", ctypes.c_int32),
        ("non_air_blocks", ctypes.c_uint64),
    ]


def _candidates() -> list[Path]:
    result = []
    configured = os.environ.get("WATER_STRUCTURE_LIBRARY")
    if configured:
        result.append(Path(configured))
    here = Path(__file__).resolve().parent
    result.extend([
        here / "water_structure.dll",
        here / "water_structure_shared.dll",
        here.parent / "water_structure.dll",
        here.parent / "build" / "windows" / "x64" / "release" / "water_structure_shared.dll",
    ])
    return result


def _load() -> ctypes.CDLL:
    for candidate in _candidates():
        if candidate.is_file():
            return ctypes.CDLL(str(candidate))
    raise FileNotFoundError(
        "water_structure shared library not found; set WATER_STRUCTURE_LIBRARY"
    )


_lib = _load()
_lib.ws_context_create.argtypes = [ctypes.c_char_p]
_lib.ws_context_create.restype = ctypes.c_void_p
_lib.ws_abi_version.argtypes = []
_lib.ws_abi_version.restype = ctypes.c_uint32
_lib.ws_context_destroy.argtypes = [ctypes.c_void_p]
_lib.ws_context_destroy.restype = None
_lib.ws_last_error.argtypes = [ctypes.c_void_p]
_lib.ws_last_error.restype = ctypes.c_char_p
_lib.ws_reader_open.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
_lib.ws_reader_open.restype = ctypes.c_void_p
_lib.ws_reader_close.argtypes = [ctypes.c_void_p]
_lib.ws_reader_close.restype = None
_lib.ws_reader_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(StructureInfo)]
_lib.ws_reader_info.restype = ctypes.c_int
_lib.ws_reader_format.argtypes = [ctypes.c_void_p]
_lib.ws_reader_format.restype = ctypes.c_char_p
_lib.ws_convert.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64]
_lib.ws_convert.restype = ctypes.c_int
_lib.ws_to_world.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32]
_lib.ws_to_world.restype = ctypes.c_int

if _lib.ws_abi_version() != 1:
    raise RuntimeError("unsupported WaterStructure C ABI version")


class Error(RuntimeError):
    pass


class Context:
    def __init__(self, assets_directory: str | os.PathLike[str] | None = None):
        if assets_directory is None:
            bundled = Path(__file__).resolve().parent / "assets"
            if bundled.is_dir():
                assets_directory = bundled
        encoded = None if assets_directory is None else os.fsencode(assets_directory)
        self._handle = _lib.ws_context_create(encoded)
        if not self._handle:
            raise Error("failed to create WaterStructure context")

    def __del__(self):
        handle = getattr(self, "_handle", None)
        if handle:
            _lib.ws_context_destroy(handle)
            self._handle = None

    def _error(self) -> str:
        value = _lib.ws_last_error(self._handle)
        return value.decode("utf-8", "replace") if value else "unknown WaterStructure error"

    def inspect(self, path: str | os.PathLike[str], streaming_world_import: bool = False) -> StructureInfo:
        reader = _lib.ws_reader_open(self._handle, os.fsencode(path), int(streaming_world_import))
        if not reader:
            raise Error(self._error())
        try:
            info = StructureInfo()
            if not _lib.ws_reader_info(reader, ctypes.byref(info)):
                raise Error(self._error())
            return info
        finally:
            _lib.ws_reader_close(reader)

    def format(self, path: str | os.PathLike[str], streaming_world_import: bool = False) -> str:
        reader = _lib.ws_reader_open(self._handle, os.fsencode(path), int(streaming_world_import))
        if not reader:
            raise Error(self._error())
        try:
            return _lib.ws_reader_format(reader).decode("utf-8")
        finally:
            _lib.ws_reader_close(reader)

    def convert(self, input_path: str | os.PathLike[str], target_format: str,
                output_path: str | os.PathLike[str], threads: int = 0) -> None:
        if threads < 0:
            raise ValueError("threads must be >= 0")
        ok = _lib.ws_convert(
            self._handle, os.fsencode(input_path), target_format.encode("utf-8"),
            os.fsencode(output_path), threads)
        if not ok:
            raise Error(self._error())

    def to_world(self, input_path: str | os.PathLike[str], world_path: str | os.PathLike[str],
                 start: tuple[int, int, int] = (0, -4, 0)) -> None:
        ok = _lib.ws_to_world(
            self._handle, os.fsencode(input_path), os.fsencode(world_path),
            int(start[0]), int(start[1]), int(start[2]))
        if not ok:
            raise Error(self._error())


__all__ = ["Context", "Error", "StructureInfo"]
