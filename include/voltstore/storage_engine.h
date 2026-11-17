#pragma once

#include "voltstore/common.h"
#include "voltstore/data_directory_lock.h"
#include "voltstore/lru_cache.h"
#include "voltstore/wal.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace voltstore {

// When enabled, sync_writes calls fsync before acknowledging WAL writes.
// Disabling it reduces sync overhead but weakens crash durability.
struct EngineConfig {
    std::filesystem::path data_dir{"./voltstore-data"};
    std::size_t cache_capacity{256};
    bool sync_writes{true};
    // Optional hook for deterministic post-WAL recovery-path tests.
    std::function<void()> after_wal_append_hook{};
};
struct ScanItem {
    std::string key;
    std::string value;
};
struct EngineStats {
    std::size_t live_keys{0};
    std::size_t cache_items{0};
    std::uintmax_t wal_bytes{0};
    std::uint64_t puts{0};
    std::uint64_t gets{0};
    std::uint64_t deletes{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    std::uint64_t snapshots{0};
};

// WAL-backed key-value engine with an in-memory index and optional read cache.
// Mutations and snapshots are serialized; a file lock enforces exclusive ownership.
class StorageEngine {
public:
    // Lock the data directory before opening the WAL and restoring the index.
    // Startup validates exclusive ownership, persisted records, and filesystem access.
    explicit StorageEngine(EngineConfig config);

    // Append a PUT record before updating the index. Invalid inputs are rejected.
    // A post-WAL publication error enters recovery-required state; reopen to replay the log.
    // Index updates are O(1) on average, excluding I/O.
    void put(std::string key, std::string value,
             std::optional<std::int64_t> ttl_ms = std::nullopt);

    // Return a copy of a live value or nullopt for missing/expired keys.
    // Average O(1) lookup; internally synchronized for concurrent callers.
    std::optional<std::string> get(const std::string& key);

    // Append a delete record before removing an existing live key. Returns false
    // for a missing/expired key. A post-WAL publication error is recovered by restart and replay.
    bool erase(const std::string& key);

    // Check whether a live value is present. Uses get(), so it also updates
    // read/cache counters; average O(1) index work, excluding I/O.
    bool exists(const std::string& key);

    // Return up to limit matches in key order. Scans the index using a bounded
    // heap: O(n log(limit + 1)) comparisons, plus value copies.
    std::vector<ScanItem> scan_prefix(const std::string& prefix,
                                      std::size_t limit = 100);

    // Remove expired in-memory entries. O(n) over indexed entries; no new WAL
    // records are needed because expiry timestamps survive restart.
    std::size_t cleanup_expired();

    // Atomically replace the checkpoint, then truncate the WAL. Serialized
    // with mutations; startup recovery validates the checkpoint/WAL pair after I/O errors.
    void snapshot();

    // Read counters and current live-key count; filesystem errors propagate
    // if WAL size cannot be read. Counters are process-local and reset on restart.
    EngineStats stats() const;

    // Return diagnostics produced during construction and initial replay.
    ReplayReport replay_report() const { return replay_report_; }

    // Return the immutable configuration. The reference is engine-owned.
    const EngineConfig& config() const { return config_; }

private:
    // Load a checkpoint, replay durable mutations, then discard expired data.
    void recover();
    void apply_replayed(const WalRecord& record);
    void require_healthy() const;
    bool erase_expired_if_needed(const std::string& key, ClockMs now);

    EngineConfig config_;
    std::filesystem::path snapshot_path_;
    DataDirectoryLock directory_lock_;
    Wal wal_;
    mutable std::shared_mutex data_mu_;
    mutable std::mutex mutation_mu_;
    std::unordered_map<std::string, Entry> data_;
    LruCache cache_;
    ReplayReport replay_report_{};
    // Set when a post-WAL publication error requires restart and WAL replay.
    std::atomic<bool> recovery_required_{false};
    std::atomic<std::uint64_t> puts_{0};
    std::atomic<std::uint64_t> gets_{0};
    std::atomic<std::uint64_t> deletes_{0};
    std::atomic<std::uint64_t> cache_hits_{0};
    std::atomic<std::uint64_t> cache_misses_{0};
    std::atomic<std::uint64_t> snapshots_{0};
};
}  // namespace voltstore
