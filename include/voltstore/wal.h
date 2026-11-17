#pragma once

#include "voltstore/common.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace voltstore {

// Operation tags in the v1 WAL format.
enum class WalOp : std::uint8_t { Put = 1, Delete = 2 };

// A mutation before serialization into a WAL record.
struct WalRecord {
    WalOp op;
    std::string key;
    std::string value;
    ClockMs expires_at_ms{0};
};

// Replay counts and incomplete-tail repair status.
struct ReplayReport {
    std::size_t applied{0};
    bool ignored_partial_tail{false};
};

// Append-only WAL with CRC32 checks during replay.
// Use one writer per data directory; methods serialize file access.
class Wal {
public:
    explicit Wal(std::filesystem::path path, bool sync_on_write = true);
    ~Wal();
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Serialize and append one complete record. With sync_on_write, fsync
    // before returning. Allocation, write, and synchronization errors propagate;
    // an I/O error places this process's WAL writer in recovery-required state.
    void append(const WalRecord& record);

    // Apply checksum-validated records in order. Repairs a truncated final
    // record before returning; complete-record corruption and unreadable files
    // fail recovery. The callback is called while the internal lock is held.
    ReplayReport replay(const std::function<void(const WalRecord&)>& apply) const;

    // Reset the log after a successful snapshot. Throws on I/O failure;
    // startup recovery validates the resulting WAL state before further appends.
    void truncate();

    // Return the on-disk byte count. Filesystem errors throw.
    std::uintmax_t size_bytes() const;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    bool sync_on_write_;
    int fd_{-1};
    // The failed state blocks appends until startup recovery validates the WAL tail.
    bool failed_{false};
    mutable std::mutex mu_;
};
}  // namespace voltstore
