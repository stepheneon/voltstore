#include "voltstore/data_directory_lock.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace voltstore {

DataDirectoryLock::DataDirectoryLock(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto lock_path = directory / ".voltstore.lock";
    fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open data directory lock");
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(fd_);
        fd_ = -1;
        if (error == EWOULDBLOCK || error == EAGAIN) {
            throw std::runtime_error("data directory is already in use");
        }
        throw std::system_error(error, std::generic_category(), "lock data directory");
    }
}

DataDirectoryLock::~DataDirectoryLock() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

}  // namespace voltstore
