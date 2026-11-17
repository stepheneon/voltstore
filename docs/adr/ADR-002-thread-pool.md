ADR 002 — Fixed Connection Worker Pool
======================================

Decision: Use a fixed number of worker threads and a bounded pending connection queue.

Reasoning: This avoids unbounded thread creation and puts a predictable ceiling on queued connections. The server uses a 30-second idle timeout and bounded socket operations to support shutdown.

Engineering considerations: Each connected client occupies one worker, giving the server a bounded and predictable concurrency model. Event-driven I/O is an alternative architecture for workloads dominated by idle connections.
