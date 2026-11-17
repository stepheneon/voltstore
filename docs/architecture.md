Architecture
============

VoltStore uses one in-memory `std::unordered_map` as the primary index, with a WAL and snapshots for persistence. `StorageEngine` owns storage state; `TcpServer` owns the listener and the connection worker pool.

Data flow
---------

1. The TCP listener assigns each connection to a bounded worker pool.
2. The command parser checks syntax and passes valid operations to `StorageEngine`.
3. `PUT` and `DELETE` serialize WAL writes before updating the index and cache. If index publication fails after the WAL append, the instance stops accepting operations until it is reopened.
4. `GET` holds the index read lock through cache access, keeping it consistent with completed writes.
5. `SNAPSHOT` writes and synchronizes a replacement checkpoint before truncating the WAL.
6. Recovery loads the snapshot and replays complete WAL records; an incomplete trailing record is repaired.

[Component diagram](diagrams/architecture.mmd) · [Recovery sequence](diagrams/recovery.mmd)

Locking and resource ownership
------------------------------

Mutations and snapshots

- Owner / synchronization: `StorageEngine::mutation_mu_`

Primary index

- Owner / synchronization: `StorageEngine::data_mu_` (shared/exclusive)

LRU list and lookup map

- Owner / synchronization: `LruCache::mu_`

Data directory

- Owner / synchronization: `DataDirectoryLock`, held until the engine closes

WAL descriptor and writes

- Owner / synchronization: `Wal::mu_` and RAII descriptor cleanup

Network connections

- Owner / synchronization: Per-connection descriptor guard

Worker threads

- Owner / synchronization: `ThreadPool`, joined during destruction


Writers acquire the mutation lock before the index write lock, then use the cache lock as needed. Reads hold the index read lock before accessing the cache. The WAL append precedes the index mutation and is performed outside the index lock. Once the WAL append returns, an index-update failure marks the engine for recovery before releasing its exclusive index lock. The optional `EngineConfig::after_wal_append_hook` enables deterministic regression tests at the post-WAL publication boundary.

Operation costs
---------------

GET / EXISTS

- Expected complexity: O(1) average hash lookup
- Qualification: Worst-case O(n) collisions; copied value cost scales with bytes

PUT / DELETE

- Expected complexity: O(1) average index work
- Qualification: WAL encoding and I/O scale with record length; `fsync` can dominate latency

SCAN prefix

- Expected complexity: O(n log(k + 1) + k log(k + 1))
- Qualification: Scans n keys, retains up to k results, O(k) auxiliary entries plus value copies

Snapshot

- Expected complexity: O(n + total serialized bytes)
- Qualification: Copies the index before writing the checkpoint

WAL replay

- Expected complexity: O(B)
- Qualification: Reads and validates B bytes of WAL records

LRU lookup/update

- Expected complexity: O(1) average
- Qualification: Duplicates values already held by the in-memory index


The LRU adds bounded recency tracking alongside the in-memory primary index. Its performance contribution is workload-dependent and should be evaluated with representative measurements.

For file formats, recovery ordering, and operating assumptions, see [storage format](storage-format.md), [WAL recovery](wal-recovery.md), and [recovery model](recovery-model.md). Specific choices and alternatives are recorded in the [ADRs](adr/ADR-003-wal.md).
