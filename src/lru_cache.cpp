#include "voltstore/lru_cache.h"

namespace voltstore {

LruCache::LruCache(std::size_t capacity) : capacity_(capacity) {}

std::optional<Entry> LruCache::get(const std::string& key, ClockMs now) {
    std::lock_guard lock(mu_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return std::nullopt;
    }
    if (is_expired(it->second->second, now)) {
        list_.erase(it->second);
        index_.erase(it);
        return std::nullopt;
    }
    // Move the existing list node: refresh recency without reallocating it.
    list_.splice(list_.begin(), list_, it->second);
    return it->second->second;
}

void LruCache::put(const std::string& key, const Entry& value) {
    if (capacity_ == 0) {
        return;
    }
    std::lock_guard lock(mu_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        it->second->second = value;
        list_.splice(list_.begin(), list_, it->second);
        return;
    }

    // Publish a list entry first, then index it. Roll back the list entry
    // if the index insertion fails so the two structures stay consistent.
    list_.emplace_front(key, value);
    try {
        index_.emplace(list_.front().first, list_.begin());
    } catch (...) {
        list_.pop_front();
        throw;
    }
    while (list_.size() > capacity_) {
        auto last = std::prev(list_.end());
        index_.erase(last->first);
        list_.erase(last);
    }
}

void LruCache::erase(const std::string& key) {
    std::lock_guard lock(mu_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        list_.erase(it->second);
        index_.erase(it);
    }
}

void LruCache::clear() {
    std::lock_guard lock(mu_);
    list_.clear();
    index_.clear();
}

std::size_t LruCache::size() const {
    std::lock_guard lock(mu_);
    return list_.size();
}

}  // namespace voltstore
