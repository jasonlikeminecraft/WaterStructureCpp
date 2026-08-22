from os import PathLike
from typing import Callable, NamedTuple, Optional, Tuple, Union

PathValue = Union[str, PathLike[str]]

class Error(RuntimeError): ...

class Progress(NamedTuple):
    stage: str
    completed: int
    total: Optional[int]
    percent: Optional[float]
    elapsed: float
    eta: Optional[float]
    indeterminate: bool

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

def version() -> str: ...
def abi_version() -> int: ...
def formats() -> Tuple[FormatCapabilities, ...]: ...

class Context:
    def __init__(self, assets_directory: Optional[PathValue] = None) -> None: ...
    def close(self) -> None: ...
    def __enter__(self) -> Context: ...
    def __exit__(self, *args: object) -> None: ...
    def inspect(self, path: PathValue, *, streaming_world_import: bool = False) -> StructureInfo: ...
    def format(self, path: PathValue, *, streaming_world_import: bool = False) -> str: ...
    def convert(self, input_path: PathValue, target_format: str, output_path: PathValue, *, threads: int = 0, clear_air: bool = True, chunk_partition: bool = False, memory_budget_mib: int = 450, max_in_flight_tasks: int = 0, max_in_flight_chunks: int = 0, allow_temporary_spool: bool = True, progress: Optional[Callable[[Progress], object]] = None) -> None: ...
    def to_world(self, input_path: PathValue, world_path: PathValue, *, start: Tuple[int, int, int] = (0, -4, 0), threads: int = 0, memory_budget_mib: int = 450, max_in_flight_chunks: int = 0, allow_temporary_spool: bool = True, progress: Optional[Callable[[Progress], object]] = None) -> None: ...
