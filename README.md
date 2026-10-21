# lsm-engine-c
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) [![CI](https://github.com/sohamshewani/lsm-engine-c/actions/workflows/ci.yml/badge.svg)](https://github.com/sohamshewani/lsm-engine-c/actions) [![Security: Hardened](https://img.shields.io/badge/Security-Hardened-blue.svg)](https://github.com/sohamshewani/lsm-engine-c) [![Stars](https://img.shields.io/github/stars/sohamshewani/lsm-engine-c?style=flat)](https://github.com/sohamshewani/lsm-engine-c)

## Executive Summary & Problem Space

`lsm-engine-c` is a production-grade, high-throughput Log-Structured Merge-tree (LSM) storage engine engineered from the ground up in modern C (C11). Designed for zero-allocation hot paths and deterministic memory bounds, it addresses the fundamental mechanical limitations of traditional B-Tree indices—namely, random I/O write amplification, lock contention on high-concurrency multi-core processors, and fragmentation under heavy update workloads.

In high-throughput distributed systems and local transactional stores, synchronous random disk writes severely bottleneck throughput due to head movement and page cache thrashing. `lsm-engine-c` eliminates random write bottlenecks by appending all mutations sequentially to a Write-Ahead Log (WAL) and buffering writes in an in-memory, lock-free or fine-grained concurrent skip-list/memtable structure. Background compaction threads systematically stream sorted runs down to immutable, disk-resident Sorted String Tables (SSTables), providing predictable write latency profiles, high cache locality, and enterprise-grade durability.

---

## Structural ASCII Architecture / Dataflow Diagram

The engine architecture decouples the synchronous ingestion path from asynchronous persistence and background structural maintenance.

```
                              +---------------------------------------+
                              |              Client Thread            |
                              +---------------------------------------+
                                        |                   |
                                (1) Write(k,v)      (2) Read(k)
                                        v                   v
+-----------------------+     +---------------------------------------+     +-----------------------+
|    Write-Ahead Log    | <-- |                 Engine Core           | --> |     Active Memtable   |
|        (WAL)          |     |          (include/lsm_engine.h)       |     |    (src/memtable.c)   |
+-----------------------+     +---------------------------------------+     +-----------------------+
                                        |                             \           | (Full)
                                        |                              \----------+
                                        v                                         v
                              +--------------------+                    +-------------------+
                              | Immutable Memtable |                    | Background Worker |
                              +--------------------+                    +-------------------+
                                        |                                         |
                                        +-------------------+---------------------+
                                                            |
                                                   (3) Flush & Compaction
                                                   (src/compaction.c)
                                                            v
                                             +-----------------------------+
                                             |     Disk-Resident SSTables  |
                                             |       (src/sstable.c)       |
                                             +-----------------------------+
```

### Dataflow Execution Path
1. **Write Path (`lsm_put`)**:
   - The mutation is serialized and appended to the disk-backed `WAL` (`src/wal.c`) for crash recovery.
   - Upon successful fsync/append, the key-value pair is inserted into the Active Memtable (`src/memtable.c`).
   - If the active memtable exceeds capacity, it is marked immutable and scheduled for disk flushing, while a new active memtable is allocated.
2. **Read Path (`lsm_get`)**:
   - The engine checks the Active Memtable. If not found, it queries any Immutable Memtables currently awaiting flush.
   - If a miss occurs across memory, the engine traverses disk-resident SSTables (`src/sstable.c`) level-by-level, utilizing Bloom filters to prune unnecessary disk access.
3. **Compaction Path (`lsm_compact`)**:
   - Background threads execute tiered or leveled compaction algorithms (`src/compaction.c`), merging overlapping SSTables, purging tombstones, and rewriting sequentially to minimize space amplification.

---

## Concurrency Model & Memory Invariants

`lsm-engine-c` utilizes a hybrid concurrency model combining hazard-pointer-free read paths with exclusive-writer locks for memtable rotation, minimizing global synchronization overhead.

* **Thread Safety**: 
  - Read operations (`lsm_get`, iterator scans) are lock-free with respect to active writes, utilizing immutable snapshot semantics on memtables and reference-counted file descriptors for SSTables.
  - Write operations (`lsm_put`, `lsm_delete`) acquire a lightweight mutex guarding memtable insertion pointers and WAL file handles.
* **Memory Ownership & Zero-Copy Boundaries**:
  - Callers retain ownership of input buffers during write calls; the engine internalizes data via explicit deep copies into memtable arenas.
  - Read results returned via `lsm_get` point directly to memtable or pinned block-cache memory. Callers must invoke engine-provided release hooks to prevent use-after-free conditions.
* **Atomic Primitives**:
  - State transitions (e.g., memtable swap triggers, compaction status flags) rely on C11 `<stdatomic.h>` memory barriers (`memory_order_acquire` and `memory_order_release`) to guarantee cross-core visibility without heavy kernel-level futex contention.

---

## Performance Characteristics & Algorithmic Complexity Table

| Operation | Average Case | Worst Case | Latency Target (p99) | Description |
| :--- | :--- | :--- | :--- | :--- |
| **Point Write (`Put`)** | $O(1)$ | $O(\log N)$ | $< 150\mu\text{s}$ | Append to WAL + insert into in-memory memtable. Worst case triggers memtable flush. |
| **Point Read (`Get`)** | $O(\log M)$ | $O(D \cdot \log S)$ | $< 500\mu\text{s}$ | Memtable lookup ($O(\log M)$) fallback to Bloom filter checks and binary search across $D$ SSTables. |
| **Range Scan (`Scan`)** | $O(\log M + K)$ | $O(D \cdot \log S + K)$ | $< 2\text{ms}$ (per 100 keys) | Merged iterator over memtables and active SSTables yielding $K$ sorted keys. |
| **Compaction (`Merge`)** | $O(N)$ background | $O(N \log N)$ | Background | Multi-way merge sort of SSTables running asynchronously out-of-band. |

*Notation*: $M$ = keys in memory, $D$ = depth of SSTable levels, $S$ = keys per SSTable, $N$ = total dataset size.

---

## Security Architecture & Threat Model

Designed for integration into security-critical infrastructure, `lsm-engine-c` enforces strict defensive coding paradigms:

* **Input Boundaries & Sanitization**:
  - All external API inputs (`include/lsm_types.h`, `include/lsm_engine.h`) undergo rigorous bounds checking. Key and value lengths are strictly validated against pre-defined maximums to prevent integer overflows and stack/heap buffer overflows.
* **Memory Corruption Protections**:
  - The codebase avoids unsafe standard library functions (e.g., `strcpy`, `sprintf`), mandating bounds-checked alternatives (`memcpy_s`-style patterns).
  - Explicit pointer sanitization and lifecycle tracing eliminate double-frees and dangling references during compaction error unwinding.
* **Threat Vectors & Mitigations**:
  - *Denial of Service (Disk Filling)*: Mitigated by enforcing strict memtable limits and backpressure signals when compaction threads fall behind ingestion rates.
  - *Data Corruption (Bit rot/Crash inconsistency)*: Mitigated by cryptographic checksums (CRC32c) embedded in WAL records and SSTable block headers, verified automatically upon reading.

---

## Build, Verification & Benchmarking Recipes

Ensure you have a modern C11 compliant compiler (`gcc` or `clang`), `make`, and POSIX development utilities installed.

### 1. Compilation
Compile the core engine static library, invariant test suites, and benchmarking harnesses in Debug or Release modes:
```bash
# Clean previous build artifacts
make clean

# Build optimized release binaries
make BUILD_MODE=release all
```

### 2. Verification & Invariant Testing
Execute the rigorous test harness (`tests/test_invariants.c`) to validate crash recovery, concurrency safety, and structural consistency:
```bash
# Run automated invariant and stress test suites
make test
```

### 3. Execution of Benchmarks
Run the high-concurrency performance harness (`benchmarks/bench_engine.c`) to measure throughput (Ops/sec) and latency distributions across mixed workloads:
```bash
# Execute the benchmarking harness
make benchmark