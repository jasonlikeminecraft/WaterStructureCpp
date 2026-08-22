import os
import re

from setuptools import setup
from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel


class PlatformWheel(_bdist_wheel):
    """Mark the ctypes package as platform-specific but Python-ABI neutral."""

    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        _, _, platform_tag = super().get_tag()
        override = os.environ.get("WATER_STRUCTURE_WHEEL_PLATFORM")
        if override:
            if (not re.fullmatch(r"[A-Za-z0-9_.]+", override) or
                    override.lower() == "any"):
                raise RuntimeError(f"invalid native wheel platform tag: {override!r}")
            platform_tag = override
        return "py3", "none", platform_tag


setup(cmdclass={"bdist_wheel": PlatformWheel})
