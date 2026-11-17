Concurrency Model
=================

The TCP listener dispatches accepted connections to a fixed worker pool. There are 16 pending queue slots per configured worker; additional connections receive `ERR server busy`. A connected client occupies a worker until the request loop ends. Bounded socket waits allow shutdown to proceed.

`StorageEngine` serializes WAL writes and snapshots with `mutation_mu_`. Its `data_mu_` guards the primary index; cache operations execute under the corresponding index lock so a reader cannot repopulate the cache with a value superseded by a completed write. The cache and WAL also maintain their own mutexes.

Execution characteristics
-------------------------

- A nonblocking advisory lock on `.voltstore.lock` coordinates one active engine per data directory. Keep the lock file in place across restarts; the filesystem must provide local `flock` semantics.
- Each worker handles one connection at a time. The fixed worker pool provides bounded connection concurrency; event-driven I/O is an alternative for idle-connection-heavy workloads.
- The synchronization model uses `std::shared_mutex`; evaluate writer progress under sustained read contention with the target workload.
- ThreadSanitizer and stress tests exercise selected concurrency paths and reproducible schedules.

See [architecture](architecture.md) for lock order and ownership.
