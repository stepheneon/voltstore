#include "voltstore/wal.h"

#include "voltstore/crc32.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace voltstore {
namespace {

constexpr std::uint32_t kMagic = 0x31544C56u;  // "VLT1" in little-endian bytes.
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaxField = 16 * 1024 * 1024;
constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kChecksumBytes = 4;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_i64(std::vector<std::uint8_t>& output, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    for (int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    std::uint32_t result = 0;
    for (int index = 0; index < 4; ++index) {
        result |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    }
    return result;
}

std::int64_t read_i64(const std::uint8_t* bytes) {
    std::uint64_t result = 0;
    for (int index = 0; index < 8; ++index) {
        result |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    return static_cast<std::int64_t>(result);
}

// write() can return early; retry until all bytes are written or an error occurs.
void write_all(int fd, const std::uint8_t* bytes, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const auto count = ::write(fd, bytes + written, size - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "write WAL");
        }
        if (count == 0) {
            throw std::runtime_error("write WAL made no progress");
        }
        written += static_cast<std::size_t>(count);
    }
}

// Distinguish an incomplete final record from a read error.
bool read_exact(std::ifstream& input, char* destination, std::size_t size,
                bool& partial_tail) {
    input.read(destination, static_cast<std::streamsize>(size));
    const auto received = static_cast<std::size_t>(input.gcount());
    if (received == size) {
        return true;
    }
    if (input.bad() || !input.eof()) {
        throw std::runtime_error("WAL read failed");
    }
    partial_tail = received != 0;
    return false;
}

}  // namespace

Wal::Wal(std::filesystem::path path, bool sync_on_write)
    : path_(std::move(path)), sync_on_write_(sync_on_write) {
    std::filesystem::create_directories(path_.parent_path());
    fd_ = ::open(path_.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0600);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open WAL");
    }
}

Wal::~Wal() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void Wal::append(const WalRecord& record) {
    if (record.key.size() > kMaxField || record.value.size() > kMaxField) {
        throw std::invalid_argument("WAL field too large");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderBytes + record.key.size() + record.value.size() +
                  kChecksumBytes);
    append_u32(bytes, kMagic);
    append_u16(bytes, kVersion);
    bytes.push_back(static_cast<std::uint8_t>(record.op));
    bytes.push_back(0);
    append_u32(bytes, static_cast<std::uint32_t>(record.key.size()));
    append_u32(bytes, static_cast<std::uint32_t>(record.value.size()));
    append_i64(bytes, record.expires_at_ms);
    bytes.insert(bytes.end(), record.key.begin(), record.key.end());
    bytes.insert(bytes.end(), record.value.begin(), record.value.end());
    append_u32(bytes, crc32(bytes));

    std::lock_guard lock(mu_);
    if (failed_) {
        throw std::runtime_error("WAL disabled after an earlier I/O failure");
    }
    try {
        write_all(fd_, bytes.data(), bytes.size());
        if (sync_on_write_ && ::fsync(fd_) != 0) {
            throw std::system_error(errno, std::generic_category(), "fsync WAL");
        }
    } catch (...) {
        // A partial append requires recovery. Startup replay validates the
        // WAL tail before the next writer is allowed to proceed.
        failed_ = true;
        throw;
    }
}

ReplayReport Wal::replay(
    const std::function<void(const WalRecord&)>& apply) const {
    std::lock_guard lock(mu_);
    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        // Report an unreadable WAL during startup validation while preserving the existing durable state.
        throw std::runtime_error("unable to open WAL for recovery");
    }

    ReplayReport report;
    std::streamoff last_complete_offset = 0;

    while (true) {
        bool partial = false;
        std::array<std::uint8_t, kHeaderBytes> header{};
        if (!read_exact(input, reinterpret_cast<char*>(header.data()),
                        header.size(), partial)) {
            report.ignored_partial_tail = partial;
            break;
        }
        if (read_u32(header.data()) != kMagic) {
            throw std::runtime_error("WAL corruption: bad magic");
        }
        if (read_u16(header.data() + 4) != kVersion) {
            throw std::runtime_error("WAL corruption: unsupported version");
        }

        const auto operation = header[6];
        if (operation != static_cast<std::uint8_t>(WalOp::Put) &&
            operation != static_cast<std::uint8_t>(WalOp::Delete)) {
            throw std::runtime_error("WAL corruption: bad op");
        }
        const auto key_length = read_u32(header.data() + 8);
        const auto value_length = read_u32(header.data() + 12);
        const auto expires_at = read_i64(header.data() + 16);
        if (key_length > kMaxField || value_length > kMaxField) {
            throw std::runtime_error("WAL corruption: unreasonable field length");
        }

        std::vector<std::uint8_t> checksum_input(header.begin(), header.end());
        std::vector<char> body(static_cast<std::size_t>(key_length) + value_length);
        if (!read_exact(input, body.data(), body.size(), partial)) {
            report.ignored_partial_tail = true;
            break;
        }
        checksum_input.insert(checksum_input.end(), body.begin(), body.end());

        std::array<std::uint8_t, kChecksumBytes> checksum{};
        if (!read_exact(input, reinterpret_cast<char*>(checksum.data()),
                        checksum.size(), partial)) {
            report.ignored_partial_tail = true;
            break;
        }
        if (crc32(checksum_input) != read_u32(checksum.data())) {
            throw std::runtime_error("WAL corruption: checksum mismatch");
        }

        WalRecord record;
        record.op = static_cast<WalOp>(operation);
        record.key.assign(body.begin(), body.begin() + key_length);
        record.value.assign(body.begin() + key_length, body.end());
        record.expires_at_ms = expires_at;
        apply(record);
        ++report.applied;
        last_complete_offset = input.tellg();
    }

    if (report.ignored_partial_tail) {
        // Remove an incomplete tail before accepting new records.
        if (::ftruncate(fd_, last_complete_offset) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "repair WAL tail");
        }
        if (::fsync(fd_) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "sync repaired WAL tail");
        }
    }
    return report;
}

void Wal::truncate() {
    std::lock_guard lock(mu_);
    if (failed_) {
        throw std::runtime_error("WAL disabled after an earlier I/O failure");
    }
    if (::fsync(fd_) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "fsync WAL before truncate");
    }
    if (::ftruncate(fd_, 0) != 0) {
        throw std::system_error(errno, std::generic_category(), "truncate WAL");
    }
    if (::lseek(fd_, 0, SEEK_END) < 0) {
        throw std::system_error(errno, std::generic_category(), "seek WAL");
    }
    if (::fsync(fd_) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "fsync WAL after truncate");
    }
}

std::uintmax_t Wal::size_bytes() const {
    std::lock_guard lock(mu_);
    // Report file-size errors rather than returning zero.
    return std::filesystem::file_size(path_);
}

}  // namespace voltstore
