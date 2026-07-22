# SPDX-License-Identifier: Apache-2.0
"""
lmc_spdk_native_connector - SPDK NVMe native connector plugin for LMCache.

Stores KV cache chunks directly on an NVMe SSD via SPDK's userspace polling
driver (kernel bypass), exposing the standard LMCache native connector
interface (event_fd, submit_batch_get/set/exists, drain_completions, close).
Load it via the ``native_plugin`` L2 adapter type.

Public API:
- SpdkNativeConnector: Python factory that builds the C++ connector.
"""

# Third Party
from lmc_spdk_native_connector.connector import (
    SpdkNativeConnector,
)

__all__ = [
    "SpdkNativeConnector",
]
