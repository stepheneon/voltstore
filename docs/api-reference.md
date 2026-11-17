Storage engine API
==================

VoltStore exposes a C++20 `StorageEngine` class and a local TCP command
interface. Public C++ declarations are under `include/voltstore/`; the
`voltstore` CMake library target implements them. The service uses the same
engine internally.

C++20 usage
-----------

Build the library from the repository root:

```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
```

A small program can instantiate the engine, write a value and read it back:

```cpp
#include "voltstore/storage_engine.h"

#include <filesystem>
#include <string>

int main() {
    voltstore::EngineConfig config;
    config.data_dir = std::filesystem::path{"./example-store"};

    voltstore::StorageEngine engine(config);
    engine.put("sensor:1", "22.7C");
    const auto value = engine.get("sensor:1");
    return value && *value == "22.7C" ? 0 : 1;
}
```

To compile this code saved as `example.cpp` after building VoltStore:

```bash
c++ -std=c++20 -I include example.cpp build/Release/libvoltstore.a -pthread -o example
./example
```

The example writes its data under `./example-store` relative to the current
directory.

`EngineConfig::data_dir` identifies the storage directory. The constructor
acquires an exclusive directory lock and reconstructs persisted state before
serving operations. `put` and `erase` use the WAL before publishing the
corresponding in-memory change. `get` returns an `std::optional<std::string>`.

Other engine methods include `exists`, `scan_prefix`, `cleanup_expired`,
`snapshot`, `stats` and `replay_report`. Refer to
[`storage_engine.h`](../include/voltstore/storage_engine.h) for their exact
arguments, return values and preconditions, and to
[Architecture](architecture.md) for state ownership and concurrency.

TCP command interface
---------------------

Start the server with its local binding and an explicit data directory:

```bash
./build/Release/voltstore_server --data-dir ./voltstore-data --bind 127.0.0.1 --port 7379
```

In another terminal, use the included client:

```bash
python3 tools/volt_cli.py --host 127.0.0.1 --port 7379
```

Sample commands:

```text
PING
PUT sensor:1 0 22.7C
GET sensor:1
SCAN sensor: 20
QUIT
```

The protocol uses newline-delimited requests. See
[Protocol](protocol.md) for syntax, limits and response encoding.
The server and engine are intended for trusted local use under the
operational model described in the repository README.

Testing the interface
---------------------

```bash
ctest --test-dir build/Release --output-on-failure
```

Tests cover direct engine calls, TCP request processing, concurrency,
WAL replay and process-restart recovery. See [Verification results](verification-results.md)
for the recorded environment and execution scope.
