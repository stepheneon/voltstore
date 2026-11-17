Runtime Configuration
=====================

VoltStore runtime settings are supplied through command-line flags: `--data-dir`, `--bind`, `--port`, `--threads`, `--cache`, and `--no-fsync`.

`--bind` defaults to `127.0.0.1`, supporting the trusted-local operating model. External deployment requires a separately configured network-security boundary. `--no-fsync` selects asynchronous WAL persistence; the default mode synchronizes each acknowledged mutation. See [server usage](../README.md#run-locally).
