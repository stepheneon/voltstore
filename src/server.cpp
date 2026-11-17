#include "voltstore/server.h"

#include "voltstore/logger.h"
#include "voltstore/protocol.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace voltstore {
namespace {

constexpr std::size_t kMaxRequestLine = 64 * 1024;
constexpr int kPollIntervalMs = 200;
constexpr int kSocketTimeoutUs = 500'000;
constexpr auto kMaxIdleTime = std::chrono::seconds(30);

class ClientSocket {
public:
    explicit ClientSocket(int fd) : fd_(fd) {}

    ~ClientSocket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

std::string escape_scan_value(const std::string& value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char byte : value) {
        switch (byte) {
        case '\\':
            encoded += "\\\\";
            break;
        case '\t':
            encoded += "\\t";
            break;
        case '\n':
            encoded += "\\n";
            break;
        case '\r':
            encoded += "\\r";
            break;
        default:
            if (byte < 0x20 || byte == 0x7F) {
                encoded += "\\x";
                encoded += kHex[byte >> 4];
                encoded += kHex[byte & 0x0F];
            } else {
                encoded += static_cast<char>(byte);
            }
        }
    }
    return encoded;
}

bool send_all(int fd, const std::string& response) {
    std::size_t sent = 0;
    while (sent < response.size()) {
        const auto count = ::send(fd, response.data() + sent,
                                  response.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace

TcpServer::TcpServer(ServerConfig config, StorageEngine& engine)
    : config_(std::move(config)), engine_(engine), pool_(config_.threads) {
    if (config_.threads == 0 || config_.threads > 128) {
        throw std::invalid_argument("thread count must be in 1..128");
    }
}

TcpServer::~TcpServer() {
    request_stop();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

void TcpServer::request_stop() {
    // Close the listener on the server thread after the accept loop exits.
    stop_.store(true);
}

void TcpServer::run(const volatile std::sig_atomic_t* termination_signal) {
    if (listen_fd_ >= 0) {
        throw std::logic_error("server is already running");
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket creation failed");
    }

    try {
        const int enable = 1;
        if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                         &enable, sizeof(enable)) != 0) {
            throw std::runtime_error("setsockopt(SO_REUSEADDR) failed");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.port);
        if (::inet_pton(AF_INET, config_.bind_address.c_str(),
                        &address.sin_addr) != 1) {
            throw std::invalid_argument("bind address must be an IPv4 address");
        }
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0) {
            throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
        }
        if (::listen(listen_fd_, 128) != 0) {
            throw std::runtime_error("listen failed");
        }

        socklen_t address_length = sizeof(address);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                          &address_length) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        bound_port_ = ntohs(address.sin_port);
        std::cout << "READY " << config_.bind_address << ':' << bound_port_ << std::endl;

        while (!stop_.load() &&
               (termination_signal == nullptr || *termination_signal == 0)) {
            pollfd listener{listen_fd_, POLLIN, 0};
            const int poll_result = ::poll(&listener, 1, kPollIntervalMs);
            if (poll_result < 0 && errno == EINTR) {
                continue;
            }
            if (poll_result < 0) {
                throw std::runtime_error("poll failed");
            }
            if (poll_result == 0) {
                continue;
            }
            if ((listener.revents & POLLIN) == 0) {
                throw std::runtime_error("listening socket became unavailable");
            }

            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0 && errno == EINTR) {
                continue;
            }
            if (client < 0) {
                throw std::runtime_error("accept failed");
            }
            try {
                pool_.submit([this, client] { handle_client(client); });
            } catch (const std::runtime_error&) {
                // Reject connections when the worker queue is full.
                send_all(client, "ERR server busy\n");
                ::close(client);
            } catch (...) {
                ::close(client);
                throw;
            }
        }
    } catch (...) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        request_stop();
        throw;
    }

    request_stop();
    ::close(listen_fd_);
    listen_fd_ = -1;
}

void TcpServer::handle_client(int client_fd) {
    ClientSocket socket(client_fd);
    timeval receive_timeout{0, kSocketTimeoutUs};
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                     &receive_timeout, sizeof(receive_timeout)) != 0 ||
        ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
                     &receive_timeout, sizeof(receive_timeout)) != 0) {
        return;
    }

    char chunk[4096];
    std::string pending;
    auto last_activity = std::chrono::steady_clock::now();
    bool close_connection = false;

    while (!close_connection && !stop_.load()) {
        const auto count = ::recv(socket.get(), chunk, sizeof(chunk), 0);
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (std::chrono::steady_clock::now() - last_activity > kMaxIdleTime) {
                    break;
                }
                continue;
            }
            break;
        }
        last_activity = std::chrono::steady_clock::now();
        pending.append(chunk, static_cast<std::size_t>(count));

        while (true) {
            const auto newline = pending.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            if (newline > kMaxRequestLine) {
                send_all(socket.get(), "ERR request line too long\n");
                return;
            }
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!send_all(socket.get(), execute_line(line, close_connection))) {
                return;
            }
            if (close_connection) {
                break;
            }
        }
        if (pending.size() > kMaxRequestLine) {
            send_all(socket.get(), "ERR request line too long\n");
            break;
        }
    }
}

std::string TcpServer::execute_line(const std::string& line, bool& close_connection) {
    try {
        const Command command = parse_command(line);
        switch (command.type) {
        case CommandType::Put:
            engine_.put(command.key, command.value, command.ttl_ms);
            return "OK\n";
        case CommandType::Get: {
            const auto value = engine_.get(command.key);
            return value ? "VALUE " + std::to_string(value->size()) + " " + *value + "\n"
                         : "NOT_FOUND\n";
        }
        case CommandType::Delete:
            return engine_.erase(command.key) ? "DELETED\n" : "NOT_FOUND\n";
        case CommandType::Exists:
            return engine_.exists(command.key) ? "1\n" : "0\n";
        case CommandType::Scan: {
            const auto items = engine_.scan_prefix(command.key, command.limit);
            std::ostringstream response;
            response << "ITEMS " << items.size() << '\n';
            for (const auto& item : items) {
                response << item.key << '\t' << escape_scan_value(item.value) << '\n';
            }
            response << "END\n";
            return response.str();
        }
        case CommandType::Stats: {
            const auto stats = engine_.stats();
            std::ostringstream response;
            response << "STATS live_keys=" << stats.live_keys
                     << " cache_items=" << stats.cache_items
                     << " wal_bytes=" << stats.wal_bytes
                     << " puts=" << stats.puts
                     << " gets=" << stats.gets
                     << " deletes=" << stats.deletes
                     << " cache_hits=" << stats.cache_hits
                     << " cache_misses=" << stats.cache_misses
                     << " snapshots=" << stats.snapshots << '\n';
            return response.str();
        }
        case CommandType::Snapshot:
            engine_.snapshot();
            return "SNAPSHOT_OK\n";
        case CommandType::Ping:
            return "PONG\n";
        case CommandType::Quit:
            close_connection = true;
            return "BYE\n";
        case CommandType::Invalid:
            return "ERR " + command.error + "\n";
        }
    } catch (const std::exception& error) {
        // Log the detailed error locally; send a generic message to the client.
        log_error(std::string("request failed: ") + error.what());
        return "ERR storage operation failed\n";
    }
    return "ERR unsupported command\n";
}

}  // namespace voltstore
