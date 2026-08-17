from __future__ import annotations

import argparse
import os
import platform
import shutil
import stat
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PYTHON_SOURCE = ROOT / "python"


def native_names() -> tuple[str, ...]:
    system = platform.system()
    if system == "Windows":
        return ("water_structure_shared.dll",)
    if system == "Linux":
        return ("libwater_structure_shared.so", "water_structure_shared.so")
    if system == "Darwin":
        return ("libwater_structure_shared.dylib", "water_structure_shared.dylib")
    raise RuntimeError(f"unsupported build platform: {system}")


def find_native_library(explicit: Path | None) -> Path:
    if explicit is not None:
        candidate = explicit if explicit.is_absolute() else ROOT / explicit
        if candidate.is_file():
            return candidate
        raise FileNotFoundError(f"native library was not found: {candidate}")
    candidates = [
        path
        for name in native_names()
        for path in (ROOT / "build").glob(f"**/{name}")
        if path.is_file()
    ]
    if not candidates:
        raise FileNotFoundError(
            "native library was not produced under build/; run xmake for "
            "water_structure_shared or pass --native-library"
        )
    return max(candidates, key=lambda path: path.stat().st_mtime_ns)


def copy_package(stage: Path, native_library: Path) -> None:
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
    shutil.copy2(native_library, package / native_library.name)
    shutil.copytree(ROOT / "assets", package / "assets")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a platform wheel")
    parser.add_argument("--native-library", type=Path)
    parser.add_argument("--output", type=Path, default=Path("dist/python"))
    parser.add_argument("--skip-native-build", action="store_true")
    args = parser.parse_args()

    if not args.skip_native_build:
        subprocess.run(
            ["xmake", "f", "-m", "release", "-y"], cwd=ROOT, check=True
        )
        subprocess.run(
            ["xmake", "build", "water_structure_shared"],
            cwd=ROOT,
            check=True,
        )

    native_library = find_native_library(args.native_library)
    stage = ROOT / "build" / "python-wheel"
    copy_package(stage, native_library)
    output = args.output if args.output.is_absolute() else ROOT / args.output
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [sys.executable, "-m", "build", str(stage), "--wheel", "--outdir", str(output)],
        cwd=ROOT,
        check=True,
    )
    wheels = sorted(output.glob("water_structure-*.whl"), key=lambda p: p.stat().st_mtime_ns)
    if not wheels:
        raise RuntimeError("wheel build did not produce an output file")
    print(f"Built {wheels[-1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
