import os

from setuptools import setup
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


class PlatformWheel(_bdist_wheel):
    """Mark the ctypes package as platform-specific but Python-ABI neutral."""

    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        _, _, platform_tag = super().get_tag()
        platform_tag = os.environ.get("WATER_STRUCTURE_WHEEL_PLATFORM", platform_tag)
        return "py3", "none", platform_tag


setup(cmdclass={"bdist_wheel": PlatformWheel})
