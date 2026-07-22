# SPDX-License-Identifier: Apache-2.0
"""
Build script for the SPDK NVMe native connector plugin.

Compiles the C++ pybind11 extension implementing ``SpdkNvmeConnector``. The
LMCache ``connector_base.h`` headers are resolved from the LMCache source
tree, assuming this example lives inside the repository.

Requirements to build the native extension:
  * Linux (SPDK + eventfd).
  * A working SPDK installation discoverable via ``pkg-config`` under the
    ``spdk_nvme`` / ``spdk_env_dpdk`` packages. Build SPDK with
    ``./configure --with-shared && make`` and either run ``make install`` or
    point ``PKG_CONFIG_PATH`` at ``<spdk>/build/lib/pkgconfig``.

On non-Linux platforms, or when SPDK is not found, the native extension is
skipped and only the pure-Python wrapper module is installed (which will
raise ImportError if actually used). This keeps ``pip install`` from hard
-failing during inspection on unsupported hosts.
"""

# Standard
from pathlib import Path
import os
import platform
import subprocess

# Third Party
from setuptools import Extension, find_packages, setup
import pybind11

ROOT_DIR = Path(__file__).resolve().parent
CSRC_REL = os.path.join("csrc")

# Resolve LMCache csrc headers (connector_base.h, etc.) from the repo root.
LMCACHE_ROOT = ROOT_DIR.parent.parent
LMCACHE_CSRC = str(LMCACHE_ROOT / "csrc" / "storage_backends")


def _pkg_config(args: list[str]) -> list[str]:
    """Return ``pkg-config`` tokens for the SPDK packages, or [] on failure."""
    packages = ["spdk_nvme", "spdk_env_dpdk"]
    try:
        out = subprocess.check_output(["pkg-config", *args, *packages])
    except (OSError, subprocess.CalledProcessError):
        return []
    return out.decode().split()


ext_modules = []
if platform.system() == "Linux":
    cflags = _pkg_config(["--cflags"])
    ldflags = _pkg_config(["--libs"])
    if ldflags:
        ext_modules = [
            Extension(
                "lmc_spdk_native_connector._native",
                sources=[
                    os.path.join(CSRC_REL, "pybind.cpp"),
                    os.path.join(CSRC_REL, "spdk_connector.cpp"),
                ],
                include_dirs=[
                    str(ROOT_DIR / "csrc"),
                    LMCACHE_CSRC,
                    pybind11.get_include(),
                ],
                language="c++",
                extra_compile_args=["-O3", "-std=c++17", *cflags],
                extra_link_args=ldflags,
            ),
        ]
    else:
        print(
            "WARNING: SPDK not found via pkg-config (spdk_nvme, spdk_env_dpdk); "
            "skipping native extension build. Install SPDK and set "
            "PKG_CONFIG_PATH to build the connector."
        )

setup(
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    ext_modules=ext_modules,
)
