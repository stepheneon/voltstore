Benchmark Methodology
=====================

`voltstore_bench` uses deterministic keys and short values. It runs a write phase followed by a read phase against one local engine, with configurable operation and thread counts. Each run uses a separate temporary data directory.

```bash
./build/Release/voltstore_bench --ops 50000 --threads 4
./build/Release/voltstore_bench --ops 1000 --threads 4 --fsync
```

The default mode measures write throughput with per-write `fsync` disabled; use `--fsync` to measure synchronized-write behavior. The read phase uses the populated in-memory index and represents a warm-read workload.

Report the CPU, compiler, build type, storage medium, operation count, thread count, key/value sizes, synchronization mode, and repeated-run distribution. Compare results using equivalent persistence semantics and workloads, and report each measurement with its environment and workload configuration.
