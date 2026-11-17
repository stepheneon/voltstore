#pragma once

#include <filesystem>

namespace voltstore {

// Holds an advisory lock for the lifetime of a storage engine. The lock file
// stays on disk so all processes coordinate through the same inode across restarts.
class DataDirectoryLock {
public:
    explicit DataDirectoryLock(const std::filesystem::path& directory);
    ~DataDirectoryLock();
    DataDirectoryLock(const DataDirectoryLock&) = delete;
    DataDirectoryLock& operator=(const DataDirectoryLock&) = delete;

private:
    int fd_{-1};
};

}  // namespace voltstore
