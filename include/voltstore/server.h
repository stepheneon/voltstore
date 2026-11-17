#pragma once

#include "voltstore/storage_engine.h"
#include "voltstore/thread_pool.h"
#include <atomic>
#include <csignal>
#include <cstdint>
#include <memory>
#include <string>

namespace voltstore {
struct ServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{7379};
    std::size_t threads{4};
};

// TCP line-protocol server backed by a bounded worker pool.
// The default configuration binds to IPv4 loopback for trusted local use.
// StorageEngine must outlive the server.
class TcpServer {
public:
    TcpServer(ServerConfig config, StorageEngine& engine);
    ~TcpServer();
    // Bind and accept clients on the calling thread until a stop request or
    // a signal-safe termination flag is observed. Throws on listener errors.
    void run(const volatile std::sig_atomic_t* termination_signal = nullptr);

    // Request shutdown. Existing clients leave when they next observe stop or
    // their bounded socket timeout expires; worker destruction joins tasks.
    void request_stop();
    // Return the assigned listener port after run() initializes the endpoint.
    std::uint16_t bound_port() const { return bound_port_; }

private:
    void handle_client(int client_fd);
    std::string execute_line(const std::string& line, bool& close_connection);
    ServerConfig config_;
    StorageEngine& engine_;
    int listen_fd_{-1};
    std::atomic<bool> stop_{false};
    std::uint16_t bound_port_{0};
    // Workers may read stop_ until the pool has joined them.
    ThreadPool pool_;
};
}  // namespace voltstore
