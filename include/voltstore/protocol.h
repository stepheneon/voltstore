#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace voltstore {
enum class CommandType {
    Put,
    Get,
    Delete,
    Exists,
    Scan,
    Stats,
    Snapshot,
    Ping,
    Quit,
    Invalid,
};

// Parsed command fields. Invalid commands carry a client-safe error message.
struct Command {
    CommandType type{CommandType::Invalid};
    std::string key;
    std::string value;
    std::optional<long long> ttl_ms;
    std::size_t limit{100};
    std::string error;
};

// Parse one newline-free command. Malformed syntax produces CommandType::Invalid
// and a controlled error message; valid commands are returned for server dispatch.
Command parse_command(const std::string& line);
}  // namespace voltstore
