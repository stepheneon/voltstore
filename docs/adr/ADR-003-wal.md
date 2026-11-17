ADR 003 — Write-Ahead Log
=========================

Decision: Persist each mutation as a CRC32-checked WAL record before updating the in-memory index.

Reasoning: Complete records can be replayed in order after a process restart. In synchronized mode, the WAL file is `fsync`ed before the memory update and response.

Engineering considerations: Synchronized writes add per-record I/O latency. Recovery repairs an incomplete trailing record and treats checksum failure in a complete record as an integrity error. I/O failures transition the engine to its documented recovery workflow.
