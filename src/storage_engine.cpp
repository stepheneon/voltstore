#include "voltstore/storage_engine.h"
#include "voltstore/snapshot.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <queue>

namespace voltstore {

ClockMs now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

StorageEngine::StorageEngine(EngineConfig config)
    : config_(std::move(config)),
      snapshot_path_(config_.data_dir / "snapshot.vlt"),
      directory_lock_(config_.data_dir),
      wal_(config_.data_dir / "wal.vlt", config_.sync_writes),
      cache_(config_.cache_capacity) {
    std::filesystem::create_directories(config_.data_dir);
    recover();
}

void StorageEngine::require_healthy() const {
    if (recovery_required_.load()) {
        throw std::runtime_error("storage state requires restart and WAL recovery");
    }
}

void StorageEngine::recover() {
    data_ = Snapshot::load(snapshot_path_);
    const auto now = now_ms();
    for (auto it = data_.begin(); it != data_.end();) {
        if (is_expired(it->second, now)) {
            it = data_.erase(it);
        } else {
            ++it;
        }
    }
    // Replay the WAL after loading the checkpoint.
    replay_report_ = wal_.replay([this](const WalRecord& record) {
        apply_replayed(record);
    });
    cleanup_expired();
}

void StorageEngine::apply_replayed(const WalRecord& record) {
    if (record.op == WalOp::Put) {
        data_[record.key] = Entry{record.value, record.expires_at_ms};
    } else {
        data_.erase(record.key);
    }
}

void StorageEngine::put(std::string key, std::string value,
                        std::optional<std::int64_t> ttl_ms) {
    if (key.empty()) {
        throw std::invalid_argument("key must not be empty");
    }
    if (key.size() > 1024) {
        throw std::invalid_argument("key too long");
    }
    if (value.size() > 16 * 1024 * 1024) {
        throw std::invalid_argument("value too large");
    }
    if (ttl_ms && *ttl_ms < 0) {
        throw std::invalid_argument("TTL must be >= 0");
    }

    const auto now = now_ms();
    if (ttl_ms && *ttl_ms > std::numeric_limits<ClockMs>::max() - now) {
        throw std::invalid_argument("TTL would overflow its absolute timestamp");
    }
    const ClockMs expires_at = (ttl_ms && *ttl_ms > 0) ? now + *ttl_ms : 0;
    Entry entry{value, expires_at};

    std::lock_guard mutation(mutation_mu_);
    require_healthy();
    wal_.append(WalRecord{WalOp::Put, key, value, expires_at});
    std::unique_lock lock(data_mu_, std::defer_lock);
    try {
        lock.lock();
        if (config_.after_wal_append_hook) {
            config_.after_wal_append_hook();
        }
        data_[key] = entry;
        try {
            cache_.put(key, entry);
        } catch (const std::bad_alloc&) {
            cache_.clear();
        }
    } catch (...) {
        // Publish the recovery-required state before other readers can acquire the index lock.
        recovery_required_.store(true);
        throw;
    }
    ++puts_;
}

bool StorageEngine::erase_expired_if_needed(const std::string& key, ClockMs now) {
    std::unique_lock lock(data_mu_);
    require_healthy();
    auto it = data_.find(key);
    if (it != data_.end() && is_expired(it->second, now)) {
        data_.erase(it);
        cache_.erase(key);
        return true;
    }
    return false;
}

std::optional<std::string> StorageEngine::get(const std::string& key) {
    require_healthy();
    ++gets_;
    const auto now = now_ms();
    {
        // Hold the index read lock through cache access to avoid stale entries.
        std::shared_lock lock(data_mu_);
        require_healthy();
        if (auto cached = cache_.get(key, now)) {
            ++cache_hits_;
            return cached->value;
        }
        ++cache_misses_;
        const auto it = data_.find(key);
        if (it == data_.end()) {
            return std::nullopt;
        }
        if (!is_expired(it->second, now)) {
            const auto value = it->second.value;
            try {
                cache_.put(key, it->second);
            } catch (const std::bad_alloc&) {
                cache_.clear();
            }
            return value;
        }
    }
    erase_expired_if_needed(key, now);
    return std::nullopt;
}

bool StorageEngine::erase(const std::string& key) {
    std::lock_guard mutation(mutation_mu_);
    require_healthy();
    {
        std::shared_lock lock(data_mu_);
        auto it = data_.find(key);
        if (it == data_.end() || is_expired(it->second, now_ms())) {
            lock.unlock();
            erase_expired_if_needed(key, now_ms());
            return false;
        }
    }

    wal_.append(WalRecord{WalOp::Delete, key, "", 0});
    std::unique_lock lock(data_mu_, std::defer_lock);
    try {
        lock.lock();
        if (config_.after_wal_append_hook) {
            config_.after_wal_append_hook();
        }
        data_.erase(key);
        cache_.erase(key);
    } catch (...) {
        recovery_required_.store(true);
        throw;
    }
    ++deletes_;
    return true;
}

bool StorageEngine::exists(const std::string& key) {
    return get(key).has_value();
}

std::vector<ScanItem> StorageEngine::scan_prefix(const std::string& prefix, std::size_t limit) {
    require_healthy();
    cleanup_expired();
    if (limit == 0) {
        return {};
    }

    // Retain the smallest matches in a bounded heap instead of sorting every key.
    const auto by_key = [](const ScanItem& left, const ScanItem& right) {
        return left.key < right.key;
    };
    std::priority_queue<ScanItem, std::vector<ScanItem>, decltype(by_key)> nearest(by_key);

    {
        std::shared_lock lock(data_mu_);
        require_healthy();
        for (const auto& [key, entry] : data_) {
            if (key.rfind(prefix, 0) != 0) {
                continue;
            }
            if (nearest.size() < limit) {
                nearest.push({key, entry.value});
            } else if (key < nearest.top().key) {
                nearest.pop();
                nearest.push({key, entry.value});
            }
        }
    }

    std::vector<ScanItem> results;
    results.reserve(nearest.size());
    while (!nearest.empty()) {
        results.push_back(nearest.top());
        nearest.pop();
    }
    std::sort(results.begin(), results.end(), by_key);
    return results;
}

std::size_t StorageEngine::cleanup_expired() {
    require_healthy();
    const auto now = now_ms();
    std::size_t removed = 0;
    std::unique_lock lock(data_mu_);
    require_healthy();
    for (auto it = data_.begin(); it != data_.end();) {
        if (is_expired(it->second, now)) {
            cache_.erase(it->first);
            it = data_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void StorageEngine::snapshot() {
    std::lock_guard mutation(mutation_mu_);
    require_healthy();
    cleanup_expired();
    std::unordered_map<std::string, Entry> copy;
    {
        std::shared_lock lock(data_mu_);
        copy = data_;
    }
    Snapshot::write_atomic(snapshot_path_, copy);
    wal_.truncate();
    ++snapshots_;
}

EngineStats StorageEngine::stats() const {
    require_healthy();
    EngineStats stats;
    {
        std::shared_lock lock(data_mu_);
        require_healthy();
        const auto now = now_ms();
        stats.live_keys = static_cast<std::size_t>(std::count_if(
            data_.begin(), data_.end(), [now](const auto& item) {
                return !is_expired(item.second, now);
            }));
    }
    stats.cache_items = cache_.size();
    stats.wal_bytes = wal_.size_bytes();
    stats.puts = puts_;
    stats.gets = gets_;
    stats.deletes = deletes_;
    stats.cache_hits = cache_hits_;
    stats.cache_misses = cache_misses_;
    stats.snapshots = snapshots_;
    return stats;
}

}  // namespace voltstore
