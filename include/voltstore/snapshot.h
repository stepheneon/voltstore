#pragma once

#include "voltstore/common.h"
#include <filesystem>
#include <string>
#include <unordered_map>

namespace voltstore {

// CRC32-checked binary checkpoint of the in-memory index.
// The caller must serialize writes; the directory must have one trusted writer.
class Snapshot {
public:
    // Write to a temporary file, fsync, rename over the destination, then
    // fsync the parent directory. Filesystem errors propagate; after a
    // directory-sync error, startup recovery validates the snapshot/WAL pair.
    static void write_atomic(const std::filesystem::path& path,
                             const std::unordered_map<std::string, Entry>& data);

    // Load validated records; an absent path yields an empty map. Malformed,
    // duplicate, truncated, or checksum-invalid records produce an error.
    static std::unordered_map<std::string, Entry> load(
        const std::filesystem::path& path);
};
}  // namespace voltstore
