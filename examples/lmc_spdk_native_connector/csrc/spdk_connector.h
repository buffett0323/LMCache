// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "connector_base.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <spdk/env.h>
#include <spdk/nvme.h>
}

namespace lmc_spdk {

// ---------------------------------------------------------------
// Shared NVMe device state (one per connector, shared by workers).
//
// Populated once at construction time by probing/attaching the NVMe
// controller with the SPDK userspace driver. All workers read these
// fields; they are immutable after construction.
// ---------------------------------------------------------------
struct SharedNvme {
  struct spdk_nvme_ctrlr* ctrlr = nullptr;
  struct spdk_nvme_ns* ns = nullptr;
  uint32_t sector_size = 0;     // logical block size in bytes
  uint64_t num_sectors = 0;     // number of logical blocks on the namespace
  uint32_t blocks_per_slot = 0; // fixed slots carved from the namespace
  uint64_t slot_size_bytes = 0; // blocks_per_slot * sector_size
  uint64_t total_slots = 0;     // num_sectors / blocks_per_slot
};

// ---------------------------------------------------------------
// key -> slot index mapping.
//
// The NVMe namespace is a raw block device with no filesystem, so this
// connector carves it into ``total_slots`` fixed-size slots and keeps an
// in-memory index mapping each key to a slot and the exact stored length.
//
// NOTE: the index is in-memory only, so cached data does NOT survive a
// restart. This is sufficient for the benchmark comparison against
// ``fs_native`` (issue #4113). A production backend would persist the
// index (e.g. in a reserved metadata region of the namespace).
// ---------------------------------------------------------------
struct SlotMeta {
  uint64_t slot_idx = 0;
  size_t length = 0; // exact byte length stored (<= slot_size_bytes)
};

struct SharedIndex {
  std::mutex mu;
  std::unordered_map<std::string, SlotMeta> map;
  uint64_t next_free = 0;         // bump allocator cursor
  std::vector<uint64_t> freelist; // reclaimed slots (from deletes)
  uint64_t total_slots = 0;

  // Allocate a slot for ``key`` (reusing its existing slot if present).
  // Returns the slot index. Throws std::runtime_error when full.
  uint64_t allocate(const std::string& key, size_t length);
  // Look up ``key``; returns true and fills ``out`` on hit.
  bool lookup(const std::string& key, SlotMeta& out);
  bool exists(const std::string& key);
  // Erase ``key``, returning its slot to the freelist. Returns true if erased.
  bool erase(const std::string& key);
};

// ---------------------------------------------------------------
// Per-worker connection: one NVMe I/O queue pair + one DMA bounce buffer.
//
// SPDK requires that a qpair be used by exactly one thread at a time.
// ``ConnectorBase`` creates one connection per worker thread via
// ``create_connection()`` and never shares it, which satisfies that
// constraint. Move-only: the destructor frees the qpair and DMA buffer.
// ---------------------------------------------------------------
struct WorkerSpdkConn {
  std::shared_ptr<SharedNvme> nvme;
  std::shared_ptr<SharedIndex> index;
  struct spdk_nvme_qpair* qpair = nullptr;
  void* dma_buf = nullptr;
  size_t dma_buf_size = 0;

  WorkerSpdkConn() = default;
  WorkerSpdkConn(const WorkerSpdkConn&) = delete;
  WorkerSpdkConn& operator=(const WorkerSpdkConn&) = delete;
  WorkerSpdkConn(WorkerSpdkConn&& other) noexcept;
  WorkerSpdkConn& operator=(WorkerSpdkConn&& other) noexcept;
  ~WorkerSpdkConn();
};

// ---------------------------------------------------------------
// SpdkNvmeConnector
//
// A native LMCache connector that stores KV cache chunks directly on an
// NVMe SSD through SPDK's userspace polling driver, bypassing the kernel
// block stack. Overrides the four ``ConnectorBase`` primitives; the batch
// fan-out, worker pool and eventfd completion signalling are inherited.
//
// Wrap with ``LMCACHE_BIND_CONNECTOR_METHODS`` and load through the
// ``native_plugin`` L2 adapter type (see the package README).
// ---------------------------------------------------------------
class SpdkNvmeConnector : public lmcache::connector::ConnectorBase<WorkerSpdkConn> {
 public:
  // Args:
  //   pci_addr: PCIe address of the NVMe device (e.g. "0000:00:04.0").
  //   slot_size_bytes: fixed slot size; must be >= the largest chunk
  //     that will be stored. Rounded up to a multiple of the sector size.
  //   num_workers: number of I/O worker threads (== queue depth).
  //   nsid: NVMe namespace id to use (default 1).
  SpdkNvmeConnector(std::string pci_addr, uint64_t slot_size_bytes,
                    int num_workers, uint32_t nsid = 1);
  ~SpdkNvmeConnector() override;

 protected:
  WorkerSpdkConn create_connection() override;
  void do_single_get(WorkerSpdkConn& conn, const std::string& key, void* buf,
                     size_t len, size_t chunk_size) override;
  void do_single_set(WorkerSpdkConn& conn, const std::string& key,
                     const void* buf, size_t len, size_t chunk_size) override;
  bool do_single_exists(WorkerSpdkConn& conn, const std::string& key) override;
  bool do_single_delete(WorkerSpdkConn& conn, const std::string& key) override;

 private:
  // Blocking single-slot I/O: submits one NVMe read/write and polls the
  // qpair until completion. Throws std::runtime_error on failure.
  void blocking_io(WorkerSpdkConn& conn, bool is_write, uint64_t lba,
                   uint32_t lba_count);

  std::string pci_addr_;
  uint32_t nsid_;
  std::shared_ptr<SharedNvme> nvme_;
  std::shared_ptr<SharedIndex> index_;
};

}  // namespace lmc_spdk
