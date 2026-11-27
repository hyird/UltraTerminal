#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "platform/platform.transport.h"

namespace ultraterimal::agent {
void saveCredential(const std::filesystem::path& path, std::string_view value) {
    if (value.empty() || value.size() > 4096) {
        throw std::runtime_error("Invalid credential length");
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::system_error(errno, std::system_category(), "Create device credential");
    }
    try {
        std::size_t written = 0;
        while (written < value.size()) {
            const ssize_t count = ::write(fd, value.data() + written, value.size() - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::system_category(), "Write device credential");
            }
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(fd) != 0) {
            throw std::system_error(errno, std::system_category(), "Sync device credential");
        }
        ::close(fd);
    } catch (...) {
        ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
}

std::string loadCredential(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("Cannot open device credential");
    }
    struct FdGuard {
        int value;
        ~FdGuard() {
            if (value >= 0) {
                ::close(value);
            }
        }
    } guard{fd};

    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        throw std::runtime_error("Invalid device credential file");
    }
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw std::runtime_error("Device credential permissions are too broad");
    }
    if (st.st_size <= 0 || st.st_size > 4096) {
        throw std::runtime_error("Invalid device credential length");
    }
    std::string out(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t offset = 0;
    while (offset < out.size()) {
        const ssize_t count = ::read(fd, out.data() + offset, out.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::system_category(), "Read device credential");
        }
        if (count == 0) {
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    out.resize(offset);
    if (out.empty()) {
        throw std::runtime_error("Empty device credential");
    }
    return out;
}
} // namespace ultraterimal::agent
