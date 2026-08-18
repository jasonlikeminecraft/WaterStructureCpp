from __future__ import annotations

import ctypes
import os
import platform
import time
from pathlib import Path
from typing import Callable, List, NamedTuple, Optional, Tuple, Union

PathValue = Union[str, os.PathLike[str]]


class Error(RuntimeError):
    """Raised when the native WaterStructure library reports an error."""


class StructureInfo(NamedTuple):
    format_id: int
    width: int
    height: int
    length: int
    offset_x: int
    offset_y: int
    offset_z: int
    non_air_blocks: int


class Progress(NamedTuple):
    """A throttled progress snapshot emitted by a native conversion."""

    stage: str
    completed: int
    total: Optional[int]
    percent: Optional[float]
    elapsed: float
    eta: Optional[float]
    indeterminate: bool


class _CStructureInfo(ctypes.Structure):
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


def _library_candidates() -> List[Path]:
    configured = os.environ.get("WATER_STRUCTURE_LIBRARY")
    package = Path(__file__).resolve().parent
    system = platform.system()
    if system == "Windows":
        names = ["water_structure_shared.dll"]
        names.extend(path.name for path in package.glob("_water_structure*.pyd"))
    elif system == "Linux":
        names = ["libwater_structure_shared.so", "water_structure_shared.so"]
    elif system == "Darwin":
        names = ["libwater_structure_shared.dylib", "water_structure_shared.dylib"]
    else:
        names = []
    candidates = [package / name for name in names]
    if configured:
        candidates.insert(0, Path(configured).expanduser())
    return candidates


def _load_library() -> ctypes.CDLL:
    system = platform.system()
    if system not in {"Windows", "Linux", "Darwin"}:
        raise ImportError(f"water-structure does not support {system or 'this platform'}")
    errors: List[str] = []
    for candidate in _library_candidates():
        if not candidate.is_file():
            continue
        try:
            return ctypes.CDLL(str(candidate))
        except OSError as exc:
            errors.append(f"{candidate}: {exc}")
    detail = "; ".join(errors) if errors else "bundled native library is missing"
    raise ImportError(
        "unable to load WaterStructure native library (" + detail + "). "
        "Reinstall the wheel or set WATER_STRUCTURE_LIBRARY to a compatible library."
    )


_lib = _load_library()
_lib.ws_version.argtypes = []
_lib.ws_version.restype = ctypes.c_char_p
_lib.ws_abi_version.argtypes = []
_lib.ws_abi_version.restype = ctypes.c_uint32
_lib.ws_context_create.argtypes = [ctypes.c_char_p]
_lib.ws_context_create.restype = ctypes.c_void_p
_lib.ws_context_destroy.argtypes = [ctypes.c_void_p]
_lib.ws_context_destroy.restype = None
_lib.ws_last_error.argtypes = [ctypes.c_void_p]
_lib.ws_last_error.restype = ctypes.c_char_p
_lib.ws_reader_open.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
_lib.ws_reader_open.restype = ctypes.c_void_p
_lib.ws_reader_close.argtypes = [ctypes.c_void_p]
_lib.ws_reader_close.restype = None
_lib.ws_reader_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CStructureInfo)]
_lib.ws_reader_info.restype = ctypes.c_int
_lib.ws_reader_format.argtypes = [ctypes.c_void_p]
_lib.ws_reader_format.restype = ctypes.c_char_p
_lib.ws_convert.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_uint64,
]
_lib.ws_convert.restype = ctypes.c_int
_ws_convert_ex = getattr(_lib, "ws_convert_ex", None)
if _ws_convert_ex is not None:
    _ws_convert_ex.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_int,
    ]
    _ws_convert_ex.restype = ctypes.c_int
_ProgressCallback = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint64, ctypes.c_uint64
)
_ws_convert_with_progress = getattr(_lib, "ws_convert_with_progress", None)
if _ws_convert_with_progress is not None:
    _ws_convert_with_progress.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        _ProgressCallback,
        ctypes.c_void_p,
    ]
    _ws_convert_with_progress.restype = ctypes.c_int
_ws_convert_with_progress_ex = getattr(_lib, "ws_convert_with_progress_ex", None)
if _ws_convert_with_progress_ex is not None:
    _ws_convert_with_progress_ex.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_int,
        _ProgressCallback,
        ctypes.c_void_p,
    ]
    _ws_convert_with_progress_ex.restype = ctypes.c_int
_lib.ws_to_world.argtypes = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_int32,
    ctypes.c_int32,
    ctypes.c_int32,
]
_lib.ws_to_world.restype = ctypes.c_int
_ws_to_world_with_progress = getattr(_lib, "ws_to_world_with_progress", None)
if _ws_to_world_with_progress is not None:
    _ws_to_world_with_progress.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int32,
        ctypes.c_int32,
        ctypes.c_int32,
        _ProgressCallback,
        ctypes.c_void_p,
    ]
    _ws_to_world_with_progress.restype = ctypes.c_int

if _lib.ws_abi_version() != 1:
    raise ImportError(f"unsupported WaterStructure C ABI: {_lib.ws_abi_version()}")


def version() -> str:
    """Return the bundled native library version."""
    value = _lib.ws_version()
    return value.decode("utf-8", "replace") if value else "unknown"


def abi_version() -> int:
    """Return the native C ABI version."""
    return int(_lib.ws_abi_version())


class Context:
    """Owns the runtime registry used for inspect and conversion operations."""

    def __init__(self, assets_directory: Optional[PathValue] = None):
        if assets_directory is None:
            assets_directory = Path(__file__).resolve().parent / "assets"
        self._handle = _lib.ws_context_create(os.fsencode(assets_directory))
        if not self._handle:
            raise Error("failed to create WaterStructure context")

    def close(self) -> None:
        """Release the native context. Calling close repeatedly is safe."""
        handle = getattr(self, "_handle", None)
        if handle:
            _lib.ws_context_destroy(handle)
            self._handle = None

    def __enter__(self) -> Context:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()

    def _require_open(self) -> ctypes.c_void_p:
        if not self._handle:
            raise Error("WaterStructure context is closed")
        return self._handle

    def _error(self) -> str:
        value = _lib.ws_last_error(self._require_open())
        return value.decode("utf-8", "replace") if value else "unknown error"

    @staticmethod
    def _progress_callback(
        callback: Callable[[Progress], object],
    ) -> tuple[_ProgressCallback, list[BaseException]]:
        stages = ("open", "read", "encode", "write", "finalize")
        errors: list[BaseException] = []
        state = {"stage": None, "started": time.monotonic()}

        def native_callback(
            _user_data: int,
            stage_code: int,
            completed: int,
            total: int,
        ) -> None:
            now = time.monotonic()
            stage = stages[stage_code] if stage_code < len(stages) else "unknown"
            if state["stage"] != stage:
                state["stage"] = stage
                state["started"] = now
            elapsed = now - state["started"]
            done = int(completed)
            total_value = int(total) if total else None
            percent = None
            eta = None
            if total_value is not None:
                percent = min(100.0, done * 100.0 / total_value)
                if done > 0 and done < total_value:
                    eta = elapsed * (total_value - done) / done
                elif done >= total_value:
                    eta = 0.0
            event = Progress(
                stage,
                done,
                total_value,
                percent,
                elapsed,
                eta,
                total_value is None,
            )
            try:
                callback(event)
            except BaseException as error:  # ctypes cannot propagate callback exceptions
                errors.append(error)

        return _ProgressCallback(native_callback), errors

    def inspect(
        self,
        path: PathValue,
        *,
        streaming_world_import: bool = False,
    ) -> StructureInfo:
        """Inspect a structure without converting it."""
        reader = _lib.ws_reader_open(
            self._require_open(), os.fsencode(path), int(streaming_world_import)
        )
        if not reader:
            raise Error(self._error())
        try:
            raw = _CStructureInfo()
            if not _lib.ws_reader_info(reader, ctypes.byref(raw)):
                raise Error(self._error())
            return StructureInfo(*(getattr(raw, field) for field, _ in raw._fields_))
        finally:
            _lib.ws_reader_close(reader)

    def format(
        self,
        path: PathValue,
        *,
        streaming_world_import: bool = False,
    ) -> str:
        """Return the detected format name."""
        reader = _lib.ws_reader_open(
            self._require_open(), os.fsencode(path), int(streaming_world_import)
        )
        if not reader:
            raise Error(self._error())
        try:
            value = _lib.ws_reader_format(reader)
            if not value:
                raise Error(self._error())
            return value.decode("utf-8", "replace")
        finally:
            _lib.ws_reader_close(reader)

    def convert(
        self,
        input_path: PathValue,
        target_format: str,
        output_path: PathValue,
        *,
        threads: int = 0,
        clear_air: bool = True,
        progress: Optional[Callable[[Progress], object]] = None,
    ) -> None:
        """Convert a structure to a supported writer format.

        ``target_format`` is a registered writer name (for example
        ``SchemV1``, ``SchemV2``, ``BDX``, ``MCStructure``, ``MCFunction``,
        ``Schematic``, ``Litematic``, ``AxiomBP``, ``IBImport``,
        ``FuHongV4`` or ``FuHongV5``).

        ``threads`` selects the worker count for parallel encoding stages.
        Pass ``0`` (the default) to let the library choose automatically:
        a single worker for tiny inputs, otherwise ``min(CPU cores, 2)``
        (parallel encoding beyond 2 threads is memory-bandwidth limited).

        ``clear_air`` controls MCFunction's destination reset. The default
        clears the complete structure bounds; set it to ``False`` to emit
        only non-air placement commands and preserve existing blocks.
        """
        if threads < 0:
            raise ValueError("threads must be >= 0")
        handle = self._require_open()
        if progress is None:
            args = (
                handle,
                os.fsencode(input_path),
                target_format.encode("utf-8"),
                os.fsencode(output_path),
                threads,
            )
            if clear_air:
                converted = _lib.ws_convert(*args)
            elif _ws_convert_ex is None:
                raise Error("native library does not support clear_air")
            else:
                converted = _ws_convert_ex(*args, 0)
            callback_errors: list[BaseException] = []
        else:
            if not callable(progress):
                raise TypeError("progress must be callable")
            if _ws_convert_with_progress is None:
                raise Error("native library does not provide progress callbacks")
            callback, callback_errors = self._progress_callback(progress)
            args = (
                handle,
                os.fsencode(input_path),
                target_format.encode("utf-8"),
                os.fsencode(output_path),
                threads,
            )
            if clear_air:
                converted = _ws_convert_with_progress(
                    *args, callback, None,
                )
            elif _ws_convert_with_progress_ex is None:
                raise Error("native library does not support clear_air")
            else:
                converted = _ws_convert_with_progress_ex(
                    *args, 0, callback, None,
                )
        if callback_errors:
            raise callback_errors[0]
        if not converted:
            raise Error(self._error())

    def to_world(
        self,
        input_path: PathValue,
        world_path: PathValue,
        *,
        start: Tuple[int, int, int] = (0, -4, 0),
        progress: Optional[Callable[[Progress], object]] = None,
    ) -> None:
        """Stream a structure into a world directory or .mcworld archive."""
        if len(start) != 3:
            raise ValueError("start must contain x, subchunk-y, and z")
        handle = self._require_open()
        if progress is None:
            converted = _lib.ws_to_world(
                handle,
                os.fsencode(input_path),
                os.fsencode(world_path),
                int(start[0]),
                int(start[1]),
                int(start[2]),
            )
            callback_errors: list[BaseException] = []
        else:
            if not callable(progress):
                raise TypeError("progress must be callable")
            if _ws_to_world_with_progress is None:
                raise Error("native library does not provide progress callbacks")
            callback, callback_errors = self._progress_callback(progress)
            converted = _ws_to_world_with_progress(
                handle,
                os.fsencode(input_path),
                os.fsencode(world_path),
                int(start[0]),
                int(start[1]),
                int(start[2]),
                callback,
                None,
            )
        if callback_errors:
            raise callback_errors[0]
        if not converted:
            raise Error(self._error())
