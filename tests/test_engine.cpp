#include "test_harness.h"

#include "voltstore/lru_cache.h"
#include "voltstore/storage_engine.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>

TEST_CASE(basic_put_get_delete) {
    test::Workspace workspace("basic");
    voltstore::StorageEngine engine({workspace.path(), 4, false});

    engine.put("a", "1");
    REQUIRE(engine.get("a").value() == "1");
    engine.put("a", "2");
    REQUIRE(engine.get("a").value() == "2");
    REQUIRE(engine.exists("a"));
    REQUIRE(engine.erase("a"));
    REQUIRE(!engine.get("a"));
    REQUIRE(!engine.erase("a"));
}

TEST_CASE(ttl_expiry) {
    test::Workspace workspace("ttl");
    voltstore::StorageEngine engine({workspace.path(), 4, false});
    engine.put("short", "value", 40);
    REQUIRE(engine.get("short"));

    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    REQUIRE(!engine.get("short"));
    REQUIRE(!engine.exists("short"));
}

TEST_CASE(prefix_scan_sorted_and_limited) {
    test::Workspace workspace("scan");
    voltstore::StorageEngine engine({workspace.path(), 4, false});
    engine.put("sensor:b", "2");
    engine.put("sensor:a", "1");
    engine.put("other", "x");

    const auto items = engine.scan_prefix("sensor:", 1);
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].key == "sensor:a");
}

TEST_CASE(cache_stats) {
    test::Workspace workspace("cache");
    voltstore::StorageEngine engine({workspace.path(), 1, false});
    engine.put("a", "1");
    REQUIRE(engine.get("a") == "1");
    REQUIRE(engine.stats().cache_hits >= 1);
}

TEST_CASE(lru_eviction_behavior) {
    voltstore::LruCache cache(2);
    const auto now = voltstore::now_ms();
    cache.put("a", {"1", 0});
    cache.put("b", {"2", 0});
    REQUIRE(cache.get("a", now));

    // Reading a refreshes recency, so b should be evicted next.
    cache.put("c", {"3", 0});
    REQUIRE(!cache.get("b", now));
    REQUIRE(cache.get("a", now)->value == "1");
    REQUIRE(cache.get("c", now)->value == "3");
}

TEST_CASE(ttl_overflow_is_rejected_without_mutation) {
    test::Workspace workspace("ttl-overflow");
    voltstore::StorageEngine engine({workspace.path(), 4, true});

    bool rejected = false;
    try {
        engine.put("invalid", "value", std::numeric_limits<std::int64_t>::max());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE(rejected);
    REQUIRE(!engine.get("invalid"));
}

TEST_CASE(invalid_input_is_rejected_before_wal_write) {
    test::Workspace workspace("invalid-input");
    voltstore::StorageEngine engine({workspace.path(), 4, true});
    const auto starting_bytes = engine.stats().wal_bytes;

    bool rejected = false;
    try {
        engine.put("", "value");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE(rejected);
    REQUIRE(engine.stats().wal_bytes == starting_bytes);
}

TEST_CASE(prefix_scan_retains_smallest_keys_under_limit) {
    test::Workspace workspace("scan-limit");
    voltstore::StorageEngine engine({workspace.path(), 4, false});
    engine.put("sensor:z", "z");
    engine.put("sensor:b", "b");
    engine.put("sensor:a", "a");
    engine.put("sensor:c", "c");
    engine.put("other", "x");

    const auto items = engine.scan_prefix("sensor:", 2);
    REQUIRE(items.size() == 2);
    REQUIRE(items[0].key == "sensor:a");
    REQUIRE(items[1].key == "sensor:b");
    REQUIRE(engine.scan_prefix("sensor:", 0).empty());
}

TEST_CASE(stats_exclude_expired_entries) {
    test::Workspace workspace("live-count");
    voltstore::StorageEngine engine({workspace.path(), 4, false});
    engine.put("soon-expired", "value", 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(engine.stats().live_keys == 0);
}
