ADR 005 — Newline-Delimited TCP Protocol
========================================

Decision: Use one newline-delimited command per request with an explicit maximum line size.

Reasoning: The protocol is easy to inspect from a terminal and to exercise in end-to-end tests.

Engineering considerations: Newline-delimited framing keeps commands easy to inspect and test. The server defaults to IPv4 loopback and follows a trusted-local operating model; external deployment requires a separately configured network-security boundary.
