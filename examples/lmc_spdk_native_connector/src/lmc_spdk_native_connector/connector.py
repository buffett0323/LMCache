# SPDX-License-Identifier: Apache-2.0
"""
SPDK NVMe native connector plugin for LMCache.

This module exposes a thin Python factory over the C++ pybind11-wrapped
``SpdkNvmeConnector``, which stores KV cache chunks directly on an NVMe SSD
through SPDK's userspace polling driver (kernel bypass).

It is loaded through the ``native_plugin`` L2 adapter type::

    --l2-adapter '{
      "type": "native_plugin",
      "module_path": "lmc_spdk_native_connector",
      "class_name": "SpdkNativeConnector",
      "adapter_params": {
        "pci_addr": "0000:00:04.0",
        "slot_size_bytes": 1048576,
        "num_workers": 8,
        "nsid": 1
      }
    }'

Requires Linux, a working SPDK installation, hugepages, and an NVMe device
bound to the SPDK userspace driver (``scripts/setup.sh`` from SPDK). See the
package README for the full setup and benchmarking procedure.
"""

# Future
from __future__ import annotations

# Third Party
from lmc_spdk_native_connector._native import (
    SpdkNvmeConnector,
)


class SpdkNativeConnector:
    """Factory-style wrapper around the C++ ``SpdkNvmeConnector``.

    Constructor keyword arguments:
    - pci_addr (str): PCIe address of the NVMe device, e.g.
      ``"0000:00:04.0"`` (required).
    - slot_size_bytes (int): fixed per-key slot size in bytes; must be
      >= the largest chunk that will be stored. Rounded up to a multiple
      of the device sector size (required).
    - num_workers (int): number of C++ I/O worker threads, i.e. the NVMe
      queue depth (default 8).
    - nsid (int): NVMe namespace id (default 1).

    The returned instance is itself a native connector exposing the full
    pybind interface (``event_fd``, ``submit_batch_get/set/exists``,
    ``drain_completions``, ``close``), so it plugs straight into
    ``NativeConnectorL2Adapter`` via the ``native_plugin`` adapter type.
    """

    def __new__(
        cls,
        pci_addr: str,
        slot_size_bytes: int,
        num_workers: int = 8,
        nsid: int = 1,
    ):
        if not pci_addr:
            raise ValueError("pci_addr must be a non-empty PCIe address string")
        if not isinstance(slot_size_bytes, int) or slot_size_bytes <= 0:
            raise ValueError("slot_size_bytes must be a positive integer")
        if not isinstance(num_workers, int) or num_workers <= 0:
            raise ValueError("num_workers must be a positive integer")
        if not isinstance(nsid, int) or nsid <= 0:
            raise ValueError("nsid must be a positive integer")
        return SpdkNvmeConnector(pci_addr, slot_size_bytes, num_workers, nsid)
