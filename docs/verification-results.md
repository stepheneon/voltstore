Verification Results
====================

Environment and date
--------------------

The checks below were rerun against this release source tree on 2026-11-17 in a Linux x86-64 container.

- GCC / G++ 14.2.0
- CMake 3.31.6
- Ninja 1.12.1
- Python 3.13.5
- Clang Static Analyzer (installed Clang toolchain)

Executed checks
---------------

Debug build and CTest

- Result: 3/3 passed
- Scope: Unit/component tests, TCP server integration, and crash-recovery process test

Release build and CTest

- Result: 3/3 passed
- Scope: Same test targets in Release configuration

AddressSanitizer + UndefinedBehaviorSanitizer

- Result: 3/3 passed
- Scope: Debug sanitizer build; zero sanitizer reports during the test run

ThreadSanitizer

- Result: 3/3 passed
- Scope: Separate Debug build with `-fsanitize=thread`; zero data-race reports during this run

Clang Static Analyzer

- Result: 9/9 translation units analyzed
- Scope: Every `src/*.cpp` implementation file; zero diagnostics

Warnings as errors

- Result: PASS
- Scope: Debug, Release, ASan/UBSan, and TSan CMake builds used `VOLTSTORE_WARNINGS_AS_ERRORS=ON`

GitHub Actions

- Result: Configured
- Scope: Push and pull-request workflow with Debug, Release, and sanitizer jobs


Reproduce
---------

```bash
cmake -S . -B build/Debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVOLTSTORE_WARNINGS_AS_ERRORS=ON
cmake --build build/Debug --parallel 2
ctest --test-dir build/Debug --output-on-failure --timeout 120

cmake -S . -B build/Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVOLTSTORE_WARNINGS_AS_ERRORS=ON
cmake --build build/Release --parallel 2
ctest --test-dir build/Release --output-on-failure --timeout 120

cmake -S . -B build/Asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVOLTSTORE_ENABLE_SANITIZERS=ON \
  -DVOLTSTORE_WARNINGS_AS_ERRORS=ON
cmake --build build/Asan --parallel 2
ASAN_OPTIONS=detect_leaks=1 \
  ctest --test-dir build/Asan --output-on-failure --timeout 120

cmake -S . -B build/Tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVOLTSTORE_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/Tsan --parallel 2
ctest --test-dir build/Tsan --output-on-failure --timeout 120

for source in src/*.cpp; do
  clang++ --analyze -std=c++20 -pthread -I include "$source"
done
```

Verification scope
------------------

These results cover the listed tests and static-analysis invocations in the stated Linux x86-64 environment. Persistence guarantees are defined by the filesystem and hardware assumptions in the [recovery model](recovery-model.md). The TCP interface follows the trusted-local operating model described in the [protocol specification](protocol.md), and I/O recovery behavior is documented in the same recovery model.
