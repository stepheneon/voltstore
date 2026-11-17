#pragma once

#include "voltstore/common.h"
#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace voltstore {

// Entry-count-bounded LRU cache. A mutex protects the list and lookup table.
// The front is most recently used; operations are O(1) on average.
class LruCache {
public:
    explicit LruCache(std::size_t capacity);
    // Return a copy of a live entry and refresh recency; missing/expired
    // entries return nullopt. StorageEngine coordinates persistence and expiry.
    std::optional<Entry> get(const std::string& key, ClockMs now);

    // Insert or update an entry and evict the least recently used when full.
    // May throw on allocation failure; callers own their source entry.
    void put(const std::string& key, const Entry& value);

    // Remove one cached key, or remove everything; neither changes storage.
    void erase(const std::string& key);
    void clear();
    // Read current occupancy and configured entry limit. Both are O(1).
    std::size_t size() const;
    std::size_t capacity() const { return capacity_; }

private:
    using Node = std::pair<std::string, Entry>;
    std::size_t capacity_;
    mutable std::mutex mu_;
    std::list<Node> list_;
    std::unordered_map<std::string, std::list<Node>::iterator> index_;
};
}  // namespace voltstore
