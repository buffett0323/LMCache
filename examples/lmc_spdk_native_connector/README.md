# LMCache SPDK NVMe Native Connector

An **SPDK-based native storage backend** for LMCache (proposed in
[issue #4113](https://github.com/LMCache/LMCache/issues/4113)). It stores KV
cache chunks **directly on an NVMe SSD through SPDK's userspace polling
driver**, bypassing the kernel block stack, and exposes the standard LMCache
native-connector interface so it can be benchmarked head-to-head against the
built-in `fs_native` adapter.

It subclasses the same C++ `ConnectorBase<T>` that the built-in Redis/FS
connectors use, so it inherits the worker-thread pool, batching, and
eventfd-driven completion signalling; only the four I/O primitives
(`create_connection`, `do_single_get/set/exists`) are SPDK-specific.

## How it works

The NVMe namespace is a raw block device with no filesystem. The connector:

1. Attaches the controller with the SPDK userspace NVMe driver at construction.
2. Carves the namespace into fixed-size **slots** (`slot_size_bytes`, rounded
   up to the sector size).
3. Keeps an **in-memory index** `key -> {slot, length}` (mutex-protected).
4. Per worker thread, allocates one NVMe **I/O queue pair** and one DMA bounce
   buffer (`spdk_zmalloc`). SPDK requires a qpair be used by a single thread;
   `ConnectorBase` gives each worker its own connection, satisfying that.
5. `set` copies into the bounce buffer and issues `spdk_nvme_ns_cmd_write`;
   `get` issues `spdk_nvme_ns_cmd_read` and copies out. Each op polls its
   qpair to completion.

> **Note.** The index is **in-memory only**, so cached data does not survive a
> restart. This is intentional and sufficient for the `fs_native` benchmark
> comparison; a production backend would persist the index in a reserved
> metadata region of the namespace. The bounce-buffer copy also means this is
> not yet fully zero-copy — registering the L1 buffer with SPDK is a future
> optimization (see *Limitations*).

## Requirements

- **Linux** (SPDK + eventfd).
- **SPDK** built with shared libraries and discoverable via `pkg-config`.
- **Hugepages** configured.
- A **dedicated NVMe device** bound to the SPDK userspace driver. **All data on
  it is destroyed** — do not point this at a disk with data you care about.

## Build

```bash
# 1. Build SPDK (once)
git clone https://github.com/spdk/spdk && cd spdk
git submodule update --init
./configure --with-shared
make -j
sudo make install         # or: export PKG_CONFIG_PATH=$PWD/build/lib/pkgconfig

# 2. Reserve hugepages and bind the NVMe to the userspace driver
sudo HUGEMEM=4096 scripts/setup.sh
scripts/setup.sh status   # note the PCIe address, e.g. 0000:00:04.0

# 3. Build this plugin (from the LMCache repo)
cd examples/lmc_spdk_native_connector
pip install -e .
```

If `pkg-config` cannot find `spdk_nvme` / `spdk_env_dpdk`, the native
extension is skipped with a warning; set `PKG_CONFIG_PATH` and reinstall.

## Configure

Load via the `native_plugin` L2 adapter type:

```json
{
  "type": "native_plugin",
  "module_path": "lmc_spdk_native_connector",
  "class_name": "SpdkNativeConnector",
  "adapter_params": {
    "pci_addr": "0000:00:04.0",
    "slot_size_bytes": 1048576,
    "num_workers": 8,
    "nsid": 1
  }
}
```

| Param | Type | Default | Description |
|---|---|---|---|
| `pci_addr` | str | — | PCIe address of the NVMe device (required). |
| `slot_size_bytes` | int | — | Fixed per-key slot size; must be ≥ the largest chunk stored. Rounded up to the sector size (required). |
| `num_workers` | int | 8 | I/O worker threads == NVMe queue depth. |
| `nsid` | int | 1 | NVMe namespace id. |

Size `slot_size_bytes` from your benchmark payload: it must be ≥
`--data-size-kb * 1024`.

## Benchmark against `fs_native`

Both backends run through the **same** `lmcache bench l2` harness — swap only
the adapter JSON. This produces the apples-to-apples comparison issue #4113
asks for.

```bash
# Baseline: fs_native
lmcache bench l2 \
  --l2-adapter '{"type":"fs_native","base_path":"/data/nvme/l2","num_workers":8,"use_odirect":true}' \
  --l1-align-bytes 4096 --num-keys 64 --data-size-kb 32 --in-flight 8 \
  --rounds 5 --warmup-rounds 2

# Candidate: SPDK
lmcache bench l2 \
  --l2-adapter '{"type":"native_plugin","module_path":"lmc_spdk_native_connector","class_name":"SpdkNativeConnector","adapter_params":{"pci_addr":"0000:00:04.0","slot_size_bytes":1048576,"num_workers":8}}' \
  --num-keys 64 --data-size-kb 32 --in-flight 8 --rounds 5 --warmup-rounds 2
```

Sweep `--in-flight` (queue depth), `--num-keys`, and `--data-size-kb` to cover
prefill-like (large sequential store) and decode-like (small random load)
patterns. SPDK's advantage is expected mainly at high queue depth / concurrent
small random reads. Add `--no-skip-verify` for a correctness pass.

## Limitations / future work

- **In-memory index** — no crash/restart persistence (benchmark scope).
- **Bounce-buffer copy** — not yet zero-copy; register the L1 buffer with SPDK
  (`spdk_mem_register` / pinned hugepage L1) to remove the `memcpy`.
- **Fixed-size slots** — space-inefficient for highly variable chunk sizes; a
  real allocator (extent/bitmap) would pack better.
- **Single in-flight op per call** — `blocking_io` polls one command to
  completion; deeper per-worker pipelining could raise IOPS further.
