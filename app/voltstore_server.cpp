#include "voltstore/logger.h"
#include "voltstore/server.h"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
volatile std::sig_atomic_t shutdown_signal = 0;

// Signal handling sets a flag; normal control flow performs shutdown.
void on_signal(int) {
    shutdown_signal = 1;
}

std::uint64_t parse_unsigned(const std::string& value, const char* name,
                             std::uint64_t maximum) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(),
                                             result);
    if (error != std::errc{} || end != value.data() + value.size() || result > maximum) {
        throw std::invalid_argument(std::string(name) + " must be an unsigned integer <= " +
                                    std::to_string(maximum));
    }
    return result;
}

void print_usage() {
    std::cout << "voltstore_server [--data-dir DIR] [--bind IPv4] [--port 0..65535] "
                 "[--threads 1..128] [--cache N] [--no-fsync]\n";
}

}  // namespace

int main(int argc, char** argv) {
    voltstore::EngineConfig engine_config;
    voltstore::ServerConfig server_config;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            const auto next_value = [&](const char* name) {
                if (index + 1 >= argc) {
                    throw std::invalid_argument(std::string("missing value for ") + name);
                }
                return std::string(argv[++index]);
            };

            if (option == "--data-dir") {
                engine_config.data_dir = next_value("--data-dir");
            } else if (option == "--port") {
                server_config.port = static_cast<std::uint16_t>(
                    parse_unsigned(next_value("--port"), "port", 65535));
            } else if (option == "--bind") {
                server_config.bind_address = next_value("--bind");
            } else if (option == "--threads") {
                const auto count = parse_unsigned(next_value("--threads"), "threads", 128);
                if (count == 0) {
                    throw std::invalid_argument("threads must be >= 1");
                }
                server_config.threads = static_cast<std::size_t>(count);
            } else if (option == "--cache") {
                engine_config.cache_capacity = static_cast<std::size_t>(
                    parse_unsigned(next_value("--cache"), "cache size", 1'000'000));
            } else if (option == "--no-fsync") {
                engine_config.sync_writes = false;
            } else if (option == "--help") {
                print_usage();
                return 0;
            } else {
                throw std::invalid_argument("unknown argument: " + option);
            }
        }

        voltstore::StorageEngine engine(engine_config);
        voltstore::TcpServer server(server_config, engine);
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        voltstore::log_info("starting VoltStore server");
        server.run(&shutdown_signal);
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "argument error: " << error.what() << '\n';
        print_usage();
        return 2;
    } catch (const std::exception& error) {
        voltstore::log_error(error.what());
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
