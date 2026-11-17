#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>

namespace voltstore {
inline void log_line(std::string_view level, std::string_view message) {
    static std::mutex mu;
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::lock_guard lock(mu);
    std::cerr << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ")
              << " [" << level << "] " << message << '\n';
}
inline void log_info(std::string_view message) {
    log_line("INFO", message);
}

inline void log_error(std::string_view message) {
    log_line("ERROR", message);
}
}  // namespace voltstore
