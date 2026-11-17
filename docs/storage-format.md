Storage Format
==============

WAL record (little-endian)
--------------------------

magic `VLT1` — 4 bytes

version — 2 bytes

op (`1=PUT`, `2=DELETE`) — 1 bytes

reserved — 1 bytes

key length — 4 bytes

value length — 4 bytes

absolute expiry in Unix ms (`0=none`) — 8 bytes

key bytes — variable bytes

value bytes — variable bytes

CRC32 of every preceding byte in this record — 4 bytes


Limits are checked before allocation (16 MiB value/field ceiling in the file decoder; server-facing key limit is smaller).

Snapshot
--------
Header is ASCII `VLTSNP1` plus an unsigned 64-bit entry count. Each entry contains key length, value length, absolute expiry, key bytes, value bytes, and a CRC32. A snapshot is written to `snapshot.vlt.tmp`, synced, renamed to `snapshot.vlt`, then its directory is synced.

Why absolute TTL
----------------
An expiry timestamp survives restart. Storing only “5 seconds remaining” would require reconstructing elapsed downtime and would change semantics after a crash.
