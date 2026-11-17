#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace voltstore {

// Fixed workers with a bounded queue (16 pending jobs per worker).
// Destruction drains queued work and joins workers; the owner completes submissions first.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    // Enqueue one callable. Throws for an empty callable, a full queue,
    // or shutdown. Worker task exceptions are logged rather than propagated.
    void submit(std::function<void()> job);

    // Return the fixed number of workers created during construction.
    std::size_t worker_count() const { return workers_.size(); }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    bool stopping_{false};
    std::size_t max_pending_{0};
    std::vector<std::thread> workers_;
};
}  // namespace voltstore
