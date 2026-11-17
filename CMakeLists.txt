cmake_minimum_required(VERSION 3.20)
project(VoltStore VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VOLTSTORE_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers" OFF)
option(VOLTSTORE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

add_library(voltstore
    src/crc32.cpp
    src/data_directory_lock.cpp
    src/wal.cpp
    src/snapshot.cpp
    src/lru_cache.cpp
    src/storage_engine.cpp
    src/thread_pool.cpp
    src/protocol.cpp
    src/server.cpp
)
target_include_directories(voltstore PUBLIC include)
target_compile_options(voltstore PRIVATE -Wall -Wextra -Wpedantic)
if(VOLTSTORE_WARNINGS_AS_ERRORS)
    target_compile_options(voltstore PRIVATE -Werror)
endif()
if(VOLTSTORE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(voltstore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(voltstore PUBLIC -fsanitize=address,undefined)
endif()
find_package(Threads REQUIRED)
target_link_libraries(voltstore PUBLIC Threads::Threads)

add_executable(voltstore_server app/voltstore_server.cpp)
target_link_libraries(voltstore_server PRIVATE voltstore)

add_executable(voltstore_bench benchmarks/engine_benchmark.cpp)
target_link_libraries(voltstore_bench PRIVATE voltstore)

enable_testing()
add_executable(voltstore_tests
    tests/test_main.cpp
    tests/test_engine.cpp
    tests/test_concurrency.cpp
    tests/test_recovery.cpp
    tests/test_protocol.cpp
)
target_link_libraries(voltstore_tests PRIVATE voltstore)
add_test(NAME voltstore_unit_and_component_tests COMMAND voltstore_tests)
set_tests_properties(voltstore_unit_and_component_tests PROPERTIES TIMEOUT 120)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME voltstore_server_integration
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_server.py $<TARGET_FILE:voltstore_server>)
    add_test(NAME voltstore_crash_recovery
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_recovery.py $<TARGET_FILE:voltstore_server>)
    set_tests_properties(voltstore_server_integration voltstore_crash_recovery
        PROPERTIES TIMEOUT 45)
endif()

install(TARGETS voltstore_server voltstore_bench RUNTIME DESTINATION bin)
