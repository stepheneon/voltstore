#include "voltstore/snapshot.h"

#include "voltstore/crc32.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace voltstore {
namespace {

constexpr char kMagic[] = "VLTSNP1";
constexpr std::size_t kMagicLength = sizeof(kMagic) - 1;
constexpr std::size_t kMaxFieldLength = 16 * 1024 * 1024;

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    }
    return value;
}

std::uint64_t read_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

void write_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const auto result = ::write(fd, data + written, size - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            throw std::system_error(result < 0 ? errno : EIO,
                                    std::generic_category(), "write snapshot");
        }
        written += static_cast<std::size_t>(result);
    }
}

void read_exact(std::ifstream& input, char* bytes, std::size_t size,
                const char* failure_message) {
    input.read(bytes, static_cast<std::streamsize>(size));
    if (static_cast<std::size_t>(input.gcount()) != size) {
        throw std::runtime_error(failure_message);
    }
}

void sync_parent_directory(const std::filesystem::path& path) {
    const int directory = ::open(
        path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open snapshot directory");
    }

    const int sync_result = ::fsync(directory);
    const int sync_error = errno;
    const int close_result = ::close(directory);
    const int close_error = errno;
    if (sync_result != 0) {
        throw std::system_error(sync_error, std::generic_category(),
                                "fsync snapshot directory");
    }
    if (close_result != 0) {
        throw std::system_error(close_error, std::generic_category(),
                                "close snapshot directory");
    }
}

}  // namespace

void Snapshot::write_atomic(const std::filesystem::path& path,
                            const std::unordered_map<std::string, Entry>& data) {
    std::filesystem::create_directories(path.parent_path());

    std::vector<std::uint8_t> bytes(kMagic, kMagic + kMagicLength);
    append_u64(bytes, data.size());

    for (const auto& [key, entry] : data) {
        if (key.size() > kMaxFieldLength || entry.value.size() > kMaxFieldLength) {
            throw std::invalid_argument("snapshot field is too large");
        }

        std::vector<std::uint8_t> record;
        append_u32(record, static_cast<std::uint32_t>(key.size()));
        append_u32(record, static_cast<std::uint32_t>(entry.value.size()));
        append_u64(record, static_cast<std::uint64_t>(entry.expires_at_ms));
        record.insert(record.end(), key.begin(), key.end());
        record.insert(record.end(), entry.value.begin(), entry.value.end());
        append_u32(record, crc32(record));
        bytes.insert(bytes.end(), record.begin(), record.end());
    }

    // The fixed temporary path requires a trusted, single-writer directory.
    auto temporary = path;
    temporary += ".tmp";
    int fd = ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open snapshot temporary file");
    }

    try {
        write_all(fd, bytes.data(), bytes.size());
        if (::fsync(fd) != 0) {
            throw std::system_error(errno, std::generic_category(), "fsync snapshot");
        }
        if (::close(fd) != 0) {
            fd = -1;
            throw std::system_error(errno, std::generic_category(), "close snapshot");
        }
        fd = -1;
        std::filesystem::rename(temporary, path);
        sync_parent_directory(path);
    } catch (...) {
        if (fd >= 0) {
            ::close(fd);
        }
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}

std::unordered_map<std::string, Entry> Snapshot::load(
    const std::filesystem::path& path) {
    std::unordered_map<std::string, Entry> data;
    if (!std::filesystem::exists(path)) {
        return data;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open snapshot");
    }

    char magic[kMagicLength]{};
    read_exact(input, magic, sizeof(magic), "snapshot corruption: missing magic");
    if (std::string_view(magic, sizeof(magic)) !=
        std::string_view(kMagic, kMagicLength)) {
        throw std::runtime_error("snapshot corruption: bad magic");
    }

    std::uint8_t count_bytes[8]{};
    read_exact(input, reinterpret_cast<char*>(count_bytes), sizeof(count_bytes),
               "snapshot corruption: missing entry count");
    const auto count = read_u64(count_bytes);

    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint8_t header[16]{};
        read_exact(input, reinterpret_cast<char*>(header), sizeof(header),
                   "snapshot corruption: truncated record header");
        const auto key_size = read_u32(header);
        const auto value_size = read_u32(header + 4);
        const auto expiry = static_cast<ClockMs>(read_u64(header + 8));
        if (key_size > kMaxFieldLength || value_size > kMaxFieldLength) {
            throw std::runtime_error(
                "snapshot corruption: unreasonable field length");
        }

        std::vector<char> body(static_cast<std::size_t>(key_size) + value_size);
        read_exact(input, body.data(), body.size(),
                   "snapshot corruption: truncated body");

        std::vector<std::uint8_t> checksum_input(header, header + sizeof(header));
        checksum_input.insert(checksum_input.end(), body.begin(), body.end());
        std::uint8_t checksum_bytes[4]{};
        read_exact(input, reinterpret_cast<char*>(checksum_bytes),
                   sizeof(checksum_bytes), "snapshot corruption: missing checksum");
        if (crc32(checksum_input) != read_u32(checksum_bytes)) {
            throw std::runtime_error("snapshot corruption: checksum mismatch");
        }

        const std::string key(body.begin(), body.begin() + key_size);
        Entry entry;
        entry.expires_at_ms = expiry;
        entry.value.assign(body.begin() + key_size, body.end());
        if (!data.emplace(key, std::move(entry)).second) {
            throw std::runtime_error("snapshot corruption: duplicate key");
        }
    }

    if (input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("snapshot corruption: unexpected trailing bytes");
    }
    if (input.bad()) {
        throw std::runtime_error("snapshot read failed");
    }
    return data;
}

}  // namespace voltstore
