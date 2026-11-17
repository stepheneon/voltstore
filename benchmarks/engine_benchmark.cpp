#include "voltstore/storage_engine.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <exception>
#include <mutex>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class BenchmarkWorkspace {
public:
    BenchmarkWorkspace() {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "voltstore-benchmark-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) == nullptr) {
            throw std::runtime_error("cannot create a benchmark directory");
        }
        path_ = buffer.data();
    }

    ~BenchmarkWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::size_t parse_count(const std::string& text, const char* option) {
    std::size_t count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc{} || end != text.data() + text.size() || count == 0) {
        throw std::invalid_argument(std::string(option) + " requires a positive integer");
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t operations = 50'000;
    std::size_t threads = 4;
    bool sync_writes = false;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--fsync") {
                sync_writes = true;
            } else if (option == "--help") {
                std::cout << "voltstore_bench [--ops N] [--threads 1..128] [--fsync]\n";
                return 0;
            } else if ((option == "--ops" || option == "--threads") && index + 1 < argc) {
                const auto count = parse_count(argv[++index], option.c_str());
                if (option == "--ops") {
                    operations = count;
                } else {
                    threads = count;
                }
            } else {
                throw std::invalid_argument("unknown or incomplete option: " + option);
            }
        }
        if (threads > 128) {
            throw std::invalid_argument("thread count must be <= 128");
        }

        BenchmarkWorkspace directory;
        voltstore::StorageEngine engine({directory.path(), 1024, sync_writes});
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> workers;

        const auto run_workers = [&](auto operation) {
            workers.clear();
            std::mutex failure_mutex;
            std::exception_ptr failure;
            try {
                for (std::size_t worker = 0; worker < threads; ++worker) {
                    workers.emplace_back([&] {
                        try {
                            while (true) {
                                const auto index = next.fetch_add(1);
                                if (index >= operations) {
                                    return;
                                }
                                operation(index);
                            }
                        } catch (...) {
                            std::lock_guard lock(failure_mutex);
                            if (!failure) {
                                failure = std::current_exception();
                            }
                        }
                    });
                }
            } catch (...) {
                for (auto& worker : workers) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }
                throw;
            }
            for (auto& worker : workers) {
                worker.join();
            }
            if (failure) {
                std::rethrow_exception(failure);
            }
        };

        // Time PUT and GET separately against the same engine.
        const auto start = std::chrono::steady_clock::now();
        run_workers([&](std::size_t index) {
            engine.put("k" + std::to_string(index), "value-" + std::to_string(index));
        });
        const auto written_at = std::chrono::steady_clock::now();

        next = 0;
        std::atomic<std::size_t> found{0};
        run_workers([&](std::size_t index) {
            if (engine.get("k" + std::to_string(index))) ++found;
        });
        const auto finished_at = std::chrono::steady_clock::now();

        const auto write_time = std::chrono::duration<double>(written_at - start).count();
        const auto read_time = std::chrono::duration<double>(finished_at - written_at).count();
        const auto rate = [](std::size_t count, double seconds) {
            return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
        };

        std::cout << "VoltStore engine microbenchmark\n"
                  << "operations=" << operations << " threads=" << threads
                  << " fsync=" << (sync_writes ? "on" : "off") << '\n'
                  << std::fixed << std::setprecision(2)
                  << "write_seconds=" << write_time
                  << " write_ops_per_sec=" << rate(operations, write_time) << '\n'
                  << "read_seconds=" << read_time
                  << " read_ops_per_sec=" << rate(operations, read_time)
                  << " found=" << found.load() << '\n';
        return found == operations ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 2;
    }
}
