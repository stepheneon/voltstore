ADR 004 — In-Memory Hash Index
==============================

Decision: Use `std::unordered_map` for the primary key-value index.

Reasoning: Average constant-time point lookup and straightforward synchronization support the engine's point-operation workload.

Engineering considerations: The in-memory index optimizes point operations. Prefix scans traverse the key set and use a bounded heap to retain the requested number of results before sorting.
