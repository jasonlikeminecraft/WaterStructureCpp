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


class FormatCapabilities(NamedTuple):
    format_id: int
    name: str
    file_reader: bool
    file_writer: bool
    structure_to_world: bool
    world_to_structure: bool
    streaming_reader: bool
    streaming_writer: bool
    lossy_round_trip: bool


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
    elif system in {"Linux", "Android"}:
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
    if system not in {"Windows", "Linux", "Android", "Darwin"}:
        raise ImportError(f"water-structure does not support {system or 'this platform'}")
    errors: List[str] = []
    for candidate in _library_candidates():
        if not candidate.is_file():
            continue
        dll_directory = None
        try:
            # Python 3.8+ intentionally uses a restricted DLL search path on
            # Windows. Add the package directory while loading so an optional
            # side-by-side dependency can be resolved without modifying PATH.
            if system == "Windows" and hasattr(os, "add_dll_directory"):
                dll_directory = os.add_dll_directory(str(candidate.parent))
            library = ctypes.CDLL(str(candidate))
        except OSError as exc:
            if dll_directory is not None:
                dll_directory.close()
            errors.append(f"{candidate}: {exc}")
        else:
            if dll_directory is not None:
                _DLL_DIRECTORY_HANDLES.append(dll_directory)
            return library
    detail = "; ".join(errors) if errors else "bundled native library is missing"
    raise ImportError(
        "unable to load WaterStructure native library (" + detail + "). "
        "Reinstall the wheel or set WATER_STRUCTURE_LIBRARY to a compatible library."
    )


_DLL_DIRECTORY_HANDLES: List[object] = []
_lib = _load_library()
_REQUIRED_BASE_SYMBOLS = (
    "ws_version",
    "ws_abi_version",
    "ws_context_create",
    "ws_context_destroy",
    "ws_last_error",
    "ws_reader_open",
    "ws_reader_close",
    "ws_reader_info",
    "ws_reader_format",
    "ws_convert",
    "ws_to_world",
)
_missing_base_symbols = [
    name for name in _REQUIRED_BASE_SYMBOLS if not hasattr(_lib, name)
]
if _missing_base_symbols:
    raise ImportError(
        "WaterStructure native library is missing C ABI symbols: "
        + ", ".join(_missing_base_symbols)
    )
_lib.ws_version.argtypes = []
_lib.ws_version.restype = ctypes.c_char_p
_lib.ws_abi_version.argtypes = []
_lib.ws_abi_version.restype = ctypes.c_uint32
_ws_format_count = getattr(_lib, "ws_format_count", None)
_ws_format_name = getattr(_lib, "ws_format_name", None)
_ws_format_capabilities = getattr(_lib, "ws_format_capabilities", None)
if _ws_format_count is not None:
    _ws_format_count.argtypes = []
    _ws_format_count.restype = ctypes.c_uint32
if _ws_format_name is not None:
    _ws_format_name.argtypes = [ctypes.c_uint8]
    _ws_format_name.restype = ctypes.c_char_p
if _ws_format_capabilities is not None:
    _ws_format_capabilities.argtypes = [ctypes.c_uint8]
    _ws_format_capabilities.restype = ctypes.c_uint32
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
_ws_convert_ex2 = getattr(_lib, "ws_convert_ex2", None)
if _ws_convert_ex2 is not None:
    _ws_convert_ex2.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_int,
        ctypes.c_int,
    ]
    _ws_convert_ex2.restype = ctypes.c_int
_ws_convert_ex3 = getattr(_lib, "ws_convert_ex3", None)
if _ws_convert_ex3 is not None:
    _ws_convert_ex3.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_int,
    ]
    _ws_convert_ex3.restype = ctypes.c_int
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
_ws_convert_with_progress_ex2 = getattr(
    _lib, "ws_convert_with_progress_ex2", None
)
if _ws_convert_with_progress_ex2 is not None:
    _ws_convert_with_progress_ex2.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_int,
        ctypes.c_int,
        _ProgressCallback,
        ctypes.c_void_p,
    ]
    _ws_convert_with_progress_ex2.restype = ctypes.c_int
_ws_convert_with_progress_ex3 = getattr(
    _lib, "ws_convert_with_progress_ex3", None
)
if _ws_convert_with_progress_ex3 is not None:
    _ws_convert_with_progress_ex3.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_int,
        _ProgressCallback,
        ctypes.c_void_p,
    ]
    _ws_convert_with_progress_ex3.restype = ctypes.c_int
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
_ws_to_world_ex3 = getattr(_lib, "ws_to_world_ex3", None)
_ws_to_world_with_progress_ex3 = getattr(_lib, "ws_to_world_with_progress_ex3", None)
_WorldEx3Arguments = [
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_int32,
    ctypes.c_int32,
    ctypes.c_int32,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.c_int,
]
if _ws_to_world_ex3 is not None:
    _ws_to_world_ex3.argtypes = _WorldEx3Arguments
    _ws_to_world_ex3.restype = ctypes.c_int
if _ws_to_world_with_progress_ex3 is not None:
    _ws_to_world_with_progress_ex3.argtypes = _WorldEx3Arguments + [
        _ProgressCallback,
        ctypes.c_void_p,
    ]
    _ws_to_world_with_progress_ex3.restype = ctypes.c_int

if _lib.ws_abi_version() != 1:
    raise ImportError(f"unsupported WaterStructure C ABI: {_lib.ws_abi_version()}")


def version() -> str:
    """Return the bundled native library version."""
    value = _lib.ws_version()
    return value.decode("utf-8", "replace") if value else "unknown"


def abi_version() -> int:
    """Return the native C ABI version."""
    return int(_lib.ws_abi_version())


def formats() -> Tuple[FormatCapabilities, ...]:
    """Return the audited, direction-specific format capability table."""
    if (_ws_format_count is None or _ws_format_name is None or
            _ws_format_capabilities is None):
        raise Error("native library does not expose audited format capabilities")
    values: list[FormatCapabilities] = []
    expected = int(_ws_format_count())
    # ws_format_count() is a count, not a promise that future StructureId
    # values remain contiguous. Scan the complete uint8_t domain and stop once
    # the advertised number of entries has been collected.
    for format_id in range(1, 256):
        raw_name = _ws_format_name(format_id)
        if not raw_name:
            continue
        flags = int(_ws_format_capabilities(format_id))
        values.append(
            FormatCapabilities(
                format_id,
                raw_name.decode("utf-8", "replace"),
                bool(flags & (1 << 0)),
                bool(flags & (1 << 1)),
                bool(flags & (1 << 2)),
                bool(flags & (1 << 3)),
                bool(flags & (1 << 4)),
                bool(flags & (1 << 5)),
                bool(flags & (1 << 6)),
            )
        )
        if len(values) == expected:
            break
    if len(values) != expected:
        raise Error(
            "native capability table is inconsistent: "
            f"expected {expected} entries, received {len(values)}"
        )
    return tuple(values)


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
            if errors:
                return
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
        chunk_partition: bool = False,
        memory_budget_mib: int = 450,
        max_in_flight_tasks: int = 0,
        max_in_flight_chunks: int = 0,
        allow_temporary_spool: bool = True,
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

        ``chunk_partition`` keeps MCFunction's 3D optimizer inside individual
        16x16 chunks. It is disabled by default for backward-compatible output.

        ``memory_budget_mib`` is a soft streaming budget. Queue limits of zero
        select conservative native defaults; the external test runner can
        still apply a stricter hard process limit.
        """
        if threads < 0:
            raise ValueError("threads must be >= 0")
        if memory_budget_mib < 64 or memory_budget_mib > 8192:
            raise ValueError("memory_budget_mib must be between 64 and 8192")
        if max_in_flight_tasks < 0 or max_in_flight_chunks < 0:
            raise ValueError("in-flight limits must be >= 0")
        if max_in_flight_tasks > 4096 or max_in_flight_chunks > 4096:
            raise ValueError("in-flight limits must be <= 4096")
        memory_budget_bytes = memory_budget_mib * 1024 * 1024
        advanced_options = (
            memory_budget_mib != 450
            or max_in_flight_tasks != 0
            or max_in_flight_chunks != 0
            or not allow_temporary_spool
        )
        handle = self._require_open()
        if progress is None:
            args = (
                handle,
                os.fsencode(input_path),
                target_format.encode("utf-8"),
                os.fsencode(output_path),
                threads,
            )
            if _ws_convert_ex3 is not None:
                converted = _ws_convert_ex3(
                    *args,
                    int(clear_air),
                    int(chunk_partition),
                    memory_budget_bytes,
                    max_in_flight_tasks,
                    max_in_flight_chunks,
                    int(allow_temporary_spool),
                )
            elif advanced_options:
                raise Error("native library does not support streaming budgets")
            elif chunk_partition:
                if _ws_convert_ex2 is None:
                    raise Error("native library does not support chunk_partition")
                converted = _ws_convert_ex2(
                    *args, int(clear_air), 1,
                )
            elif clear_air:
                converted = _lib.ws_convert(*args)
            elif _ws_convert_ex is None:
                raise Error("native library does not support clear_air")
            else:
                converted = _ws_convert_ex(*args, 0)
            callback_errors: list[BaseException] = []
        else:
            if not callable(progress):
                raise TypeError("progress must be callable")
            if (_ws_convert_with_progress_ex3 is None and
                    _ws_convert_with_progress is None):
                raise Error("native library does not provide progress callbacks")
            callback, callback_errors = self._progress_callback(progress)
            args = (
                handle,
                os.fsencode(input_path),
                target_format.encode("utf-8"),
                os.fsencode(output_path),
                threads,
            )
            if _ws_convert_with_progress_ex3 is not None:
                converted = _ws_convert_with_progress_ex3(
                    *args,
                    int(clear_air),
                    int(chunk_partition),
                    memory_budget_bytes,
                    max_in_flight_tasks,
                    max_in_flight_chunks,
                    int(allow_temporary_spool),
                    callback,
                    None,
                )
            elif advanced_options:
                raise Error("native library does not support streaming budgets")
            elif chunk_partition:
                if _ws_convert_with_progress_ex2 is None:
                    raise Error("native library does not support chunk_partition")
                converted = _ws_convert_with_progress_ex2(
                    *args, int(clear_air), 1, callback, None,
                )
            elif clear_air:
                assert _ws_convert_with_progress is not None
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
        threads: int = 0,
        memory_budget_mib: int = 450,
        max_in_flight_chunks: int = 0,
        allow_temporary_spool: bool = True,
        progress: Optional[Callable[[Progress], object]] = None,
    ) -> None:
        """Stream a structure into a world directory or .mcworld archive."""
        if len(start) != 3:
            raise ValueError("start must contain x, subchunk-y, and z")
        if threads < 0:
            raise ValueError("threads must be >= 0")
        if memory_budget_mib < 64 or memory_budget_mib > 8192:
            raise ValueError("memory_budget_mib must be between 64 and 8192")
        if max_in_flight_chunks < 0 or max_in_flight_chunks > 4096:
            raise ValueError("max_in_flight_chunks must be between 0 and 4096")
        advanced_options = (
            threads != 0
            or memory_budget_mib != 450
            or max_in_flight_chunks != 0
            or not allow_temporary_spool
        )
        handle = self._require_open()
        world_args = (
            handle, os.fsencode(input_path), os.fsencode(world_path),
            int(start[0]), int(start[1]), int(start[2]), int(threads),
            memory_budget_mib * 1024 * 1024, int(max_in_flight_chunks),
            int(allow_temporary_spool),
        )
        if progress is None:
            if _ws_to_world_ex3 is not None:
                converted = _ws_to_world_ex3(*world_args)
            elif advanced_options:
                raise Error("native library does not support streaming world budgets")
            else:
                converted = _lib.ws_to_world(*world_args[:6])
            callback_errors: list[BaseException] = []
        else:
            if not callable(progress):
                raise TypeError("progress must be callable")
            if (_ws_to_world_with_progress_ex3 is None and
                    _ws_to_world_with_progress is None):
                raise Error("native library does not provide progress callbacks")
            callback, callback_errors = self._progress_callback(progress)
            if _ws_to_world_with_progress_ex3 is not None:
                converted = _ws_to_world_with_progress_ex3(*world_args, callback, None)
            elif advanced_options:
                raise Error("native library does not support streaming world budgets")
            else:
                assert _ws_to_world_with_progress is not None
                converted = _ws_to_world_with_progress(*world_args[:6], callback, None)
        if callback_errors:
            raise callback_errors[0]
        if not converted:
            raise Error(self._error())
