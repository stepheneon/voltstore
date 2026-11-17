# VoltStore — Engineering questions

C++20 storage, concurrency, persistence, and recovery

VoltStore began with a simple question: what should a key-value service promise when a request, a write, or a process fails? The design questions below trace the decisions behind the index, log, snapshots, and network interface. I have focused on concrete behavior and the tests that exercise it, rather than broad claims about reliability.

The answers link directly to the implementation and tests, so a reader can follow the reasoning into the code.

## System architecture and correctness model

**1. What engineering problem does VoltStore make concrete?**

The public API looks small—put a value, read it, delete it—but the difficult part is deciding what each operation means when execution is interrupted. A write can reach the log before the index changes; a reader can overlap an update; a process can stop between snapshot steps. I built VoltStore around those boundaries. The hash index answers current reads, the WAL records mutations for replay, snapshots provide checkpoints, and synchronization defines which version another thread can observe. The central goal is a state model that can be explained and tested across normal operation and restart.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`docs/architecture.md`](../docs/architecture.md) · [`src/wal.cpp`](../src/wal.cpp)

**2. Why use a single-node architecture for this design?**

A single-node design lets the persistence and concurrency rules be studied without conflating them with replication and consensus. One process owns one data directory; threads coordinate through explicit locks; recovery reads the same local files the previous process wrote. That scope is a design choice, not a shortcut hidden in the documentation. It makes questions about acknowledgment, partial writes, checkpoints, and restart precise enough to test. Extending the system to multiple nodes would require new ownership and ordering rules rather than a simple change to the network listener.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`src/data_directory_lock.cpp`](../src/data_directory_lock.cpp) · [`tests/test_recovery.cpp`](../tests/test_recovery.cpp)

**3. Why is C++20 appropriate for the engine's execution model?**

C++20 fits the parts of this project where resource lifetime and concurrent execution matter. RAII makes file descriptors and locks easier to reason about across exceptions. Standard containers provide a well-understood primary index, while shared and exclusive locking express the reader/writer access pattern. I use the language facilities to make ownership and synchronization visible, not simply to maximize the number of language features on display. The CMake targets and sanitizer builds also make these choices inspectable in compiled code.

Code and tests: [`docs/adr/ADR-001-why-cpp.md`](../docs/adr/ADR-001-why-cpp.md) · [`include/voltstore/storage_engine.h`](../include/voltstore/storage_engine.h) · [`CMakeLists.txt`](../CMakeLists.txt)

**4. Why split the engine, WAL, snapshot, protocol, and thread pool?**

I separated the engine, WAL, snapshot, protocol, and thread pool because they answer different questions. The engine owns operation semantics and the current index. WAL defines a record stream and replay. Snapshot code handles checkpoint bytes and publication. The protocol parses requests; the server manages connections and scheduling. The interfaces matter most at the transitions: when an append becomes visible, when a checkpoint makes older log records redundant, and when a request is acknowledged. Keeping those responsibilities apart makes the failure paths easier to isolate in tests.

Code and tests: [`include/voltstore/`](../include/voltstore/) · [`docs/architecture.md`](../docs/architecture.md) · [`src/protocol.cpp`](../src/protocol.cpp)

**5. What are the principal state invariants?**

The main invariants are about state authority and order. The index is the current in-process view; the WAL and a validated snapshot are the material from which a new process reconstructs it. Mutations are serialized for log ordering, readers see index updates through the data lock, and the cache must follow the index rather than define a different truth. A recovery-required state blocks normal operations after a particularly dangerous partial transition. I treat those invariants as questions for tests, especially at the points where one representation is updated before another.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`include/voltstore/wal.h`](../include/voltstore/wal.h) · [`docs/recovery-model.md`](../docs/recovery-model.md)

## Indexing, expiration, and caching

**6. Why choose `std::unordered_map` as the primary index?**

Point lookups are central to the selected workload, so `std::unordered_map` is a practical primary index with expected constant-time access. That choice is not free: ordered iteration and prefix searches do not come naturally from a hash table. I kept the main index simple and made the cost of scanning explicit rather than introducing a larger tree implementation without a demonstrated need. The relevant comparison is workload and complexity, not which data structure sounds more sophisticated.

Code and tests: [`docs/adr/ADR-004-index.md`](../docs/adr/ADR-004-index.md) · [`src/storage_engine.cpp`](../src/storage_engine.cpp)

**7. How are prefix scans bounded without sorting the full key space?**

A bounded result set does not automatically make a scan independent of total key count. VoltStore examines candidate keys in the hash index and retains a limited set of matching results, avoiding a full sort of every key for a small requested prefix result. The scan still inspects the underlying key space. I prefer to state both facts: output is bounded, but the search is still tied to the number of indexed entries. If prefix queries became the dominant workload, I would reassess the primary indexing strategy rather than conceal that trade-off.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`include/voltstore/storage_engine.h`](../include/voltstore/storage_engine.h)

**8. Why store absolute expiration times rather than remaining TTL?**

A remaining-TTL counter would reset its meaning when the process exits. VoltStore records an absolute expiration timestamp with the entry, so a restarted process can decide whether the key should still be visible. Reads and recovery compare that deadline against the current time. The important contract is the expiration boundary, not just the presence of a timer. Persisting the deadline also makes replay deterministic relative to a stated clock value.

Code and tests: [`include/voltstore/common.h`](../include/voltstore/common.h) · [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`docs/storage-format.md`](../docs/storage-format.md)

**9. What purpose does the LRU cache serve when the index is already in memory?**

At first glance, an LRU cache beside an in-memory hash index looks redundant. The cache instead serves as a bounded recency structure that can be evaluated separately from the authoritative index. It keeps a list for usage order and a hash lookup for entries, with average constant-time cache operations. Writes and deletes must update or invalidate cached values so recency cannot turn into stale state. I do not assume the cache is faster merely because it exists; its value depends on hit rate, contention, allocation overhead, and the workload being measured.

Code and tests: [`src/lru_cache.cpp`](../src/lru_cache.cpp) · [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`tests/test_concurrency.cpp`](../tests/test_concurrency.cpp)

**10. How does the design prevent stale cache values during concurrent access?**

A stale-cache race is easy to describe: a reader observes an old value, a writer publishes a new value, and the reader then caches what it saw earlier. The read path therefore holds its shared data lock through cache inspection and population. Writers take the exclusive lock while updating the index and coordinating cache state. That gives a concrete ordering between a completed mutation and subsequent reads. A concurrency regression test targets this interleaving instead of assuming a mutex in each class must make the overall behavior correct.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`src/lru_cache.cpp`](../src/lru_cache.cpp)

## Write-ahead logging and mutation semantics

**11. Why use write-ahead logging rather than rely on the in-memory map?**

An in-memory map is a useful working state, but it cannot reconstruct itself after process loss. The write-ahead log stores an ordered history of PUT and DELETE records that can be replayed when a new engine starts. Choosing a log also means taking record framing, partial writes, checksums, and synchronization seriously. I wanted the storage contract to be defined by the bytes recoverable after restart, rather than by the fact that a map update returned successfully during one run.

Code and tests: [`docs/adr/ADR-003-wal.md`](../docs/adr/ADR-003-wal.md) · [`src/wal.cpp`](../src/wal.cpp) · [`src/storage_engine.cpp`](../src/storage_engine.cpp)

**12. Why append the WAL before publishing the in-memory mutation?**

The mutation must be recorded before it becomes visible in the index. Otherwise a caller could observe an update that a restarted process has no way to recover. Under the mutation lock, VoltStore appends the WAL record and then publishes the change under the exclusive data lock. There is still a gap between those steps. If publication throws after a completed append, the engine enters its recovery-required state. I designed that behavior around the intermediate state rather than assuming the two actions form an automatic transaction.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`docs/adr/ADR-003-wal.md`](../docs/adr/ADR-003-wal.md)

**13. What does a successful `PUT` acknowledgment establish?**

An `OK` response means the engine operation has returned through its configured write path. With `sync_writes=true`, that path appends the WAL record and requests `fsync` before the index update completes. With synchronization disabled, the completion boundary is different: the record was appended through the selected code path without per-write `fsync`. Those settings belong in the API and benchmark discussion because they describe different persistence conditions. The server does not acknowledge success merely because it parsed the command.

Code and tests: [`include/voltstore/storage_engine.h`](../include/voltstore/storage_engine.h) · [`src/wal.cpp`](../src/wal.cpp) · [`src/server.cpp`](../src/server.cpp)

**14. Why does the on-disk record format carry a header and checksum?**

A recovery parser has to decide whether bytes describe a complete, supported operation. The VLT1 format supplies version and operation fields, bounded lengths, an expiry timestamp, payload bytes, and CRC32. The fixed header allows the reader to check sizes before using variable-length data; the checksum helps detect accidental corruption of complete records. I use the checksum as an integrity check, not as a substitute for authentication. Record layout is part of the recovery design because replay is only as sound as the data it is willing to interpret.

Code and tests: [`docs/storage-format.md`](../docs/storage-format.md) · [`src/wal.cpp`](../src/wal.cpp) · [`src/crc32.cpp`](../src/crc32.cpp)

**15. How are short writes and interrupted system calls handled?**

`write` can return after transferring fewer bytes than requested, or it can be interrupted. WAL uses a loop that tracks the completed offset and continues until the whole record is written or an error stops progress. If an append cannot finish safely, that writer is marked failed rather than proceeding as though the record were atomic. Recovery must then inspect the resulting log, including any partial tail. This is why I treat the return value of each system call as part of the persistence algorithm.

Code and tests: [`src/wal.cpp`](../src/wal.cpp) · [`include/voltstore/wal.h`](../include/voltstore/wal.h)

**16. What does it mean when the WAL append succeeds but index publication fails?**

Suppose append succeeds but the index update throws. The log now contains an operation that the live map may not show. Continuing to serve requests would let those two representations disagree. VoltStore marks the engine recovery-required and prevents normal operations until reopening rebuilds state from the snapshot and WAL. The post-append test hook creates this exact condition for PUT and DELETE, so the test can inspect both the immediate error state and the result after restart. This boundary was one of the most consequential design choices in the engine.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`tests/test_recovery.cpp`](../tests/test_recovery.cpp) · [`tests/test_concurrency.cpp`](../tests/test_concurrency.cpp)

**17. How are WAL I/O errors different from post-WAL index errors?**

I distinguish an error during WAL I/O from an error after WAL append has returned. During I/O, the writer may have made partial progress, so it records a failed state and blocks more appends. After a successful append, the dangerous ambiguity is between persisted history and the live index; that path sets the engine's recovery-required flag. Both lead to startup validation, but they are not the same state transition. Treating every exception as an ordinary rollback would hide precisely the evidence recovery needs to examine.

Code and tests: [`src/wal.cpp`](../src/wal.cpp) · [`src/storage_engine.cpp`](../src/storage_engine.cpp)

## Snapshots, reconstruction, and recovery

**18. Why introduce snapshots if WAL can replay every mutation?**

Without snapshots, every restart would replay the entire retained mutation history. A checkpoint records the current state and lets recovery apply only subsequent log records. That improves the shape of restart work while introducing a new dependency: the checkpoint must be complete and published before the older log history becomes disposable. I therefore view snapshotting as an ordering protocol between two persistent artifacts, not simply a serialization function.

Code and tests: [`src/snapshot.cpp`](../src/snapshot.cpp) · [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`docs/storage-format.md`](../docs/storage-format.md)

**19. Why write a temporary snapshot and rename it atomically?**

Writing directly over the only valid snapshot creates an obvious interruption risk. VoltStore writes a replacement under a temporary name, synchronizes its content, then renames it into place and synchronizes the containing directory. The file and the directory have different persistence responsibilities. A test that forces temporary-file creation to fail checks that the existing recoverable state remains available. The sequence matters more than calling `rename` and declaring the checkpoint durable.

Code and tests: [`src/snapshot.cpp`](../src/snapshot.cpp) · [`tests/test_recovery.cpp`](../tests/test_recovery.cpp)

**20. Why must WAL truncation follow successful snapshot publication?**

The safe order is publish a valid snapshot first, then truncate the WAL. Doing it the other way around could remove the only history containing changes absent from the old checkpoint. An interruption after snapshot publication but before truncation can leave some mutations represented twice. For PUT and DELETE, replaying those state-setting operations is compatible with the chosen reconstruction model; an increment or append operation would need different rules. I kept the operation set and replay contract aligned with that ordering.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`src/snapshot.cpp`](../src/snapshot.cpp) · [`src/wal.cpp`](../src/wal.cpp)

**21. How does restart reconstruct the visible index?**

On startup, the engine obtains directory ownership, opens the WAL, loads a validated snapshot, and replays complete log records in sequence. It removes keys whose absolute expiry deadlines have passed and applies PUT or DELETE to rebuild the visible index. The result comes from persisted state, not from trying to recover the old process's memory layout. The recovery report records the replayed operations and whether it encountered an incomplete final tail, making the reconstruction path inspectable.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`src/wal.cpp`](../src/wal.cpp) · [`include/voltstore/wal.h`](../include/voltstore/wal.h)

**22. Why treat an incomplete log tail differently from a complete corrupt record?**

An incomplete final record can result from a process stopping during append. That is different from finding a fully framed record with a checksum mismatch. VoltStore treats an incomplete trailing record as a recoverable end-of-log condition under its replay rules, while a complete corrupted record is an integrity failure that should not be quietly skipped. The distinction protects the valid prefix without teaching recovery to ignore arbitrary damage. I use byte-level fixtures to check both cases, because a single generic 'corrupt file' test would hide the policy difference.

Code and tests: [`src/wal.cpp`](../src/wal.cpp) · [`tests/test_recovery.cpp`](../tests/test_recovery.cpp)

**23. How are malformed or inconsistent snapshots handled?**

A checkpoint should not be trusted merely because its pathname exists. Snapshot loading validates the file structure, entry bounds, checksums, and expected layout before exposing reconstructed state. Malformed records are rejected rather than partially installed as a new index. Tests damage or truncate snapshot data to make sure the loader notices. This keeps recovery anchored to explicit input conditions; the same caution applies to normal network requests, but here the consequence is which state a restarted process can safely publish.

Code and tests: [`src/snapshot.cpp`](../src/snapshot.cpp) · [`tests/test_recovery.cpp`](../tests/test_recovery.cpp)

**24. Why are replayed PUT and DELETE operations useful for recovery reasoning?**

PUT and DELETE have a useful property for this recovery model: replaying a state-setting operation again produces the same final value or absence for that key. That matters when a published snapshot and an untruncated WAL overlap. I still keep log order, because two different operations on the same key must be applied in sequence. This is a property of the chosen operation semantics, not a general promise that any future command can be replayed twice. Richer mutations would require additional identity or deduplication rules.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`docs/wal-recovery.md`](../docs/wal-recovery.md)

## Concurrency and scheduling

**25. Why use a fixed-size thread pool with a bounded queue?**

A thread per connection gives an easy prototype but leaves resource consumption tied to incoming connections. VoltStore uses a fixed worker count and a bounded task queue so execution and waiting work have explicit limits. Saturation can then be handled as an observable condition instead of silently creating more threads. The trade-off is queueing latency and how long a worker remains occupied by one connection. That makes worker count, queue capacity, and timeout behavior part of the server's performance model.

Code and tests: [`docs/adr/ADR-002-thread-pool.md`](../docs/adr/ADR-002-thread-pool.md) · [`src/thread_pool.cpp`](../src/thread_pool.cpp) · [`tests/test_concurrency.cpp`](../tests/test_concurrency.cpp)

**26. How do the engine's locks divide responsibility?**

I use different locks for different invariants. The mutation lock serializes writes that must have one WAL order. The data lock governs visible index state and the cache relationship, allowing shared reads and exclusive publication. Cache internals have their own synchronization responsibilities. What matters is the order in which these locks can be acquired and which state each one protects. A lock map is more useful here than treating 'thread-safe' as a label for each individual class.

Code and tests: [`include/voltstore/storage_engine.h`](../include/voltstore/storage_engine.h) · [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`src/lru_cache.cpp`](../src/lru_cache.cpp)

**27. Why lock the data directory as well as the in-process index?**

A mutex protects threads inside one process, not two independent programs opening the same files. VoltStore therefore holds an exclusive data-directory lock for the engine's lifetime, rejecting a second active owner. The implementation takes care with the lock file path, including symlink-related cases, and the tests cover attempted concurrent ownership and reopening after release. That is a different invariant from ordinary read/write synchronization: it protects the persistence history itself from competing processes.

Code and tests: [`src/data_directory_lock.cpp`](../src/data_directory_lock.cpp) · [`src/storage_engine.cpp`](../src/storage_engine.cpp)

**28. What is the role of exception safety in the cache and index path?**

Not every exception threatens the same state. A failed index publication after WAL append may leave persisted and visible state out of step, so the engine requires recovery. A cache allocation failure is different because the cache is derived data; the engine can clear that optional structure while preserving the authoritative index. The cache implementation also rolls back a partially inserted list entry if its hash lookup insertion fails. I designed those responses according to which component owns the truth, not according to one universal exception handler.

Code and tests: [`src/storage_engine.cpp`](../src/storage_engine.cpp) · [`src/lru_cache.cpp`](../src/lru_cache.cpp) · [`tests/test_recovery.cpp`](../tests/test_recovery.cpp)

## TCP protocol and operational contracts

**29. How are TCP message boundaries and request sizes handled?**

One TCP read may contain part of a command or several commands together. The server accumulates pending bytes and extracts complete lines, retaining any trailing fragment for the next read. It also enforces a 64 KiB request-line limit, including when the sender never transmits a newline. Parser checks then handle TTL and scan arguments before storage sees a request. These are simple constraints, but they prevent accidental dependence on how the operating system happens to divide the byte stream.

Code and tests: [`src/server.cpp`](../src/server.cpp) · [`src/protocol.cpp`](../src/protocol.cpp) · [`tests/test_protocol.cpp`](../tests/test_protocol.cpp)

**30. Why does SCAN escape output values while GET reports a byte count?**

SCAN returns multiple tab-separated items, so embedded tabs, newlines, and control characters need escaping to preserve the record boundaries. GET uses a single-value response with a declared byte count. I documented these response shapes separately because one generic 'print the value' rule would be ambiguous for clients. Framing belongs to the protocol even when the underlying engine stores ordinary strings; otherwise stored data can accidentally look like a command or a new result row.

Code and tests: [`src/server.cpp`](../src/server.cpp) · [`docs/protocol.md`](../docs/protocol.md) · [`tests/test_protocol.cpp`](../tests/test_protocol.cpp)

**31. What controls the server's operational scope and connection lifecycle?**

The default listener is on loopback, with bounded request sizes, socket and idle timeouts, scoped descriptor cleanup, and a bounded worker queue. These choices establish a predictable operating environment for running and testing the server. They also make the connection lifecycle visible: what happens when a client stalls, a worker is occupied, or the service shuts down. Any broader network deployment needs access controls appropriate to that environment; changing the bind address alone is not an authentication design.

Code and tests: [`include/voltstore/server.h`](../include/voltstore/server.h) · [`src/server.cpp`](../src/server.cpp) · [`docs/protocol.md`](../docs/protocol.md)

## Fault injection and verification

**32. Why are fault-injection tests especially informative for this engine?**

A normal test observes the start and end of an operation. Storage bugs often live in the middle. The post-WAL hook lets tests interrupt execution after a record is appended but before the index is updated. Other fixtures truncate a log tail or damage snapshot bytes. For each case I want two observations: what the current process does immediately, and what a newly opened engine reconstructs. Deliberate fault injection makes those boundaries repeatable enough to investigate rather than waiting for a rare failure to occur naturally.

Code and tests: [`include/voltstore/storage_engine.h`](../include/voltstore/storage_engine.h) · [`tests/test_recovery.cpp`](../tests/test_recovery.cpp) · [`tests/test_concurrency.cpp`](../tests/test_concurrency.cpp)

**33. How do process-level crash tests complement component tests?**

A unit test of replay cannot replace the experience of losing an actual process. The integration suite starts a server, performs writes, terminates it with SIGKILL, and starts a new process on the same data directory. This exercises the boundary where in-memory state disappears and only persisted bytes remain. It complements the byte-level and component tests, which are better at isolating a specific record or error. The distinction between process loss and physical power loss still matters when interpreting the results.

Code and tests: [`tests/crash_recovery.py`](../tests/crash_recovery.py) · [`tests/integration_server.py`](../tests/integration_server.py) · [`docs/recovery-model.md`](../docs/recovery-model.md)

**34. What does each verification method contribute?**

I do not treat one green test result as a universal quality statement. Debug and Release test the compiled code under different build settings. ASan and UBSan expose selected memory and undefined-behavior faults; TSan watches executed concurrency paths for races; Clang Static Analyzer inspects source without running it. Tests also assert specified behaviors such as replay, snapshot order, and protocol handling. The verification report records which commands were actually run. That mix gives independent views of the implementation while keeping each result tied to its scope.

Code and tests: [`docs/verification-results.md`](../docs/verification-results.md) · [`CMakeLists.txt`](../CMakeLists.txt)

**35. How should throughput be evaluated without conflating different guarantees?**

Throughput has little meaning without the workload and write-completion policy. The engine benchmark lets the tester vary operation count, thread count, and synchronization; the TCP benchmark includes a different layer of parsing and socket overhead. A useful result must state key and value sizes, build mode, machine, storage medium, operation mix, and whether writes call `fsync`. I would not compare a run that skips synchronization with one that requests it as though they offered the same durability contract. Performance work starts with a comparison that is actually fair.

Code and tests: [`benchmarks/engine_benchmark.cpp`](../benchmarks/engine_benchmark.cpp) · [`benchmarks/tcp_benchmark.py`](../benchmarks/tcp_benchmark.py)

## Next engineering questions

**36. What technical questions does this architecture invite next?**

The next questions are measurable ones. How would replay change if the engine supported increments or append-only updates? What happens to tail latency when `fsync`, the mutation lock, and a fixed worker pool compete under a mixed workload? What extra ordering and ownership protocol would be necessary across multiple processes or machines? I would approach each extension by specifying the new invariant and constructing the failure schedule or benchmark that could distinguish designs. Adding features before deciding what they promise would make the system harder to reason about.

Code and tests: [`docs/recovery-model.md`](../docs/recovery-model.md) · [`docs/concurrency-model.md`](../docs/concurrency-model.md) · [`docs/benchmark-methodology.md`](../docs/benchmark-methodology.md)
