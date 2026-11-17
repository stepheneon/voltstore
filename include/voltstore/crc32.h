#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace voltstore {

// CRC32 integrity check used to detect accidental corruption in persisted records.
// The input pointer must designate `size` valid bytes when size is nonzero.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

// Convenience overload for a contiguous byte buffer.
inline std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
    return crc32(data.data(), data.size());
}
}  // namespace voltstore
