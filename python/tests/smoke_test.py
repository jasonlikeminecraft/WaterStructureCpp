from __future__ import annotations

import argparse
from pathlib import Path

import water_structure


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args()

    assert water_structure.abi_version() == 1
    capabilities = water_structure.formats()
    assert len(capabilities) == 37
    assert all(item.name for item in capabilities)
    with water_structure.Context() as context:
        detected = context.format(args.fixture)
        info = context.inspect(args.fixture)
        assert detected
        assert info.width > 0 and info.height > 0 and info.length > 0
        print(water_structure.version(), detected, info)


if __name__ == "__main__":
    main()
