ADR 001 — C++20 for the Storage Engine
======================================

Decision: Implement the storage engine in C++20 with POSIX file/socket interfaces.

Rationale: C++20 supports RAII ownership, standard containers, mutexes, atomics, and conventional modular compilation while retaining explicit interaction with Linux system calls. Compared with C, this reduces the number of manually managed resource lifetimes. Compared with a fully managed runtime, it makes lock, I/O and lifetime behavior directly observable.

Engineering considerations: RAII makes resource lifetimes explicit, while compiler diagnostics, sanitizer runs, and tests form the verification workflow. POSIX interfaces align the implementation with Linux and Unix-like systems.
