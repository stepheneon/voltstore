Recovery and Reliability Model
==============================

Exercised recovery paths
------------------------

The integration and recovery tests exercise the following behaviors with real server processes:

- Restart after WAL-backed PUT and DELETE operations.
- SIGKILL after an acknowledged synchronized write, followed by recovery in a fresh process.
- Snapshot creation followed by further WAL appends and restart.
- Incomplete final WAL record: truncate and synchronize the incomplete suffix before accepting new writes.
- Complete WAL record with checksum mismatch: report an integrity error during startup.
- Truncated snapshots and snapshots with trailing data: report validation errors.
- Unreadable WAL: report a startup error while preserving durable files for diagnosis.
- Snapshot temporary-file creation failure: propagate the error and retain the previously committed snapshot and WAL.
- Invalid TTL and key values: validate before mutation.
- Concurrent open of one data directory: coordinate exclusive ownership through the directory lock.
- Error after WAL append but before index publication: transition the current instance to recovery-required state; replay the persisted record on restart.
- SCAN response formatting: escape tabs, backslashes, and control characters in values.

Operating profile
-----------------

The validated recovery profile covers process restart and SIGKILL recovery on a local Linux filesystem, with one active engine per data directory. The TCP command interface follows a trusted-local model and defaults to IPv4 loopback binding.

I/O error handling
------------------

The engine propagates I/O exceptions and uses startup validation to establish the state of the snapshot/WAL pair. An append can reach storage partially or completely when an I/O call reports an error. Callers close the affected engine, reopen the data directory, and inspect the recovery report before resuming operations.
