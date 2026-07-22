// SPDX-License-Identifier: Apache-2.0
#include <pybind11/pybind11.h>
#include "connector_pybind_utils.h"
#include "spdk_connector.h"

namespace py = pybind11;

PYBIND11_MODULE(_native, m) {
  m.doc() = "SPDK NVMe native connector plugin for LMCache";

  py::class_<lmc_spdk::SpdkNvmeConnector>(m, "SpdkNvmeConnector")
      .def(py::init<std::string, uint64_t, int, uint32_t>(),
           py::arg("pci_addr"), py::arg("slot_size_bytes"),
           py::arg("num_workers"), py::arg("nsid") = 1)
          LMCACHE_BIND_CONNECTOR_METHODS(lmc_spdk::SpdkNvmeConnector);
}
