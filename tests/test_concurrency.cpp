#include "test_harness.h"
#include "voltstore/storage_engine.h"
#include "voltstore/thread_pool.h"

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

TEST_CASE(concurrent_writes_and_reads) {
    test::Workspace workspace("concurrency");
    voltstore::StorageEngine engine({workspace.path(), 64, false});
    std::vector<std::thread> writers;
    std::atomic<bool> all_reads_succeeded{true};

    for (int worker = 0; worker < 8; ++worker) {
        writers.emplace_back([&, worker] {
            for (int index = 0; index < 200; ++index) {
                const auto key = "k" + std::to_string(worker) + ":" +
                                 std::to_string(index);
                engine.put(key, std::to_string(index));
                if (!engine.get(key).has_value()) all_reads_succeeded = false;
            }
        });
    }
    for (auto& writer : writers) writer.join();
    REQUIRE(all_reads_succeeded.load());
    REQUIRE(engine.stats().live_keys == 1600);
}

TEST_CASE(concurrent_update_same_key_is_safe) {
    test::Workspace workspace("same-key");
    voltstore::StorageEngine engine({workspace.path(), 8, false});
    std::vector<std::thread> writers;

    for (int worker = 0; worker < 6; ++worker) {
        writers.emplace_back([&, worker] {
            for (int index = 0; index < 150; ++index) {
                engine.put("shared", std::to_string(worker * 1000 + index));
            }
        });
    }
    for (auto& writer : writers) writer.join();
    REQUIRE(engine.get("shared"));
    REQUIRE(engine.stats().live_keys == 1);
}

TEST_CASE(cache_reflects_most_recent_completed_write) {
    test::Workspace workspace("cache-concurrent");
    voltstore::StorageEngine engine({workspace.path(), 8, false});
    engine.put("shared", "initial");

    std::thread reader([&] {
        for (int index = 0; index < 2000; ++index) {
            (void)engine.get("shared");
        }
    });
    for (int index = 0; index < 300; ++index) {
        engine.put("shared", std::to_string(index));
    }
    reader.join();

    // A completed PUT must not be overwritten by an older cached read.
    for (int index = 0; index < 100; ++index) {
        REQUIRE(engine.get("shared") == "299");
    }
}

TEST_CASE(thread_pool_rejects_full_queue_and_drains_pending_jobs) {
    // Workers use release; keep it alive until the pool has joined them.
    std::promise<void> started;
    std::promise<void> release;
    const auto release_signal = release.get_future().share();
    auto started_signal = started.get_future();
    std::atomic<int> completed{0};

    bool queue_full = false;
    {
        voltstore::ThreadPool pool(1);
        pool.submit([&] {
            started.set_value();
            release_signal.wait();
        });
        started_signal.wait();

        // Block one worker to fill the 16 pending queue slots.
        for (int index = 0; index < 16; ++index) {
            pool.submit([&] { ++completed; });
        }
        try {
            pool.submit([] {});
        } catch (const std::runtime_error&) {
            queue_full = true;
        }
        release.set_value();
    }

    REQUIRE(queue_full);
    REQUIRE(completed.load() == 16);
}

TEST_CASE(thread_pool_rejects_an_empty_job) {
    voltstore::ThreadPool pool(1);
    bool rejected = false;
    try {
        pool.submit({});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

TEST_CASE(post_wal_failure_blocks_concurrent_readers_before_releasing_index_lock) {
    test::Workspace workspace("post-wal-concurrent");
    std::promise<void> hook_entered;
    std::promise<void> release_hook;
    auto release_signal = release_hook.get_future().share();
    bool fail_once = false;

    voltstore::EngineConfig config{workspace.path(), 8, true};
    config.after_wal_append_hook = [&] {
        if (fail_once) {
            fail_once = false;
            hook_entered.set_value();
            release_signal.wait();
            throw std::bad_alloc();
        }
    };

    voltstore::StorageEngine engine(config);
    engine.put("shared", "before");
    fail_once = true;
    std::atomic<bool> writer_failed{false};
    std::thread writer([&] {
        try {
            engine.put("shared", "after");
        } catch (const std::bad_alloc&) {
            writer_failed = true;
        }
    });
    hook_entered.get_future().wait();

    std::atomic<bool> rejected{false};
    std::thread reader([&] {
        try {
            (void)engine.get("shared");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
    });
    release_hook.set_value();
    writer.join();
    reader.join();
    REQUIRE(writer_failed.load());
    REQUIRE(rejected.load());
}
