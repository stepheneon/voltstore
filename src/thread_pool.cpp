#include "voltstore/thread_pool.h"

#include "voltstore/logger.h"

#include <stdexcept>
#include <utility>

namespace voltstore {

ThreadPool::ThreadPool(std::size_t threads) {
    if (threads == 0 || threads > 128) {
        throw std::invalid_argument("thread count must be in 1..128");
    }

    max_pending_ = threads * 16;
    try {
        for (std::size_t index = 0; index < threads; ++index) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mu_);
                        cv_.wait(lock, [this] {
                            return stopping_ || !jobs_.empty();
                        });
                        if (stopping_ && jobs_.empty()) {
                            return;
                        }
                        job = std::move(jobs_.front());
                        jobs_.pop();
                    }

                    try {
                        job();
                    } catch (const std::exception& error) {
                        log_error(error.what());
                    } catch (...) {
                        log_error("worker job failed with an unknown exception");
                    }
                }
            });
        }
    } catch (...) {
        {
            std::lock_guard lock(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard lock(mu_);
        if (stopping_) {
            throw std::runtime_error("thread pool is stopping");
        }
        if (!job) {
            throw std::invalid_argument("thread pool job must not be empty");
        }
        if (jobs_.size() >= max_pending_) {
            throw std::runtime_error("thread pool queue is full");
        }
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

}  // namespace voltstore
