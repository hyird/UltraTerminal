#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

#include "platform/platform.transport.h"

namespace ultraterimal::agent {
namespace {
std::atomic<std::uint64_t> nextUploadNonce{1};

class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    int release() noexcept { return std::exchange(fd_, -1); }

  private:
    int fd_ = -1;
};

struct ScopedDir {
    DIR* dir = nullptr;
    ~ScopedDir() {
        if (dir) {
            ::closedir(dir);
        }
    }
};

wire::FileErrorCode mapErrno(int err) noexcept {
    switch (err) {
    case 0:
        return wire::FILE_ERROR_NONE;
    case ENOENT:
        return wire::FILE_ERROR_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:
        return wire::FILE_ERROR_PERMISSION_DENIED;
    case EEXIST:
        return wire::FILE_ERROR_ALREADY_EXISTS;
    case EISDIR:
        return wire::FILE_ERROR_IS_DIRECTORY;
    case ENOTDIR:
        return wire::FILE_ERROR_NOT_DIRECTORY;
    case ENAMETOOLONG:
    case EINVAL:
        return wire::FILE_ERROR_INVALID_PATH;
    default:
        return wire::FILE_ERROR_IO_FAILED;
    }
}

bool normalizePosixPath(std::string_view input, std::string& normalized, std::string& parent) {
    if (input.empty() || input.size() > protocol::kMaximumFilePathBytes || input.find('\0') != std::string_view::npos) {
        return false;
    }
    if (input.front() != '/') {
        return false;
    }
    std::vector<std::string_view> parts;
    std::size_t pos = 0;
    while (pos < input.size()) {
        while (pos < input.size() && input[pos] == '/') {
            ++pos;
        }
        if (pos >= input.size()) {
            break;
        }
        const std::size_t next = input.find('/', pos);
        const std::string_view segment = next == std::string_view::npos ? input.substr(pos) : input.substr(pos, next - pos);
        if (segment == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (segment != ".") {
            parts.push_back(segment);
        }
        pos = next == std::string_view::npos ? input.size() : next + 1;
    }

    if (parts.empty()) {
        normalized = "/";
        parent.clear();
        return true;
    }

    normalized.clear();
    for (const auto part : parts) {
        normalized.push_back('/');
        normalized.append(part);
    }
    if (parts.size() == 1) {
        parent = "/";
    } else {
        parent.clear();
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            parent.push_back('/');
            parent.append(parts[i]);
        }
    }
    return normalized.size() <= protocol::kMaximumFilePathBytes;
}

std::string posixFilename(std::string_view normalized) {
    if (normalized == "/") {
        return "root";
    }
    const auto slash = normalized.rfind('/');
    if (slash == std::string_view::npos || slash + 1 >= normalized.size()) {
        return std::string(normalized);
    }
    return std::string(normalized.substr(slash + 1));
}

std::string posixParentDir(std::string_view normalized) {
    const auto slash = normalized.rfind('/');
    if (slash == std::string_view::npos || slash == 0) {
        return "/";
    }
    return std::string(normalized.substr(0, slash));
}

std::pair<std::uint16_t, std::uint16_t> unixSecondsToDosDateTime(std::time_t unixSeconds) {
    std::tm tm{};
    if (!::gmtime_r(&unixSeconds, &tm)) {
        return {0, 0x0021};
    }
    const int year = std::clamp(tm.tm_year + 1900, 1980, 2107) - 1980;
    const auto dosTime = static_cast<std::uint16_t>(((tm.tm_hour & 0x1F) << 11) | ((tm.tm_min & 0x3F) << 5) | ((tm.tm_sec / 2) & 0x1F));
    const auto dosDate = static_cast<std::uint16_t>((year << 9) | (((tm.tm_mon + 1) & 0x0F) << 5) | (tm.tm_mday & 0x1F));
    return {dosTime, dosDate};
}

bool shouldStoreUncompressed(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) {
        return false;
    }
    std::string ext(name.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::array<std::string_view, 22> kPrecompressed = {
        "zip",
        "7z",
        "rar",
        "gz",
        "tgz",
        "xz",
        "bz2",
        "zst",
        "mp4",
        "mkv",
        "avi",
        "mov",
        "mp3",
        "aac",
        "flac",
        "jpg",
        "jpeg",
        "png",
        "webp",
        "gif",
        "deb",
        "rpm",
    };
    return std::find(kPrecompressed.begin(), kPrecompressed.end(), ext) != kPrecompressed.end();
}

void appendLe16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void appendLe32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void appendLe64(std::string& out, std::uint64_t v) {
    appendLe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
    appendLe32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

struct ZipCentralEntry {
    std::string name;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint16_t dosTime = 0;
    std::uint16_t dosDate = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t localHeaderOffset = 0;
    bool directory = false;
};

struct PendingZipTask {
    std::string fullPath;
    std::string archiveName;
    bool directory = false;
    std::uint32_t depth = 0;
    std::time_t modified = 0;
};
} // namespace

wire::FileListResult listPlatformFiles(const wire::FileListRequest& request) {
    wire::FileListResult result;
    result.set_request_id(request.request_id());

    const std::string_view rawPath = request.path().empty() ? std::string_view{"/"} : std::string_view{request.path()};
    std::string normalized;
    std::string parent;
    if (!normalizePosixPath(rawPath, normalized, parent)) {
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_INVALID_PATH);
        return result;
    }

    struct stat dirStat{};
    if (::stat(normalized.c_str(), &dirStat) != 0) {
        result.set_success(false);
        result.set_error(mapErrno(errno));
        return result;
    }
    if (!S_ISDIR(dirStat.st_mode)) {
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_NOT_DIRECTORY);
        return result;
    }

    ScopedDir dir{::opendir(normalized.c_str())};
    if (!dir.dir) {
        result.set_success(false);
        result.set_error(mapErrno(errno));
        return result;
    }

    struct RawEntry {
        std::string name;
        std::string sortKey;
        bool directory = false;
        std::uint64_t size = 0;
        std::int64_t modified = 0;
    };
    std::vector<RawEntry> entries;
    while (true) {
        errno = 0;
        dirent* ent = ::readdir(dir.dir);
        if (!ent) {
            if (errno != 0) {
                result.set_success(false);
                result.set_error(mapErrno(errno));
                return result;
            }
            break;
        }
        const std::string_view nameView(ent->d_name);
        if (nameView == "." || nameView == ".." || nameView.empty() || nameView.size() > 512) {
            continue;
        }
        std::string fullPath = normalized == "/" ? ("/" + std::string(nameView)) : (normalized + "/" + std::string(nameView));
        struct stat st{};
        if (::stat(fullPath.c_str(), &st) != 0 && ::lstat(fullPath.c_str(), &st) != 0) {
            continue;
        }
        RawEntry item;
        item.name = std::string(nameView);
        item.sortKey = item.name;
        std::transform(item.sortKey.begin(), item.sortKey.end(), item.sortKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        item.directory = S_ISDIR(st.st_mode);
        if (!item.directory && S_ISREG(st.st_mode) && st.st_size > 0) {
            item.size = static_cast<std::uint64_t>(st.st_size);
        }
        item.modified = static_cast<std::int64_t>(st.st_mtime);
        entries.push_back(std::move(item));
    }

    std::sort(entries.begin(), entries.end(), [](const RawEntry& a, const RawEntry& b) {
        if (a.directory != b.directory) {
            return a.directory;
        }
        return a.sortKey < b.sortKey;
    });

    result.set_success(true);
    result.set_error(wire::FILE_ERROR_NONE);
    result.set_normalized_path(normalized);
    result.set_parent_path(parent);

    std::size_t estimatedBytes = normalized.size() + parent.size() + 64;
    for (const auto& item : entries) {
        const std::size_t entryBytes = item.name.size() + 36;
        if (static_cast<std::size_t>(result.entries_size()) >= protocol::kMaximumFileListEntries ||
            estimatedBytes + entryBytes > protocol::kMaximumFileListControlBytes) {
            result.set_truncated(true);
            break;
        }
        auto* out = result.add_entries();
        out->set_name(item.name);
        out->set_directory(item.directory);
        out->set_size(item.size);
        out->set_modified_unix_seconds(item.modified);
        estimatedBytes += entryBytes;
    }
    return result;
}

wire::FileStatResult statPlatformFile(const wire::FileStatRequest& request) {
    wire::FileStatResult result;
    result.set_request_id(request.request_id());
    std::string normalized;
    std::string parent;
    if (!normalizePosixPath(request.path(), normalized, parent)) {
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_INVALID_PATH);
        return result;
    }
    struct stat st{};
    if (::stat(normalized.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            result.set_success(true);
            result.set_error(wire::FILE_ERROR_NONE);
            result.set_exists(false);
            result.set_name(posixFilename(normalized));
            return result;
        }
        result.set_success(false);
        result.set_error(mapErrno(errno));
        return result;
    }
    result.set_success(true);
    result.set_error(wire::FILE_ERROR_NONE);
    result.set_exists(true);
    const bool isDir = S_ISDIR(st.st_mode);
    result.set_directory(isDir);
    if (!isDir && S_ISREG(st.st_mode) && st.st_size > 0) {
        result.set_size(static_cast<std::uint64_t>(st.st_size));
    }
    result.set_name(posixFilename(normalized));
    return result;
}

struct FileDownloadStream::State {
    std::string filename;
    std::uint64_t declaredSize = 0;
    bool archiveZip = false;
    std::uint64_t bytesProduced = 0;
    bool finished = false;

    UniqueFd singleFile;

    std::deque<PendingZipTask> queue;
    std::vector<ZipCentralEntry> centralDirectory;
    std::uint32_t archivedEntries = 0;
    std::uint64_t totalUncompressed = 0;

    UniqueFd activeFile;
    ZipCentralEntry activeEntry;
    bool activeDeflate = false;
    bool activeFileEof = false;
    z_stream zstream{};
    bool zstreamInitialized = false;
    std::array<unsigned char, 64 * 1024> readBuffer{};
    std::array<unsigned char, 64 * 1024> deflateBuffer{};

    std::string pending;
    std::size_t pendingOffset = 0;
    std::uint64_t streamOffset = 0;
    bool centralWritten = false;

    ~State() {
        if (zstreamInitialized) {
            ::deflateEnd(&zstream);
            zstreamInitialized = false;
        }
    }

    void appendOut(std::string_view bytes) {
        pending.append(bytes);
        streamOffset += bytes.size();
    }

    wire::FileErrorCode beginDirectory(const PendingZipTask& task) {
        if (archivedEntries >= protocol::kMaximumArchiveFiles) {
            return wire::FILE_ERROR_LIMIT_EXCEEDED;
        }
        ++archivedEntries;
        const auto [dosTime, dosDate] = unixSecondsToDosDateTime(task.modified);
        std::string dirName = task.archiveName;
        if (dirName.empty() || dirName.back() != '/') {
            dirName.push_back('/');
        }
        ZipCentralEntry entry;
        entry.name = dirName;
        entry.method = 0;
        entry.flags = 0x0800; // UTF-8
        entry.dosTime = dosTime;
        entry.dosDate = dosDate;
        entry.localHeaderOffset = streamOffset;
        entry.directory = true;
        centralDirectory.push_back(entry);

        std::string header;
        header.reserve(30 + dirName.size());
        appendLe32(header, 0x04034b50);
        appendLe16(header, 45);
        appendLe16(header, entry.flags);
        appendLe16(header, entry.method);
        appendLe16(header, entry.dosTime);
        appendLe16(header, entry.dosDate);
        appendLe32(header, 0);
        appendLe32(header, 0);
        appendLe32(header, 0);
        appendLe16(header, static_cast<std::uint16_t>(dirName.size()));
        appendLe16(header, 0);
        header.append(dirName);
        appendOut(header);
        return enqueueDirectoryChildren(task);
    }

    wire::FileErrorCode beginFile(const PendingZipTask& task) {
        if (archivedEntries >= protocol::kMaximumArchiveFiles) {
            return wire::FILE_ERROR_LIMIT_EXCEEDED;
        }
        ++archivedEntries;

        const int raw = ::open(task.fullPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (raw < 0) {
            return mapErrno(errno);
        }
        struct stat st{};
        if (::fstat(raw, &st) != 0) {
            const int err = errno;
            ::close(raw);
            return mapErrno(err);
        }
        if (!S_ISREG(st.st_mode)) {
            ::close(raw);
            return wire::FILE_ERROR_NONE;
        }
        activeFile.reset(raw);
        activeFileEof = false;

        const auto [dosTime, dosDate] = unixSecondsToDosDateTime(task.modified);
        activeEntry = {};
        activeEntry.name = task.archiveName;
        activeDeflate = !shouldStoreUncompressed(task.archiveName);
        activeEntry.method = activeDeflate ? Z_DEFLATED : 0;
        activeEntry.flags = 0x0808; // UTF-8 + data descriptor
        activeEntry.dosTime = dosTime;
        activeEntry.dosDate = dosDate;
        activeEntry.crc = 0;
        activeEntry.localHeaderOffset = streamOffset;
        activeEntry.directory = false;

        if (activeDeflate) {
            if (zstreamInitialized) {
                ::deflateEnd(&zstream);
                zstreamInitialized = false;
            }
            zstream = {};
            const int rc = ::deflateInit2(&zstream, 5, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            if (rc != Z_OK) {
                activeFile.reset();
                return wire::FILE_ERROR_IO_FAILED;
            }
            zstreamInitialized = true;
        }

        std::string header;
        header.reserve(30 + activeEntry.name.size());
        appendLe32(header, 0x04034b50);
        appendLe16(header, 45);
        appendLe16(header, activeEntry.flags);
        appendLe16(header, activeEntry.method);
        appendLe16(header, activeEntry.dosTime);
        appendLe16(header, activeEntry.dosDate);
        appendLe32(header, 0);
        appendLe32(header, 0);
        appendLe32(header, 0);
        appendLe16(header, static_cast<std::uint16_t>(activeEntry.name.size()));
        appendLe16(header, 0);
        header.append(activeEntry.name);
        appendOut(header);
        return wire::FILE_ERROR_NONE;
    }

    wire::FileErrorCode finishActiveFile() {
        std::string descriptor;
        descriptor.reserve(24);
        appendLe32(descriptor, 0x08074b50);
        appendLe32(descriptor, activeEntry.crc);
        appendLe64(descriptor, activeEntry.compressedSize);
        appendLe64(descriptor, activeEntry.uncompressedSize);
        appendOut(descriptor);

        centralDirectory.push_back(activeEntry);
        activeFile.reset();
        if (zstreamInitialized) {
            ::deflateEnd(&zstream);
            zstreamInitialized = false;
        }
        return wire::FILE_ERROR_NONE;
    }

    wire::FileErrorCode enqueueDirectoryChildren(const PendingZipTask& dirTask) {
        if (dirTask.depth >= protocol::kMaximumArchiveDepth) {
            return wire::FILE_ERROR_LIMIT_EXCEEDED;
        }
        ScopedDir dir{::opendir(dirTask.fullPath.c_str())};
        if (!dir.dir) {
            return mapErrno(errno);
        }

        std::vector<PendingZipTask> children;
        while (true) {
            errno = 0;
            dirent* ent = ::readdir(dir.dir);
            if (!ent) {
                if (errno != 0) {
                    return mapErrno(errno);
                }
                break;
            }
            const std::string_view nameView(ent->d_name);
            if (nameView == "." || nameView == ".." || nameView.empty()) {
                continue;
            }
            PendingZipTask child;
            child.fullPath = dirTask.fullPath == "/" ? ("/" + std::string(nameView)) : (dirTask.fullPath + "/" + std::string(nameView));
            struct stat st{};
            if (::lstat(child.fullPath.c_str(), &st) != 0) {
                return mapErrno(errno);
            }
            // Skip symlinks, FIFOs, sockets and device nodes to prevent cycles or blocking reads.
            if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
                continue;
            }
            child.archiveName = dirTask.archiveName.empty() ? std::string(nameView) : (dirTask.archiveName + "/" + std::string(nameView));
            child.directory = S_ISDIR(st.st_mode);
            child.depth = dirTask.depth + 1;
            child.modified = st.st_mtime;
            children.push_back(std::move(child));
        }

        std::sort(children.begin(), children.end(), [](const PendingZipTask& a, const PendingZipTask& b) {
            return a.archiveName < b.archiveName;
        });

        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            queue.push_front(std::move(*it));
        }
        return wire::FILE_ERROR_NONE;
    }

    wire::FileErrorCode advanceZip(std::size_t targetBytes) {
        while ((pending.size() - pendingOffset) < targetBytes && !finished) {
            if (activeFile) {
                if (!activeFileEof) {
                    const ssize_t readBytes = ::read(activeFile.get(), readBuffer.data(), readBuffer.size());
                    if (readBytes < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        return mapErrno(errno);
                    }
                    if (readBytes == 0) {
                        activeFileEof = true;
                    } else {
                        const auto count = static_cast<std::size_t>(readBytes);
                        if (totalUncompressed + count > protocol::kMaximumArchiveBytes) {
                            return wire::FILE_ERROR_LIMIT_EXCEEDED;
                        }
                        totalUncompressed += count;
                        activeEntry.uncompressedSize += count;
                        activeEntry.crc = static_cast<std::uint32_t>(::crc32(activeEntry.crc, readBuffer.data(), static_cast<uInt>(count)));
                        if (!activeDeflate) {
                            activeEntry.compressedSize += count;
                            appendOut(std::string_view(reinterpret_cast<const char*>(readBuffer.data()), count));
                        } else {
                            zstream.next_in = readBuffer.data();
                            zstream.avail_in = static_cast<uInt>(count);
                            while (zstream.avail_in > 0) {
                                zstream.next_out = deflateBuffer.data();
                                zstream.avail_out = static_cast<uInt>(deflateBuffer.size());
                                const int rc = ::deflate(&zstream, Z_NO_FLUSH);
                                if (rc != Z_OK) {
                                    return wire::FILE_ERROR_IO_FAILED;
                                }
                                const std::size_t produced = deflateBuffer.size() - zstream.avail_out;
                                if (produced > 0) {
                                    activeEntry.compressedSize += produced;
                                    appendOut(std::string_view(reinterpret_cast<const char*>(deflateBuffer.data()), produced));
                                }
                            }
                        }
                    }
                }
                if (activeFileEof) {
                    if (activeDeflate) {
                        zstream.next_in = nullptr;
                        zstream.avail_in = 0;
                        int rc = Z_OK;
                        do {
                            zstream.next_out = deflateBuffer.data();
                            zstream.avail_out = static_cast<uInt>(deflateBuffer.size());
                            rc = ::deflate(&zstream, Z_FINISH);
                            if (rc != Z_OK && rc != Z_STREAM_END) {
                                return wire::FILE_ERROR_IO_FAILED;
                            }
                            const std::size_t produced = deflateBuffer.size() - zstream.avail_out;
                            if (produced > 0) {
                                activeEntry.compressedSize += produced;
                                appendOut(std::string_view(reinterpret_cast<const char*>(deflateBuffer.data()), produced));
                            }
                        } while (rc != Z_STREAM_END);
                    }
                    const auto err = finishActiveFile();
                    if (err != wire::FILE_ERROR_NONE) {
                        return err;
                    }
                }
                continue;
            }

            if (!queue.empty()) {
                auto nextTask = std::move(queue.front());
                queue.pop_front();
                const auto err = nextTask.directory ? beginDirectory(nextTask) : beginFile(nextTask);
                if (err != wire::FILE_ERROR_NONE) {
                    return err;
                }
                continue;
            }

            if (!centralWritten) {
                writeCentralDirectory();
                centralWritten = true;
                finished = true;
            }
        }
        return wire::FILE_ERROR_NONE;
    }

    void writeCentralDirectory() {
        const std::uint64_t cdOffset = streamOffset;
        std::string buffer;
        for (const auto& entry : centralDirectory) {
            std::string extra;
            extra.reserve(28);
            appendLe16(extra, 0x0001);
            appendLe16(extra, 24);
            appendLe64(extra, entry.uncompressedSize);
            appendLe64(extra, entry.compressedSize);
            appendLe64(extra, entry.localHeaderOffset);

            appendLe32(buffer, 0x02014b50);
            appendLe16(buffer, (3U << 8) | 45U);
            appendLe16(buffer, 45);
            appendLe16(buffer, entry.flags);
            appendLe16(buffer, entry.method);
            appendLe16(buffer, entry.dosTime);
            appendLe16(buffer, entry.dosDate);
            appendLe32(buffer, entry.crc);
            appendLe32(buffer, 0xFFFFFFFFU);
            appendLe32(buffer, 0xFFFFFFFFU);
            appendLe16(buffer, static_cast<std::uint16_t>(entry.name.size()));
            appendLe16(buffer, static_cast<std::uint16_t>(extra.size()));
            appendLe16(buffer, 0);
            appendLe16(buffer, 0);
            appendLe16(buffer, 0);
            appendLe32(buffer, entry.directory ? 0x10U : 0U);
            appendLe32(buffer, 0xFFFFFFFFU);
            buffer.append(entry.name);
            buffer.append(extra);
        }
        const std::uint64_t cdSize = buffer.size();
        const std::uint64_t totalEntries = centralDirectory.size();
        const std::uint64_t zip64EocdOffset = cdOffset + cdSize;

        appendLe32(buffer, 0x06064b50);
        appendLe64(buffer, 44);
        appendLe16(buffer, (3U << 8) | 45U);
        appendLe16(buffer, 45);
        appendLe32(buffer, 0);
        appendLe32(buffer, 0);
        appendLe64(buffer, totalEntries);
        appendLe64(buffer, totalEntries);
        appendLe64(buffer, cdSize);
        appendLe64(buffer, cdOffset);

        appendLe32(buffer, 0x07064b50);
        appendLe32(buffer, 0);
        appendLe64(buffer, zip64EocdOffset);
        appendLe32(buffer, 1);

        appendLe32(buffer, 0x06054b50);
        appendLe16(buffer, 0);
        appendLe16(buffer, 0);
        appendLe16(buffer, static_cast<std::uint16_t>(std::min<std::uint64_t>(totalEntries, 0xFFFFU)));
        appendLe16(buffer, static_cast<std::uint16_t>(std::min<std::uint64_t>(totalEntries, 0xFFFFU)));
        appendLe32(buffer, static_cast<std::uint32_t>(std::min<std::uint64_t>(cdSize, 0xFFFFFFFFULL)));
        appendLe32(buffer, static_cast<std::uint32_t>(std::min<std::uint64_t>(cdOffset, 0xFFFFFFFFULL)));
        appendLe16(buffer, 0);

        appendOut(buffer);
    }
};

FileDownloadStream::FileDownloadStream(std::unique_ptr<State> state) : state_(std::move(state)) {}
FileDownloadStream::~FileDownloadStream() = default;

const std::string& FileDownloadStream::filename() const noexcept { return state_->filename; }
std::uint64_t FileDownloadStream::declaredSize() const noexcept { return state_->declaredSize; }
bool FileDownloadStream::isArchiveZip() const noexcept { return state_->archiveZip; }
std::uint64_t FileDownloadStream::bytesProduced() const noexcept { return state_->bytesProduced; }

std::expected<std::unique_ptr<FileDownloadStream>, wire::FileErrorCode> FileDownloadStream::open(std::string_view utf8Path) {
    std::string normalized;
    std::string parent;
    if (!normalizePosixPath(utf8Path, normalized, parent)) {
        return std::unexpected(wire::FILE_ERROR_INVALID_PATH);
    }
    struct stat st{};
    if (::stat(normalized.c_str(), &st) != 0) {
        return std::unexpected(mapErrno(errno));
    }

    auto state = std::make_unique<State>();
    std::string leaf = posixFilename(normalized);
    if (S_ISDIR(st.st_mode)) {
        PendingZipTask rootTask;
        rootTask.fullPath = normalized;
        rootTask.archiveName = leaf;
        rootTask.directory = true;
        rootTask.depth = 0;
        rootTask.modified = st.st_mtime;

        const auto check = state->enqueueDirectoryChildren(rootTask);
        if (check != wire::FILE_ERROR_NONE) {
            return std::unexpected(check);
        }
        state->filename = leaf + ".zip";
        state->declaredSize = 0;
        state->archiveZip = true;
        return std::unique_ptr<FileDownloadStream>(new FileDownloadStream(std::move(state)));
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(wire::FILE_ERROR_PERMISSION_DENIED);
    }

    const int raw = ::open(normalized.c_str(), O_RDONLY | O_CLOEXEC);
    if (raw < 0) {
        return std::unexpected(mapErrno(errno));
    }
    state->singleFile.reset(raw);
    state->filename = std::move(leaf);
    state->declaredSize = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    state->archiveZip = false;
    return std::unique_ptr<FileDownloadStream>(new FileDownloadStream(std::move(state)));
}

std::expected<std::string, wire::FileErrorCode> FileDownloadStream::readChunk(std::size_t maxBytes) {
    if (maxBytes == 0 || maxBytes > protocol::kFileTransferChunkBytes) {
        maxBytes = protocol::kFileTransferChunkBytes;
    }
    if (!state_->archiveZip) {
        if (state_->finished || !state_->singleFile) {
            return std::string{};
        }
        std::string chunk(maxBytes, '\0');
        ssize_t readBytes = -1;
        do {
            readBytes = ::read(state_->singleFile.get(), chunk.data(), chunk.size());
        } while (readBytes < 0 && errno == EINTR);
        if (readBytes < 0) {
            return std::unexpected(mapErrno(errno));
        }
        chunk.resize(static_cast<std::size_t>(readBytes));
        if (readBytes == 0) {
            state_->finished = true;
            state_->singleFile.reset();
        } else {
            state_->bytesProduced += static_cast<std::uint64_t>(readBytes);
        }
        return chunk;
    }

    const auto err = state_->advanceZip(maxBytes);
    if (err != wire::FILE_ERROR_NONE) {
        return std::unexpected(err);
    }
    const std::size_t available = state_->pending.size() - state_->pendingOffset;
    if (available == 0) {
        return std::string{};
    }
    const std::size_t take = std::min(maxBytes, available);
    std::string chunk(state_->pending.data() + state_->pendingOffset, take);
    state_->pendingOffset += take;
    if (state_->pendingOffset == state_->pending.size()) {
        state_->pending.clear();
        state_->pendingOffset = 0;
    }
    state_->bytesProduced += take;
    return chunk;
}

struct FileUploadSink::State {
    std::string targetPath;
    std::string tempPath;
    std::string filename;
    bool overwrite = false;
    bool committed = false;
    std::uint64_t bytesWritten = 0;
    UniqueFd file;

    ~State() {
        file.reset();
        if (!committed && !tempPath.empty()) {
            ::unlink(tempPath.c_str());
        }
    }
};

FileUploadSink::FileUploadSink(std::unique_ptr<State> state) : state_(std::move(state)) {}
FileUploadSink::~FileUploadSink() = default;

const std::string& FileUploadSink::filename() const noexcept { return state_->filename; }
std::uint64_t FileUploadSink::bytesWritten() const noexcept { return state_->bytesWritten; }

std::expected<std::unique_ptr<FileUploadSink>, wire::FileErrorCode> FileUploadSink::open(std::string_view utf8Path, bool overwrite) {
    std::string normalized;
    std::string parent;
    if (!normalizePosixPath(utf8Path, normalized, parent)) {
        return std::unexpected(wire::FILE_ERROR_INVALID_PATH);
    }
    if (normalized == "/") {
        return std::unexpected(wire::FILE_ERROR_IS_DIRECTORY);
    }
    struct stat targetStat{};
    if (::lstat(normalized.c_str(), &targetStat) == 0) {
        if (S_ISDIR(targetStat.st_mode)) {
            return std::unexpected(wire::FILE_ERROR_IS_DIRECTORY);
        }
        if (!overwrite) {
            return std::unexpected(wire::FILE_ERROR_ALREADY_EXISTS);
        }
    } else if (errno != ENOENT) {
        return std::unexpected(mapErrno(errno));
    }

    const std::string parentDir = posixParentDir(normalized);
    struct stat parentStat{};
    if (::stat(parentDir.c_str(), &parentStat) != 0) {
        return std::unexpected(mapErrno(errno));
    }
    if (!S_ISDIR(parentStat.st_mode)) {
        return std::unexpected(wire::FILE_ERROR_NOT_DIRECTORY);
    }

    const auto nonce = nextUploadNonce.fetch_add(1, std::memory_order_relaxed);
    const std::string tempPath = normalized + "." + std::to_string(::getpid()) + "." + std::to_string(nonce) + ".utpart";
    const int raw = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (raw < 0) {
        return std::unexpected(mapErrno(errno));
    }

    auto state = std::make_unique<State>();
    state->targetPath = normalized;
    state->tempPath = tempPath;
    state->filename = posixFilename(normalized);
    state->overwrite = overwrite;
    state->file.reset(raw);
    return std::unique_ptr<FileUploadSink>(new FileUploadSink(std::move(state)));
}

wire::FileErrorCode FileUploadSink::writeChunk(std::string_view data) {
    if (!state_->file) {
        return wire::FILE_ERROR_IO_FAILED;
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(state_->file.get(), data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return mapErrno(errno);
        }
        if (written == 0) {
            return wire::FILE_ERROR_IO_FAILED;
        }
        offset += static_cast<std::size_t>(written);
        state_->bytesWritten += static_cast<std::uint64_t>(written);
    }
    return wire::FILE_ERROR_NONE;
}

wire::FileErrorCode FileUploadSink::commit() {
    if (!state_->file) {
        return wire::FILE_ERROR_IO_FAILED;
    }
    if (::fsync(state_->file.get()) != 0) {
        return mapErrno(errno);
    }
    state_->file.reset();
    if (state_->overwrite) {
        if (::rename(state_->tempPath.c_str(), state_->targetPath.c_str()) != 0) {
            return mapErrno(errno);
        }
    } else {
        if (::link(state_->tempPath.c_str(), state_->targetPath.c_str()) == 0) {
            ::unlink(state_->tempPath.c_str());
        } else if (errno == EPERM || errno == EOPNOTSUPP) {
            struct stat targetStat{};
            if (::lstat(state_->targetPath.c_str(), &targetStat) == 0) {
                return wire::FILE_ERROR_ALREADY_EXISTS;
            }
            if (::rename(state_->tempPath.c_str(), state_->targetPath.c_str()) != 0) {
                return mapErrno(errno);
            }
        } else {
            return mapErrno(errno);
        }
    }
    state_->committed = true;
    return wire::FILE_ERROR_NONE;
}
} // namespace ultraterimal::agent
