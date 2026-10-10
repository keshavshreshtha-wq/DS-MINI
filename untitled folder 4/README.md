# LSM-Tree Storage Engine for an EV Battery Thermal Management System

A from-scratch Log-Structured Merge Tree in C11 (standard library only) that acts as the
high-speed data-acquisition layer for an AI-enabled micro-channel battery cooling system:
10 cylindrical 21700 Li-ion cells (C1..C10) stream temperature, voltage, current and
coolant data at 10 Hz each.

## Build & run

```bash
make            # or: gcc -std=gnu11 -O2 -Wall -Wextra -o lsm_demo *.c -lm
./lsm_demo                  # full dashboard demo, 25,000 readings
./lsm_demo 100000           # bigger run
./lsm_demo --scaling        # measure complexity as N grows (10^4 .. ~10^6)
./lsm_demo --no-color       # plain output (for logs / Windows consoles without ANSI)
```
Needs only gcc/clang and libm. Data is written to `./lsm_data/` and deleted at the end.

## Files

| File | Role |
|---|---|
| `record.h`   | 32-byte sensor record; key = `(timestamp_ms << 4) \| cell_id` |
| `skiplist.*` | **MemTable** - sorted skip list (O(1) tail-append fast path) |
| `bloom.*`    | **Bloom filter** - double-hashing bit array |
| `wal.*`      | **Write-Ahead Log** - checksummed append-only file + replay |
| `sstable.*`  | **SSTable** - immutable sorted file, header + Bloom + records, sparse index |
| `lsm.*`      | Engine: write path, flush, compaction, point lookup, range query, tombstones |
| `datagen.*`  | Physics-based simulator of the 10 cells + coolant loop |
| `main.c`     | Benchmark script and console dashboard |

## Architecture

```
 put(reading) ──► WAL (append, sequential) ──► MemTable (skip list, RAM)
                                                  │ >= 2000 entries
                                                  ▼
                                   FLUSH ──► SSTable L0  (immutable file)
                                                  │ 4 tables in a level
                                                  ▼
                                   COMPACTION ──► 1 bigger SSTable at L+1
                                                  (newest version wins, tombstones purged)

 get(key) ──► MemTable ──► for each SSTable, newest first:
                            key-range check ─► Bloom filter ─► sparse index ─► ONE 32-record block read
 range(t1,t2) ──► seek in MemTable + every overlapping SSTable ─► K-way merge (newest wins)
```

SSTable file: `[48-byte header | Bloom bits | sorted 32-byte records]`. The sparse index
(every 32nd key) is rebuilt in RAM when a table is opened.

## Complexity - what is proven, and the honest fine print

| Operation | Claim | Notes |
|---|---|---|
| Insert | **O(1)** | WAL append is O(1). The MemTable is a skip list, whose general insert is O(log m). Because sensor timestamps are monotonic, inserts take the **tail-append fast path: O(1) expected**. Out-of-order samples fall back to O(log m). `--scaling` and the MemTable micro-benchmark show ordered cost staying flat while random-order cost grows. Flush/compaction cost is amortised O(1) per record. |
| Point lookup | **O(log N)** | MemTable O(log m); per SSTable: range check O(1), Bloom O(k), sparse-index binary search O(log n), one block read O(32). Number of SSTables is O(log N) for size-tiered compaction. |
| Range search | **O(K + log N)** | O(log N) seeks (one per source) + K output records. Strictly, the K-way merge costs O(K·R) for R sources; R is small and bounded, so it is O(K) for fixed R (a heap would give O(K log R)). |
| Delete | **O(1)** | Lazy: one WAL append + one tombstone insert (also O(1) when it lands at the tail, otherwise O(log m)). Nothing on disk is rewritten; space is reclaimed by compaction. |

Other design points you should be able to explain:
* **Write-ahead rule:** WAL append happens *before* the MemTable insert. After a flush the SSTable
  and manifest are durable *before* the WAL is truncated. Phase 7 of the demo kills the engine
  without flushing and recovers from the WAL.
* **`fflush` vs `fsync`:** by default each WAL append is flushed to the OS (survives a process crash).
  `wal_fsync = 1` survives power loss but costs far more per write.
* **Tombstone purge rule:** a tombstone may be dropped only when no *older* table could still hold
  the key. Minor compactions purge only if nothing exists in deeper levels; `lsm_compact_all` (major
  compaction) purges all.
* **Bloom filter:** 10 bits/key, k=7 -> ~0.8% false positives (the demo measures it). No false negatives.
* **"Background" compaction:** `lsm_compaction_tick()` runs synchronously right after each flush so
  the output is deterministic and easy to follow. Moving it to a worker thread needs a mutex around
  the table list - listed under extensions.
* **Benchmark caveat:** the files sit in the OS page cache, so "disk" lookups are faster than a cold
  SSD/HDD read (which is 100 us to ms). The RAM-vs-disk *ratio* and the Bloom-filter effect are real;
  absolute disk numbers are optimistic.

## Possible extensions
Min-heap merge (O(K log R)), leveled compaction, block cache, per-block compression, multi-threaded
compaction, delta/Gorilla encoding for float time-series, an `fsync` group-commit WAL.
