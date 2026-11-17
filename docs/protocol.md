TCP Protocol
============

VoltStore accepts one newline-delimited ASCII-style command per request (values are opaque text up to the line-length limit). The server limits the entire command line to 64 KiB and defaults to IPv4 loopback binding.

`PING`

Reply: `PONG`

`PUT <key> <ttl_ms|0> <value>`

Reply: `OK` or `ERR`.

`GET <key>`

Reply: `VALUE <byte-count> <value>` or `NOT_FOUND`.

`DELETE <key>`

Reply: `DELETED` or `NOT_FOUND`.

`EXISTS <key>`

Reply: `1` or `0`.

`SCAN <prefix> [1..10000]`

Reply: `ITEMS n`, followed by `key<TAB>escaped-value` lines and `END`.

`STATS`

Reply: one `STATS` line containing counters.

`SNAPSHOT`

Reply: `SNAPSHOT_OK` or `ERR`.

`QUIT`

Reply: `BYE`, then the server closes the connection.

Keys are nonempty, limited to 1024 bytes through the engine API, and use whitespace-free identifiers in the TCP protocol. Values support spaces within the one-line request framing. TTL values are non-negative milliseconds; 0 indicates no expiry. SCAN escapes `\` as `\\`, tab as `\t`, CR as `\r`, LF as `\n`, and other ASCII control bytes as `\xHH` (uppercase hexadecimal). Decode escapes after splitting each item on its first tab. Plain text values without escapes keep their original representation. GET returns the raw value with its byte length. The protocol is designed for trusted local operation and uses newline-delimited text framing. The server binds to IPv4 loopback by default; deployments that extend access beyond the host should integrate the appropriate network-security controls.
