#include "voltstore/protocol.h"

#include <charconv>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace voltstore {
namespace {

std::vector<std::string> split_words(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> tokens;
    std::string token;
    while (input >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

bool parse_nonnegative(const std::string& text, long long& value) {
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value >= 0;
}

Command invalid(std::string message) {
    Command command;
    command.error = std::move(message);
    return command;
}

}  // namespace

Command parse_command(const std::string& line) {
    const auto tokens = split_words(line);
    if (tokens.empty()) {
        return invalid("empty command");
    }

    const auto& operation = tokens.front();
    if (operation == "GET" || operation == "DELETE" || operation == "EXISTS") {
        if (tokens.size() != 2) {
            return invalid(operation + " requires exactly one key");
        }

        Command command;
        command.key = tokens[1];
        if (operation == "GET") {
            command.type = CommandType::Get;
        } else if (operation == "DELETE") {
            command.type = CommandType::Delete;
        } else {
            command.type = CommandType::Exists;
        }
        return command;
    }

    if (operation == "PUT") {
        std::istringstream input(line);
        std::string verb;
        std::string key;
        std::string ttl_text;
        input >> verb >> key >> ttl_text;
        if (key.empty() || ttl_text.empty()) {
            return invalid("PUT syntax: PUT <key> <ttl_ms|0> <value>");
        }
        long long ttl = 0;
        if (!parse_nonnegative(ttl_text, ttl)) {
            return invalid("TTL must be a non-negative integer");
        }

        // PUT consumes the rest of the line as its value; unlike the command
        // fields, values may contain spaces.
        std::string value;
        std::getline(input, value);
        if (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }
        if (value.empty()) {
            return invalid("PUT requires a value");
        }

        Command command;
        command.type = CommandType::Put;
        command.key = std::move(key);
        command.value = std::move(value);
        command.ttl_ms = ttl;
        return command;
    }

    if (operation == "SCAN") {
        if (tokens.size() < 2 || tokens.size() > 3) {
            return invalid("SCAN syntax: SCAN <prefix> [limit]");
        }
        Command command;
        command.type = CommandType::Scan;
        command.key = tokens[1];
        if (tokens.size() == 3) {
            long long limit = 0;
            if (!parse_nonnegative(tokens[2], limit) || limit < 1 || limit > 10'000) {
                return invalid("SCAN limit must be 1..10000");
            }
            command.limit = static_cast<std::size_t>(limit);
        }
        return command;
    }

    if (tokens.size() == 1) {
        Command command;
        if (operation == "STATS") {
            command.type = CommandType::Stats;
        } else if (operation == "SNAPSHOT") {
            command.type = CommandType::Snapshot;
        } else if (operation == "PING") {
            command.type = CommandType::Ping;
        } else if (operation == "QUIT") {
            command.type = CommandType::Quit;
        } else {
            return invalid("unknown or malformed command");
        }
        return command;
    }

    return invalid("unknown or malformed command");
}

}  // namespace voltstore
