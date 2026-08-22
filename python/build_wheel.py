from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import stat
import struct
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parent.parent
PYTHON_SOURCE = ROOT / "python"
NATIVE_BUILD = ROOT / "build" / "python-native"
WHEEL_STAGE = ROOT / "build" / "python-wheel"
REQUIRED_C_API_SYMBOLS = (
    "ws_version",
    "ws_abi_version",
    "ws_context_create",
    "ws_context_destroy",
    "ws_last_error",
    "ws_reader_open",
    "ws_reader_close",
    "ws_reader_info",
    "ws_reader_format",
    "ws_format_count",
    "ws_format_name",
    "ws_format_capabilities",
    "ws_convert",
    "ws_convert_ex3",
    "ws_convert_with_progress_ex3",
    "ws_to_world",
    "ws_to_world_ex3",
    "ws_to_world_with_progress_ex3",
)


def native_names(system: str | None = None) -> tuple[str, ...]:
    system = system or platform.system()
    if system == "Windows":
        return ("water_structure_shared.dll",)
    if system in {"Linux", "Android"}:
        return ("libwater_structure_shared.so", "water_structure_shared.so")
    if system == "Darwin":
        return ("libwater_structure_shared.dylib", "water_structure_shared.dylib")
    raise RuntimeError(f"unsupported build platform: {system}")


def _host_xmake_target() -> tuple[str, str]:
    systems = {
        "Windows": "windows",
        "Linux": "linux",
        "Darwin": "macosx",
    }
    machines = {
        "amd64": "x64",
        "x86_64": "x64",
        "i386": "x86",
        "i686": "x86",
        "x86": "x86",
        "aarch64": "arm64",
        "arm64": "arm64",
    }
    system = platform.system()
    machine = platform.machine().lower()
    try:
        return systems[system], machines[machine]
    except KeyError as error:
        raise RuntimeError(
            f"unsupported native build host: {system or 'unknown'}/{machine or 'unknown'}; "
            "cross builds must pass --skip-native-build and --native-library"
        ) from error


def build_native_library() -> None:
    target_platform, target_arch = _host_xmake_target()
    subprocess.run(
        [
            "xmake",
            "f",
            "-p",
            target_platform,
            "-a",
            target_arch,
            "-m",
            "release",
            "-o",
            str(NATIVE_BUILD),
            "-y",
        ],
        cwd=ROOT,
        check=True,
    )
    # Intentionally leave parallelism to xmake and the build environment.
    subprocess.run(
        ["xmake", "build", "water_structure_shared"],
        cwd=ROOT,
        check=True,
    )


def _candidate_libraries(search_roots: Iterable[Path]) -> list[Path]:
    candidates: list[Path] = []
    stage = WHEEL_STAGE.resolve()
    for root in search_roots:
        if not root.is_dir():
            continue
        for name in native_names():
            for path in root.glob(f"**/{name}"):
                if not path.is_file():
                    continue
                try:
                    path.resolve().relative_to(stage)
                except ValueError:
                    candidates.append(path)
    return candidates


def find_native_library(
    explicit: Path | None,
    *,
    search_roots: Iterable[Path] = (NATIVE_BUILD,),
) -> Path:
    if explicit is not None:
        candidate = explicit if explicit.is_absolute() else ROOT / explicit
        if candidate.is_file():
            resolved = candidate.resolve()
            try:
                resolved.relative_to(WHEEL_STAGE.resolve())
            except ValueError:
                return resolved
            raise ValueError(
                "--native-library cannot point inside the disposable wheel staging directory"
            )
        raise FileNotFoundError(f"native library was not found: {candidate}")
    candidates = _candidate_libraries(search_roots)
    if not candidates:
        roots = ", ".join(str(path) for path in search_roots)
        raise FileNotFoundError(
            f"native library was not produced under {roots}; build "
            "water_structure_shared or pass --native-library"
        )
    return max(candidates, key=lambda path: path.stat().st_mtime_ns).resolve()


def _native_architectures(path: Path) -> frozenset[str]:
    """Return every architecture encoded by PE, ELF, or Mach-O headers."""
    cpu_names = {
        7: "x86",
        0x01000007: "x64",
        12: "arm",
        0x0100000C: "arm64",
    }
    with path.open("rb") as stream:
        header = stream.read(64)
        if header.startswith(b"MZ") and len(header) >= 64:
            pe_offset = struct.unpack_from("<I", header, 0x3C)[0]
            stream.seek(pe_offset)
            pe_header = stream.read(6)
            if pe_header[:4] != b"PE\0\0":
                return frozenset()
            architecture = {
                0x014C: "x86",
                0x8664: "x64",
                0x01C4: "arm",
                0xAA64: "arm64",
            }.get(struct.unpack_from("<H", pe_header, 4)[0])
            return frozenset((architecture,)) if architecture else frozenset()
        if header.startswith(b"\x7fELF") and len(header) >= 20:
            byte_order = "<" if header[5] == 1 else ">" if header[5] == 2 else None
            if byte_order is None:
                return frozenset()
            architecture = {
                3: "x86",
                40: "arm",
                62: "x64",
                183: "arm64",
            }.get(struct.unpack_from(f"{byte_order}H", header, 18)[0])
            return frozenset((architecture,)) if architecture else frozenset()
        fat_headers = {
            b"\xca\xfe\xba\xbe": (">", 20),
            b"\xbe\xba\xfe\xca": ("<", 20),
            b"\xca\xfe\xba\xbf": (">", 32),
            b"\xbf\xba\xfe\xca": ("<", 32),
        }
        if header[:4] in fat_headers:
            byte_order, record_size = fat_headers[header[:4]]
            slice_count = struct.unpack_from(f"{byte_order}I", header, 4)[0]
            if slice_count == 0 or slice_count > 64:
                return frozenset()
            stream.seek(8)
            architectures: set[str] = set()
            for _ in range(slice_count):
                record = stream.read(record_size)
                if len(record) != record_size:
                    return frozenset()
                architecture = cpu_names.get(
                    struct.unpack_from(f"{byte_order}I", record, 0)[0]
                )
                if architecture:
                    architectures.add(architecture)
            return frozenset(architectures)
        if header[:4] in {
            b"\xfe\xed\xfa\xce",
            b"\xce\xfa\xed\xfe",
            b"\xfe\xed\xfa\xcf",
            b"\xcf\xfa\xed\xfe",
        }:
            little_endian = header[:4] in {b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe"}
            byte_order = "<" if little_endian else ">"
            cpu_type = struct.unpack_from(f"{byte_order}I", header, 4)[0]
            architecture = cpu_names.get(cpu_type)
            return frozenset((architecture,)) if architecture else frozenset()
    return frozenset()


def canonical_native_name(path: Path) -> str:
    with path.open("rb") as stream:
        magic = stream.read(4)
    if magic[:2] == b"MZ":
        return "water_structure_shared.dll"
    if magic == b"\x7fELF":
        return "libwater_structure_shared.so"
    if magic in {
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf",
        b"\xbf\xba\xfe\xca",
        b"\xfe\xed\xfa\xce",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe",
    }:
        return "libwater_structure_shared.dylib"
    raise RuntimeError(f"unrecognized native library format: {path}")


def validate_native_library(path: Path, platform_tag: str | None) -> None:
    architectures = _native_architectures(path)
    if not architectures:
        raise RuntimeError(f"unrecognized native library format or architecture: {path}")
    native_name = canonical_native_name(path)
    # Exported C names remain in the PE/ELF/Mach-O string table even in a
    # stripped release binary. This cross-build-safe check catches accidentally
    # packaging an older library that cannot back the current Python API.
    contents = path.read_bytes()
    missing = [
        symbol for symbol in REQUIRED_C_API_SYMBOLS
        if symbol.encode("ascii") not in contents
    ]
    if missing:
        raise RuntimeError(
            f"native library is missing required C API exports: {', '.join(missing)}"
        )
    if not platform_tag:
        target_platform, expected = _host_xmake_target()
        expected_names = {
            "windows": "water_structure_shared.dll",
            "linux": "libwater_structure_shared.so",
            "macosx": "libwater_structure_shared.dylib",
        }
        if native_name != expected_names[target_platform]:
            raise RuntimeError(
                f"native library format {native_name} does not match build host "
                f"platform {target_platform}"
            )
        if expected not in architectures:
            raise RuntimeError(
                f"native library architectures {sorted(architectures)} do not match "
                f"build host architecture {expected}"
            )
        return
    tag = platform_tag.lower()
    if tag.startswith("win") and not native_name.endswith(".dll"):
        raise RuntimeError(f"PE DLL is required for wheel platform tag {platform_tag}")
    if ("linux" in tag or "manylinux" in tag or "musllinux" in tag or
            "android" in tag) and not native_name.endswith(".so"):
        raise RuntimeError(f"ELF shared object is required for wheel platform tag {platform_tag}")
    if "macosx" in tag and not native_name.endswith(".dylib"):
        raise RuntimeError(f"Mach-O dylib is required for wheel platform tag {platform_tag}")
    expected = None
    if "universal2" in tag:
        required = {"x64", "arm64"}
        if not required.issubset(architectures):
            raise RuntimeError(
                f"wheel tag {platform_tag} requires x64 and arm64 Mach-O slices; "
                f"found {sorted(architectures)}"
            )
        return
    if tag == "win32":
        expected = "x86"
    elif "arm64_v8a" in tag or "aarch64" in tag or "arm64" in tag:
        expected = "arm64"
    elif "x86_64" in tag or "amd64" in tag or "win_amd64" in tag:
        expected = "x64"
    elif "armeabi_v7a" in tag or "armv7" in tag:
        expected = "arm"
    elif re.search(r"(?:^|[_.])(?:x86|i686)(?:$|[_.])", tag):
        expected = "x86"
    if expected is not None and expected not in architectures:
        raise RuntimeError(
            f"native library architectures {sorted(architectures)} do not match "
            f"wheel platform tag {platform_tag}"
        )


def copy_package(stage: Path, native_library: Path, native_name: str) -> None:
    if stage.exists():
        def remove_readonly(function, path, _error) -> None:
            os.chmod(path, stat.S_IWRITE)
            function(path)

        shutil.rmtree(stage, onerror=remove_readonly)
    package = stage / "water_structure"
    package.mkdir(parents=True)
    for name in ("pyproject.toml", "setup.py", "README.md"):
        shutil.copy2(PYTHON_SOURCE / name, stage / name)
    shutil.copy2(ROOT / "LICENSE", stage / "LICENSE")
    shutil.copytree(
        PYTHON_SOURCE / "water_structure",
        package,
        dirs_exist_ok=True,
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    # The loader uses stable C-ABI filenames. Normalizing an explicitly named
    # input prevents producing a wheel that contains a library it cannot find.
    shutil.copy2(native_library, package / native_name)
    shutil.copytree(ROOT / "assets", package / "assets")


def validate_wheel(
    wheel: Path,
    native_name: str,
    expected_platform_tag: str | None = None,
) -> None:
    with zipfile.ZipFile(wheel) as archive:
        names = archive.namelist()
        required_suffixes = (
            "/water_structure/__init__.py",
            "/water_structure/__init__.pyi",
            "/water_structure/_binding.py",
            "/water_structure/py.typed",
            f"/water_structure/{native_name}",
            "/water_structure/assets/block_mappings_v1.json",
        )
        normalized = [f"/{name}" for name in names]
        for suffix in required_suffixes:
            if not any(name.endswith(suffix) for name in normalized):
                raise RuntimeError(f"wheel is missing required package data: {suffix}")
        wheel_metadata = [name for name in names if name.endswith(".dist-info/WHEEL")]
        if len(wheel_metadata) != 1:
            raise RuntimeError("wheel contains invalid WHEEL metadata")
        metadata = archive.read(wheel_metadata[0]).decode("utf-8", "strict")
        if "Root-Is-Purelib: false" not in metadata:
            raise RuntimeError("native wheel was incorrectly marked as pure Python")
        tags = [line[5:].strip() for line in metadata.splitlines() if line.startswith("Tag: ")]
        if not tags or any(tag.endswith("-any") for tag in tags):
            raise RuntimeError("native wheel has an invalid platform-independent tag")
        if not any(tag.startswith("py3-none-") for tag in tags):
            raise RuntimeError("ctypes wheel must use a py3-none platform tag")
        filename = wheel.name
        if not filename.endswith(".whl"):
            raise RuntimeError("wheel output has an invalid filename")
        filename_fields = filename[:-4].rsplit("-", 3)
        if len(filename_fields) != 4:
            raise RuntimeError("wheel filename does not contain PEP 425 tags")
        filename_tag = "-".join(filename_fields[1:])
        if filename_tag not in tags:
            raise RuntimeError(
                f"wheel filename tag {filename_tag} is absent from WHEEL metadata: {tags}"
            )
        if expected_platform_tag is not None:
            expected_tag = f"py3-none-{expected_platform_tag}"
            if filename_tag != expected_tag or set(tags) != {expected_tag}:
                raise RuntimeError(
                    f"requested wheel tag {expected_tag} does not exactly match "
                    f"filename/metadata tags ({filename_tag}, {tags})"
                )


def _validate_platform_tag(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.]+", value) or value.lower() == "any":
        raise argparse.ArgumentTypeError(
            "platform tag must contain only letters, digits, '.', and '_' and cannot be 'any'"
        )
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a platform wheel")
    parser.add_argument("--native-library", type=Path)
    parser.add_argument("--output", type=Path, default=Path("dist/python"))
    parser.add_argument(
        "--platform-tag",
        type=_validate_platform_tag,
        help="Override the wheel platform tag, for example android_24_arm64_v8a",
    )
    parser.add_argument("--skip-native-build", action="store_true")
    args = parser.parse_args()

    if args.platform_tag and (not args.skip_native_build or args.native_library is None):
        parser.error(
            "--platform-tag requires --skip-native-build and an explicit --native-library"
        )
    if args.skip_native_build and args.native_library is None:
        parser.error("--skip-native-build requires an explicit --native-library")

    if not args.skip_native_build:
        build_native_library()
        search_roots = (NATIVE_BUILD,)
    else:
        search_roots = ()

    native_library = find_native_library(args.native_library, search_roots=search_roots)
    validate_native_library(native_library, args.platform_tag)
    native_name = canonical_native_name(native_library)
    copy_package(WHEEL_STAGE, native_library, native_name)
    output = args.output if args.output.is_absolute() else ROOT / args.output
    output.mkdir(parents=True, exist_ok=True)
    before = {
        path.resolve(): (path.stat().st_mtime_ns, path.stat().st_size)
        for path in output.glob("water_structure-*.whl")
    }
    build_environment = os.environ.copy()
    if args.platform_tag:
        build_environment["WATER_STRUCTURE_WHEEL_PLATFORM"] = args.platform_tag
    else:
        build_environment.pop("WATER_STRUCTURE_WHEEL_PLATFORM", None)
    subprocess.run(
        [sys.executable, "-m", "build", str(WHEEL_STAGE), "--wheel", "--outdir", str(output)],
        cwd=ROOT,
        env=build_environment,
        check=True,
    )
    wheels = []
    for path in output.glob("water_structure-*.whl"):
        current = (path.stat().st_mtime_ns, path.stat().st_size)
        if before.get(path.resolve()) != current:
            wheels.append(path)
    if len(wheels) != 1:
        raise RuntimeError(
            f"wheel build produced {len(wheels)} new or changed outputs; expected exactly one"
        )
    wheel = wheels[0].resolve()
    validate_wheel(wheel, native_name, args.platform_tag)
    print(f"Built {wheel}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
