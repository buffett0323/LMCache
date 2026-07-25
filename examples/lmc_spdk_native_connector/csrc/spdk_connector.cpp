// SPDX-License-Identifier: Apache-2.0

#include "spdk_connector.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace lmc_spdk {

// ---------------------------------------------------------------
// SharedIndex
// ---------------------------------------------------------------

uint64_t SharedIndex::allocate(const std::string& key, size_t length) {
  std::lock_guard<std::mutex> lk(mu);
  auto it = map.find(key);
  if (it != map.end()) {
    it->second.length = length;
    return it->second.slot_idx;
  }
  uint64_t slot;
  if (!freelist.empty()) {
    slot = freelist.back();
    freelist.pop_back();
  } else if (next_free < total_slots) {
    slot = next_free++;
  } else {
    throw std::runtime_error("SpdkNvmeConnector: out of slots (namespace full)");
  }
  map.emplace(key, SlotMeta{slot, length});
  return slot;
}

bool SharedIndex::lookup(const std::string& key, SlotMeta& out) {
  std::lock_guard<std::mutex> lk(mu);
  auto it = map.find(key);
  if (it == map.end()) return false;
  out = it->second;
  return true;
}

bool SharedIndex::exists(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu);
  return map.count(key) > 0;
}

bool SharedIndex::erase(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu);
  auto it = map.find(key);
  if (it == map.end()) return false;
  freelist.push_back(it->second.slot_idx);
  map.erase(it);
  return true;
}

// ---------------------------------------------------------------
// WorkerSpdkConn move / destroy
// ---------------------------------------------------------------

WorkerSpdkConn::WorkerSpdkConn(WorkerSpdkConn&& other) noexcept
    : nvme(std::move(other.nvme)),
      index(std::move(other.index)),
      qpair(other.qpair),
      dma_buf(other.dma_buf),
      dma_buf_size(other.dma_buf_size) {
  other.qpair = nullptr;
  other.dma_buf = nullptr;
  other.dma_buf_size = 0;
}

WorkerSpdkConn& WorkerSpdkConn::operator=(WorkerSpdkConn&& other) noexcept {
  if (this != &other) {
    // Release any resources we currently own before taking over other's.
    if (dma_buf) spdk_free(dma_buf);
    if (qpair && nvme && nvme->ctrlr) {
      spdk_nvme_ctrlr_free_io_qpair(qpair);
    }
    nvme = std::move(other.nvme);
    index = std::move(other.index);
    qpair = other.qpair;
    dma_buf = other.dma_buf;
    dma_buf_size = other.dma_buf_size;
    other.qpair = nullptr;
    other.dma_buf = nullptr;
    other.dma_buf_size = 0;
  }
  return *this;
}

WorkerSpdkConn::~WorkerSpdkConn() {
  if (dma_buf) spdk_free(dma_buf);
  if (qpair && nvme && nvme->ctrlr) {
    spdk_nvme_ctrlr_free_io_qpair(qpair);
  }
}

// ---------------------------------------------------------------
// SPDK env init + device probe/attach
// ---------------------------------------------------------------

namespace {

// SPDK's env may only be initialized once per process. Guard it so that
// constructing multiple connectors (e.g. in tests) is safe.
std::once_flag g_spdk_env_once;

void init_spdk_env_once() {
  std::call_once(g_spdk_env_once, []() {
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "lmc_spdk_native_connector";
    if (spdk_env_init(&opts) < 0) {
      throw std::runtime_error(
          "spdk_env_init failed: ensure hugepages are configured and the "
          "process has permission to access the NVMe device");
    }
  });
}

// Context passed to the probe/attach callbacks.
struct ProbeCtx {
  uint32_t nsid;
  struct spdk_nvme_ctrlr* ctrlr = nullptr;
  bool attached = false;
};

bool probe_cb(void* cb_ctx, const struct spdk_nvme_transport_id* /*trid*/,
              struct spdk_nvme_ctrlr_opts* /*opts*/) {
  (void)cb_ctx;
  // Attach to every controller the trid selects. Because we pass an
  // explicit PCIe traddr to spdk_nvme_probe, at most one matches.
  return true;
}

void attach_cb(void* cb_ctx, const struct spdk_nvme_transport_id* /*trid*/,
               struct spdk_nvme_ctrlr* ctrlr,
               const struct spdk_nvme_ctrlr_opts* /*opts*/) {
  auto* ctx = static_cast<ProbeCtx*>(cb_ctx);
  ctx->ctrlr = ctrlr;
  ctx->attached = true;
}

}  // namespace

// ---------------------------------------------------------------
// SpdkNvmeConnector
// ---------------------------------------------------------------

SpdkNvmeConnector::SpdkNvmeConnector(std::string pci_addr,
                                     uint64_t slot_size_bytes, int num_workers,
                                     uint32_t nsid)
    : ConnectorBase(num_workers),
      pci_addr_(std::move(pci_addr)),
      nsid_(nsid),
      nvme_(std::make_shared<SharedNvme>()),
      index_(std::make_shared<SharedIndex>()) {
  if (pci_addr_.empty()) {
    throw std::runtime_error("SpdkNvmeConnector: pci_addr must be non-empty");
  }
  if (slot_size_bytes == 0) {
    throw std::runtime_error("SpdkNvmeConnector: slot_size_bytes must be > 0");
  }

  init_spdk_env_once();

  struct spdk_nvme_transport_id trid;
  std::memset(&trid, 0, sizeof(trid));
  spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
  std::snprintf(trid.traddr, sizeof(trid.traddr), "%s", pci_addr_.c_str());

  ProbeCtx ctx;
  ctx.nsid = nsid_;
  if (spdk_nvme_probe(&trid, &ctx, probe_cb, attach_cb, nullptr) != 0 ||
      !ctx.attached || ctx.ctrlr == nullptr) {
    throw std::runtime_error(
        "SpdkNvmeConnector: failed to probe/attach NVMe controller at " +
        pci_addr_ + " (is it bound to the SPDK userspace driver via setup.sh?)");
  }

  nvme_->ctrlr = ctx.ctrlr;
  nvme_->ns = spdk_nvme_ctrlr_get_ns(ctx.ctrlr, nsid_);
  if (nvme_->ns == nullptr || !spdk_nvme_ns_is_active(nvme_->ns)) {
    spdk_nvme_detach(nvme_->ctrlr);
    nvme_->ctrlr = nullptr;
    throw std::runtime_error("SpdkNvmeConnector: namespace " +
                             std::to_string(nsid_) + " is not active");
  }

  nvme_->sector_size = spdk_nvme_ns_get_sector_size(nvme_->ns);
  nvme_->num_sectors = spdk_nvme_ns_get_num_sectors(nvme_->ns);
  if (nvme_->sector_size == 0) {
    throw std::runtime_error("SpdkNvmeConnector: invalid sector size");
  }

  // Round slot size up to a whole number of sectors.
  nvme_->blocks_per_slot = static_cast<uint32_t>(
      (slot_size_bytes + nvme_->sector_size - 1) / nvme_->sector_size);
  nvme_->slot_size_bytes =
      static_cast<uint64_t>(nvme_->blocks_per_slot) * nvme_->sector_size;
  nvme_->total_slots = nvme_->num_sectors / nvme_->blocks_per_slot;
  if (nvme_->total_slots == 0) {
    throw std::runtime_error(
        "SpdkNvmeConnector: namespace too small for a single slot");
  }
  index_->total_slots = nvme_->total_slots;

  // Workers must not start until the device state above is fully populated,
  // so start_workers() is called last (ConnectorBase requirement).
  start_workers();
}

SpdkNvmeConnector::~SpdkNvmeConnector() {
  // Stop workers first (frees per-worker qpairs via WorkerSpdkConn dtor),
  // then detach the controller.
  close();
  if (nvme_ && nvme_->ctrlr) {
    spdk_nvme_detach(nvme_->ctrlr);
    nvme_->ctrlr = nullptr;
  }
}

WorkerSpdkConn SpdkNvmeConnector::create_connection() {
  WorkerSpdkConn conn;
  conn.nvme = nvme_;
  conn.index = index_;
  conn.qpair = spdk_nvme_ctrlr_alloc_io_qpair(nvme_->ctrlr, nullptr, 0);
  if (conn.qpair == nullptr) {
    throw std::runtime_error("SpdkNvmeConnector: failed to allocate I/O qpair");
  }
  // One DMA-capable bounce buffer per worker, sized for kMaxInFlightPerWorker
  // concurrent slots (do_batch_get/do_batch_set pipeline that many
  // outstanding commands on this qpair, each using its own slot-sized
  // slice) and aligned to the sector size (SPDK requires DMA-able, aligned
  // memory).
  size_t buf_size = kMaxInFlightPerWorker * nvme_->slot_size_bytes;
  conn.dma_buf = spdk_zmalloc(buf_size, nvme_->sector_size, nullptr,
                              SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
  if (conn.dma_buf == nullptr) {
    spdk_nvme_ctrlr_free_io_qpair(conn.qpair);
    conn.qpair = nullptr;
    throw std::runtime_error("SpdkNvmeConnector: spdk_zmalloc failed");
  }
  conn.dma_buf_size = buf_size;
  return conn;
}

namespace {

struct IoCtx {
  bool done = false;
  bool success = false;
};

void io_complete_cb(void* arg, const struct spdk_nvme_cpl* cpl) {
  auto* ctx = static_cast<IoCtx*>(arg);
  ctx->success = !spdk_nvme_cpl_is_error(cpl);
  ctx->done = true;
}

}  // namespace

void SpdkNvmeConnector::blocking_io(WorkerSpdkConn& conn, bool is_write,
                                    uint64_t lba, uint32_t lba_count) {
  IoCtx ctx;
  int rc;
  if (is_write) {
    rc = spdk_nvme_ns_cmd_write(conn.nvme->ns, conn.qpair, conn.dma_buf, lba,
                                lba_count, io_complete_cb, &ctx, 0);
  } else {
    rc = spdk_nvme_ns_cmd_read(conn.nvme->ns, conn.qpair, conn.dma_buf, lba,
                               lba_count, io_complete_cb, &ctx, 0);
  }
  if (rc != 0) {
    throw std::runtime_error("SpdkNvmeConnector: failed to submit NVMe " +
                             std::string(is_write ? "write" : "read"));
  }
  // Poll this worker's qpair until the single in-flight command completes.
  while (!ctx.done) {
    spdk_nvme_qpair_process_completions(conn.qpair, 0);
  }
  if (!ctx.success) {
    throw std::runtime_error("SpdkNvmeConnector: NVMe " +
                             std::string(is_write ? "write" : "read") +
                             " completed with error");
  }
}

void SpdkNvmeConnector::do_single_set(WorkerSpdkConn& conn,
                                      const std::string& key, const void* buf,
                                      size_t len, size_t /*chunk_size*/) {
  if (len > conn.nvme->slot_size_bytes) {
    throw std::runtime_error(
        "SpdkNvmeConnector: value length " + std::to_string(len) +
        " exceeds slot_size_bytes " +
        std::to_string(conn.nvme->slot_size_bytes));
  }
  uint64_t slot = conn.index->allocate(key, len);
  uint64_t lba = slot * conn.nvme->blocks_per_slot;
  uint32_t lba_count = static_cast<uint32_t>(
      (len + conn.nvme->sector_size - 1) / conn.nvme->sector_size);

  // Copy into the DMA bounce buffer and zero the tail of the final sector
  // so no stale bytes are written past ``len``.
  std::memcpy(conn.dma_buf, buf, len);
  size_t io_bytes = static_cast<size_t>(lba_count) * conn.nvme->sector_size;
  if (io_bytes > len) {
    std::memset(static_cast<char*>(conn.dma_buf) + len, 0, io_bytes - len);
  }
  blocking_io(conn, /*is_write=*/true, lba, lba_count);
}

void SpdkNvmeConnector::do_single_get(WorkerSpdkConn& conn,
                                      const std::string& key, void* buf,
                                      size_t len, size_t /*chunk_size*/) {
  SlotMeta meta;
  if (!conn.index->lookup(key, meta)) {
    throw std::runtime_error("SpdkNvmeConnector: key not found: " + key);
  }
  if (meta.length != len) {
    throw std::runtime_error("SpdkNvmeConnector: size mismatch for key " + key +
                             " (stored " + std::to_string(meta.length) +
                             ", requested " + std::to_string(len) + ")");
  }
  uint64_t lba = meta.slot_idx * conn.nvme->blocks_per_slot;
  uint32_t lba_count = static_cast<uint32_t>(
      (len + conn.nvme->sector_size - 1) / conn.nvme->sector_size);
  blocking_io(conn, /*is_write=*/false, lba, lba_count);
  std::memcpy(buf, conn.dma_buf, len);
}

bool SpdkNvmeConnector::do_single_exists(WorkerSpdkConn& conn,
                                         const std::string& key) {
  return conn.index->exists(key);
}

bool SpdkNvmeConnector::do_single_delete(WorkerSpdkConn& conn,
                                         const std::string& key) {
  return conn.index->erase(key);
}

size_t SpdkNvmeConnector::choose_num_tiles(lmcache::connector::Op op,
                                           size_t num_items) const {
  using lmcache::connector::Op;
  if (op == Op::BATCH_TILE_GET || op == Op::BATCH_TILE_SET) {
    // Route the whole batch to a single worker instead of splitting it into
    // up to num_workers tiles. do_batch_get/do_batch_set below pipeline the
    // full batch's NVMe commands on that one worker's qpair, so depth comes
    // from pipelining rather than from fanning out across workers; workers
    // still run in parallel across *different* concurrent submits. This
    // trades tile-level fan-out for far fewer enqueue/dequeue operations on
    // ConnectorBase's shared dispatch queue, which profiling showed was
    // heavily contended (see the class-level comment on the declaration).
    return 1;
  }
  return lmcache::connector::ConnectorBase<WorkerSpdkConn>::choose_num_tiles(
      op, num_items);
}

void SpdkNvmeConnector::do_batch_set(WorkerSpdkConn& conn,
                                     const lmcache::connector::Request& req) {
  const size_t n = req.keys.size();
  size_t i = 0;
  while (i < n) {
    size_t wave = std::min(n - i, kMaxInFlightPerWorker);
    std::vector<IoCtx> ctxs(wave);

    // Stage 1: copy each item into its own buffer slice and submit all
    // writes for this wave before waiting on any of them.
    for (size_t j = 0; j < wave; ++j) {
      size_t idx = i + j;
      const std::string& key = req.keys[idx];
      const void* buf = req.buf_ptrs[idx];
      size_t len = req.buf_lens[idx];
      if (len > conn.nvme->slot_size_bytes) {
        throw std::runtime_error(
            "SpdkNvmeConnector: value length " + std::to_string(len) +
            " exceeds slot_size_bytes " +
            std::to_string(conn.nvme->slot_size_bytes));
      }
      uint64_t slot = conn.index->allocate(key, len);
      uint64_t lba = slot * conn.nvme->blocks_per_slot;
      uint32_t lba_count = static_cast<uint32_t>(
          (len + conn.nvme->sector_size - 1) / conn.nvme->sector_size);

      void* slice =
          static_cast<char*>(conn.dma_buf) + j * conn.nvme->slot_size_bytes;
      std::memcpy(slice, buf, len);
      size_t io_bytes = static_cast<size_t>(lba_count) * conn.nvme->sector_size;
      if (io_bytes > len) {
        std::memset(static_cast<char*>(slice) + len, 0, io_bytes - len);
      }

      int rc = spdk_nvme_ns_cmd_write(conn.nvme->ns, conn.qpair, slice, lba,
                                      lba_count, io_complete_cb, &ctxs[j], 0);
      if (rc != 0) {
        throw std::runtime_error(
            "SpdkNvmeConnector: failed to submit NVMe write");
      }
    }

    // Stage 2: poll this worker's qpair until every command in the wave
    // has completed. Unlike blocking_io, this drains up to `wave`
    // completions per submit round instead of exactly one.
    size_t done = 0;
    while (done < wave) {
      spdk_nvme_qpair_process_completions(conn.qpair, 0);
      done = 0;
      for (const auto& c : ctxs) {
        if (c.done) ++done;
      }
    }
    for (const auto& c : ctxs) {
      if (!c.success) {
        throw std::runtime_error(
            "SpdkNvmeConnector: NVMe write completed with error");
      }
    }

    i += wave;
  }
}

void SpdkNvmeConnector::do_batch_get(WorkerSpdkConn& conn,
                                     const lmcache::connector::Request& req) {
  const size_t n = req.keys.size();
  size_t i = 0;
  while (i < n) {
    size_t wave = std::min(n - i, kMaxInFlightPerWorker);
    std::vector<IoCtx> ctxs(wave);
    std::vector<bool> submitted(wave, false);

    // Stage 1: look up each key and submit its read. Lookup/submit
    // failures are per-key (matches ConnectorBase's default do_batch_get
    // error-tolerance contract) -- record them in per_key_results and mark
    // the slot pre-"done" so stage 2 doesn't wait on it.
    for (size_t j = 0; j < wave; ++j) {
      size_t idx = i + j;
      const std::string& key = req.keys[idx];
      size_t len = req.buf_lens[idx];

      SlotMeta meta;
      bool ok = conn.index->lookup(key, meta) && meta.length == len;
      if (!ok) {
        req.batch->per_key_results[req.start_idx + idx] = 0;
        fprintf(stderr,
                "[LMCache GET] key %s failed: not found or size mismatch\n",
                key.c_str());
        ctxs[j].done = true;
        continue;
      }

      uint64_t lba = meta.slot_idx * conn.nvme->blocks_per_slot;
      uint32_t lba_count = static_cast<uint32_t>(
          (len + conn.nvme->sector_size - 1) / conn.nvme->sector_size);
      void* slice =
          static_cast<char*>(conn.dma_buf) + j * conn.nvme->slot_size_bytes;

      int rc = spdk_nvme_ns_cmd_read(conn.nvme->ns, conn.qpair, slice, lba,
                                     lba_count, io_complete_cb, &ctxs[j], 0);
      if (rc != 0) {
        req.batch->per_key_results[req.start_idx + idx] = 0;
        fprintf(stderr, "[LMCache GET] key %s failed: submit error\n",
                key.c_str());
        ctxs[j].done = true;
        continue;
      }
      submitted[j] = true;
    }

    // Stage 2: poll until every submitted command in the wave completes.
    size_t done = 0;
    while (done < wave) {
      spdk_nvme_qpair_process_completions(conn.qpair, 0);
      done = 0;
      for (const auto& c : ctxs) {
        if (c.done) ++done;
      }
    }

    // Stage 3: copy each successfully-read slice into its output buffer.
    for (size_t j = 0; j < wave; ++j) {
      if (!submitted[j]) continue;  // already recorded as failed in stage 1
      size_t idx = i + j;
      if (!ctxs[j].success) {
        req.batch->per_key_results[req.start_idx + idx] = 0;
        fprintf(stderr, "[LMCache GET] key %s failed: read error\n",
                req.keys[idx].c_str());
        continue;
      }
      void* slice =
          static_cast<char*>(conn.dma_buf) + j * conn.nvme->slot_size_bytes;
      std::memcpy(req.buf_ptrs[idx], slice, req.buf_lens[idx]);
      req.batch->per_key_results[req.start_idx + idx] = 1;
    }

    i += wave;
  }
}

}  // namespace lmc_spdk
