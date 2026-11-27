#include "platform/windows/file.transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>
#include <zlib.h>

#include <windows.h>

#include "platform/windows/handle.types.h"

namespace ultraterimal::agent {
namespace {
std::atomic<std::uint64_t> nextUploadNonce{1};

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), needed) != needed) {
        return {};
    }
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), needed, nullptr, nullptr) != needed) {
        return {};
    }
    return result;
}

wire::FileErrorCode mapWin32Error(DWORD error) {
    switch (error) {
    case ERROR_SUCCESS:
        return wire::FILE_ERROR_NONE;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return wire::FILE_ERROR_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return wire::FILE_ERROR_PERMISSION_DENIED;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return wire::FILE_ERROR_FILE_LOCKED;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return wire::FILE_ERROR_ALREADY_EXISTS;
    case ERROR_DIRECTORY:
        return wire::FILE_ERROR_NOT_DIRECTORY;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
        return wire::FILE_ERROR_INVALID_PATH;
    default:
        return wire::FILE_ERROR_IO_FAILED;
    }
}

std::int64_t fileTimeToUnixSeconds(const FILETIME& ft) {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kEpochDiff = 116444736000000000ULL;
    if (value.QuadPart < kEpochDiff) {
        return 0;
    }
    return static_cast<std::int64_t>((value.QuadPart - kEpochDiff) / 10000000ULL);
}

void fileTimeToDosDateTime(const FILETIME& ft, std::uint16_t& dosTime, std::uint16_t& dosDate) {
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) {
        dosTime = 0;
        dosDate = (1 << 5) | 1;
        return;
    }
    const std::uint16_t year = st.wYear < 1980 ? 0 : static_cast<std::uint16_t>(std::min<WORD>(st.wYear - 1980, 127));
    dosDate = static_cast<std::uint16_t>((year << 9) | (st.wMonth << 5) | st.wDay);
    dosTime = static_cast<std::uint16_t>((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
}

bool normalizeWindowsPath(std::string_view utf8Path, std::wstring& outWide, std::string& outNormalized, std::string& outParent) {
    if (!protocol::validFilePathText(utf8Path, false)) {
        return false;
    }
    std::wstring raw = utf8ToWide(utf8Path);
    if (raw.empty()) {
        return false;
    }
    std::replace(raw.begin(), raw.end(), L'/', L'\\');
    if (raw.size() == 2 && iswalpha(raw[0]) && raw[1] == L':') {
        raw.push_back(L'\\');
    }
    if (raw.size() < 3 || !iswalpha(raw[0]) || raw[1] != L':' || raw[2] != L'\\') {
        return false;
    }
    raw[0] = static_cast<wchar_t>(towupper(raw[0]));
    std::filesystem::path normalized = std::filesystem::path(raw).lexically_normal();
    std::wstring clean = normalized.wstring();
    if (clean.size() == 2 && clean[1] == L':') {
        clean.push_back(L'\\');
    }
    while (clean.size() > 3 && clean.back() == L'\\') {
        clean.pop_back();
    }
    if (clean.size() < 3 || !iswalpha(clean[0]) || clean[1] != L':' || clean[2] != L'\\') {
        return false;
    }
    outWide = clean;
    outNormalized = wideToUtf8(clean);
    if (clean.size() == 3) {
        outParent.clear();
    } else {
        auto parent = std::filesystem::path(clean).parent_path().wstring();
        if (parent.size() == 2 && parent[1] == L':') {
            parent.push_back(L'\\');
        }
        outParent = wideToUtf8(parent);
    }
    return !outNormalized.empty();
}

std::wstring extendedPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\") || path.size() < 240) {
        return path;
    }
    return L"\\\\?\\" + path;
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
        "msi",
        "cab",
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

struct ScopedFindHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~ScopedFindHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            FindClose(handle);
        }
    }
};

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
    std::wstring fullPath;
    std::string archiveName;
    bool directory = false;
    std::uint32_t depth = 0;
    FILETIME modified{};
};
} // namespace

wire::FileListResult listWindowsFiles(const wire::FileListRequest& request) {
    wire::FileListResult result;
    result.set_request_id(request.request_id());
    if (request.path().empty()) {
        const DWORD mask = GetLogicalDrives();
        result.set_success(true);
        result.set_error(wire::FILE_ERROR_NONE);
        for (int i = 0; i < 26; ++i) {
            if ((mask & (1u << i)) == 0) {
                continue;
            }
            wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
            const UINT type = GetDriveTypeW(root);
            if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) {
                continue;
            }
            auto* entry = result.add_entries();
            entry->set_name(std::string{static_cast<char>('A' + i), ':', '\\'});
            entry->set_directory(true);
            entry->set_size(0);
            entry->set_modified_unix_seconds(0);
        }
        return result;
    }

    std::wstring widePath;
    std::string normalized;
    std::string parent;
    if (!normalizeWindowsPath(request.path(), widePath, normalized, parent)) {
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_INVALID_PATH);
        return result;
    }
    const DWORD attrs = GetFileAttributesW(extendedPath(widePath).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        result.set_success(false);
        result.set_error(mapWin32Error(GetLastError()));
        return result;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_NOT_DIRECTORY);
        return result;
    }

    std::wstring pattern = extendedPath(widePath);
    if (!pattern.empty() && pattern.back() != L'\\') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    WIN32_FIND_DATAW data{};
    ScopedFindHandle finder{FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH)};
    if (finder.handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            result.set_success(false);
            result.set_error(mapWin32Error(error));
            return result;
        }
    }

    struct RawEntry {
        std::string name;
        std::string sortKey;
        bool directory = false;
        std::uint64_t size = 0;
        std::int64_t modified = 0;
    };
    std::vector<RawEntry> entries;
    if (finder.handle != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring_view nameView(data.cFileName);
            if (nameView == L"." || nameView == L"..") {
                continue;
            }
            std::string utf8Name = wideToUtf8(nameView);
            if (utf8Name.empty() || utf8Name.size() > 512) {
                continue;
            }
            RawEntry entry;
            entry.name = std::move(utf8Name);
            entry.sortKey = entry.name;
            std::transform(entry.sortKey.begin(), entry.sortKey.end(), entry.sortKey.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!entry.directory) {
                ULARGE_INTEGER size{};
                size.LowPart = data.nFileSizeLow;
                size.HighPart = data.nFileSizeHigh;
                entry.size = size.QuadPart;
            }
            entry.modified = fileTimeToUnixSeconds(data.ftLastWriteTime);
            entries.push_back(std::move(entry));
        } while (FindNextFileW(finder.handle, &data));
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

wire::FileStatResult statWindowsFile(const wire::FileStatRequest& request) {
    wire::FileStatResult result;
    result.set_request_id(request.request_id());
    std::wstring widePath;
    std::string normalized;
    std::string parent;
    if (!normalizeWindowsPath(request.path(), widePath, normalized, parent)) {
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_INVALID_PATH);
        return result;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(extendedPath(widePath).c_str(), GetFileExInfoStandard, &info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            result.set_success(true);
            result.set_error(wire::FILE_ERROR_NONE);
            result.set_exists(false);
            result.set_name(wideToUtf8(std::filesystem::path(widePath).filename().wstring()));
            return result;
        }
        result.set_success(false);
        result.set_error(mapWin32Error(error));
        return result;
    }
    result.set_success(true);
    result.set_error(wire::FILE_ERROR_NONE);
    result.set_exists(true);
    const bool isDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.set_directory(isDir);
    if (!isDir) {
        ULARGE_INTEGER size{};
        size.LowPart = info.nFileSizeLow;
        size.HighPart = info.nFileSizeHigh;
        result.set_size(size.QuadPart);
    }
    std::string leaf = wideToUtf8(std::filesystem::path(widePath).filename().wstring());
    if (leaf.empty()) {
        leaf = normalized;
    }
    result.set_name(std::move(leaf));
    return result;
}

struct FileDownloadStream::State {
    std::string filename;
    std::uint64_t declaredSize = 0;
    bool archiveZip = false;
    std::uint64_t bytesProduced = 0;
    bool finished = false;

    // Single-file mode
    UniqueHandle singleFile;

    // Streaming ZIP64 mode
    std::deque<PendingZipTask> queue;
    std::vector<ZipCentralEntry> central;
    std::string pending;
    std::size_t pendingOffset = 0;
    std::uint64_t streamOffset = 0;
    std::uint32_t totalFiles = 0;
    std::uint64_t totalUncompressed = 0;

    // Active file inside ZIP
    UniqueHandle activeFile;
    ZipCentralEntry activeEntry;
    bool activeDeflate = false;
    bool deflateInitialized = false;
    z_stream zstream{};
    bool activeFileEof = false;
    std::array<unsigned char, 32768> readBuffer{};
    std::array<unsigned char, 32768> deflateBuffer{};

    ~State() {
        if (deflateInitialized) {
            deflateEnd(&zstream);
            deflateInitialized = false;
        }
    }

    void appendOut(std::string_view bytes) {
        if (pendingOffset > 0 && pendingOffset == pending.size()) {
            pending.clear();
            pendingOffset = 0;
        }
        pending.append(bytes.data(), bytes.size());
        streamOffset += bytes.size();
    }

    void appendLocalHeader(const ZipCentralEntry& entry) {
        std::string header;
        header.reserve(30 + entry.name.size());
        appendLe32(header, 0x04034b50u); // Local file header signature
        appendLe16(header, 45);          // Version needed: 4.5 (ZIP64)
        appendLe16(header, entry.flags); // UTF-8 (+ Data Descriptor if file)
        appendLe16(header, entry.method);
        appendLe16(header, entry.dosTime);
        appendLe16(header, entry.dosDate);
        appendLe32(header, 0); // CRC-32 in data descriptor
        appendLe32(header, 0); // Compressed size in data descriptor
        appendLe32(header, 0); // Uncompressed size in data descriptor
        appendLe16(header, static_cast<std::uint16_t>(entry.name.size()));
        appendLe16(header, 0); // Extra field length
        header.append(entry.name);
        appendOut(header);
    }

    void appendDataDescriptor(const ZipCentralEntry& entry) {
        std::string descriptor;
        descriptor.reserve(24);
        appendLe32(descriptor, 0x08074b50u);
        appendLe32(descriptor, entry.crc);
        appendLe64(descriptor, entry.compressedSize);
        appendLe64(descriptor, entry.uncompressedSize);
        appendOut(descriptor);
    }

    void appendCentralDirectoryAndEnd() {
        const std::uint64_t cdOffset = streamOffset;
        for (const auto& entry : central) {
            const bool needZip64 = entry.compressedSize >= 0xFFFFFFFFULL || entry.uncompressedSize >= 0xFFFFFFFFULL || entry.localHeaderOffset >= 0xFFFFFFFFULL;
            std::string extra;
            if (needZip64) {
                extra.reserve(28);
                appendLe16(extra, 0x0001);
                appendLe16(extra, 24);
                appendLe64(extra, entry.uncompressedSize);
                appendLe64(extra, entry.compressedSize);
                appendLe64(extra, entry.localHeaderOffset);
            }
            std::string record;
            record.reserve(46 + entry.name.size() + extra.size());
            appendLe32(record, 0x02014b50u);
            appendLe16(record, (3u << 8) | 45u); // Version made by: Unix + 4.5
            appendLe16(record, 45u);
            appendLe16(record, entry.flags);
            appendLe16(record, entry.method);
            appendLe16(record, entry.dosTime);
            appendLe16(record, entry.dosDate);
            appendLe32(record, entry.crc);
            appendLe32(record, needZip64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(entry.compressedSize));
            appendLe32(record, needZip64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(entry.uncompressedSize));
            appendLe16(record, static_cast<std::uint16_t>(entry.name.size()));
            appendLe16(record, static_cast<std::uint16_t>(extra.size()));
            appendLe16(record, 0); // Comment length
            appendLe16(record, 0); // Disk number start
            appendLe16(record, 0); // Internal attributes
            appendLe32(record, entry.directory ? 0x10u : 0x20u);
            appendLe32(record, needZip64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(entry.localHeaderOffset));
            record.append(entry.name);
            record.append(extra);
            appendOut(record);
        }
        const std::uint64_t cdSize = streamOffset - cdOffset;
        const std::uint64_t entryCount = central.size();
        const std::uint64_t zip64EocdOffset = streamOffset;

        // ZIP64 End of Central Directory Record
        std::string z64;
        z64.reserve(56 + 20 + 22);
        appendLe32(z64, 0x06064b50u);
        appendLe64(z64, 44); // Remaining size of ZIP64 EOCD
        appendLe16(z64, 45);
        appendLe16(z64, 45);
        appendLe32(z64, 0);
        appendLe32(z64, 0);
        appendLe64(z64, entryCount);
        appendLe64(z64, entryCount);
        appendLe64(z64, cdSize);
        appendLe64(z64, cdOffset);

        // ZIP64 End of Central Directory Locator
        appendLe32(z64, 0x07064b50u);
        appendLe32(z64, 0);
        appendLe64(z64, zip64EocdOffset);
        appendLe32(z64, 1);

        // Classic End of Central Directory Record
        appendLe32(z64, 0x06054b50u);
        appendLe16(z64, 0);
        appendLe16(z64, 0);
        appendLe16(z64, entryCount >= 0xFFFFu ? 0xFFFFu : static_cast<std::uint16_t>(entryCount));
        appendLe16(z64, entryCount >= 0xFFFFu ? 0xFFFFu : static_cast<std::uint16_t>(entryCount));
        appendLe32(z64, cdSize >= 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(cdSize));
        appendLe32(z64, cdOffset >= 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(cdOffset));
        appendLe16(z64, 0);
        appendOut(z64);
    }

    wire::FileErrorCode enqueueDirectoryChildren(const PendingZipTask& dirTask) {
        if (dirTask.depth >= protocol::kMaximumArchiveDepth) {
            return wire::FILE_ERROR_LIMIT_EXCEEDED;
        }
        std::wstring pattern = extendedPath(dirTask.fullPath);
        if (!pattern.empty() && pattern.back() != L'\\') {
            pattern.push_back(L'\\');
        }
        pattern.push_back(L'*');

        WIN32_FIND_DATAW data{};
        ScopedFindHandle finder{FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH)};
        if (finder.handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) {
                return wire::FILE_ERROR_NONE;
            }
            return mapWin32Error(error);
        }

        std::vector<PendingZipTask> children;
        do {
            const std::wstring_view nameView(data.cFileName);
            if (nameView == L"." || nameView == L"..") {
                continue;
            }
            // Skip reparse points / symlinks / junctions to prevent directory cycles.
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                continue;
            }
            std::string utf8Name = wideToUtf8(nameView);
            if (utf8Name.empty()) {
                continue;
            }
            PendingZipTask child;
            child.fullPath = dirTask.fullPath;
            if (!child.fullPath.empty() && child.fullPath.back() != L'\\') {
                child.fullPath.push_back(L'\\');
            }
            child.fullPath.append(nameView);
            child.archiveName = dirTask.archiveName.empty() ? utf8Name : (dirTask.archiveName + "/" + utf8Name);
            child.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            child.depth = dirTask.depth + 1;
            child.modified = data.ftLastWriteTime;
            children.push_back(std::move(child));
        } while (FindNextFileW(finder.handle, &data));

        std::sort(children.begin(), children.end(), [](const PendingZipTask& a, const PendingZipTask& b) {
            return a.archiveName < b.archiveName;
        });

        // Push in reverse so front of deque preserves alphabetical order depth-first.
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            queue.push_front(std::move(*it));
        }
        return wire::FILE_ERROR_NONE;
    }

    wire::FileErrorCode advanceZip(std::size_t targetBytes) {
        while ((pending.size() - pendingOffset) < targetBytes && !finished) {
            if (activeFile) {
                if (!activeFileEof) {
                    DWORD readBytes = 0;
                    if (!ReadFile(activeFile.get(), readBuffer.data(), static_cast<DWORD>(readBuffer.size()), &readBytes, nullptr)) {
                        return mapWin32Error(GetLastError());
                    }
                    if (readBytes == 0) {
                        activeFileEof = true;
                    } else {
                        if (totalUncompressed + readBytes > protocol::kMaximumArchiveBytes) {
                            return wire::FILE_ERROR_LIMIT_EXCEEDED;
                        }
                        totalUncompressed += readBytes;
                        activeEntry.uncompressedSize += readBytes;
                        activeEntry.crc = static_cast<std::uint32_t>(crc32(activeEntry.crc, readBuffer.data(), readBytes));
                        if (!activeDeflate) {
                            activeEntry.compressedSize += readBytes;
                            appendOut(std::string_view(reinterpret_cast<const char*>(readBuffer.data()), readBytes));
                        } else {
                            zstream.next_in = readBuffer.data();
                            zstream.avail_in = readBytes;
                            while (zstream.avail_in > 0) {
                                zstream.next_out = deflateBuffer.data();
                                zstream.avail_out = static_cast<uInt>(deflateBuffer.size());
                                const int rc = deflate(&zstream, Z_NO_FLUSH);
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
                        while (true) {
                            zstream.next_out = deflateBuffer.data();
                            zstream.avail_out = static_cast<uInt>(deflateBuffer.size());
                            const int rc = deflate(&zstream, Z_FINISH);
                            const std::size_t produced = deflateBuffer.size() - zstream.avail_out;
                            if (produced > 0) {
                                activeEntry.compressedSize += produced;
                                appendOut(std::string_view(reinterpret_cast<const char*>(deflateBuffer.data()), produced));
                            }
                            if (rc == Z_STREAM_END) {
                                break;
                            }
                            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                                return wire::FILE_ERROR_IO_FAILED;
                            }
                        }
                        deflateEnd(&zstream);
                        deflateInitialized = false;
                    }
                    activeFile.reset();
                    appendDataDescriptor(activeEntry);
                    central.push_back(std::move(activeEntry));
                }
                continue;
            }

            if (queue.empty()) {
                appendCentralDirectoryAndEnd();
                finished = true;
                break;
            }

            PendingZipTask task = std::move(queue.front());
            queue.pop_front();
            if (++totalFiles > protocol::kMaximumArchiveFiles) {
                return wire::FILE_ERROR_LIMIT_EXCEEDED;
            }

            if (task.directory) {
                const auto prevSize = queue.size();
                const auto err = enqueueDirectoryChildren(task);
                if (err != wire::FILE_ERROR_NONE) {
                    return err;
                }
                // Write explicit directory entry (preserves empty directories too).
                if (!task.archiveName.empty()) {
                    ZipCentralEntry dirEntry;
                    dirEntry.name = task.archiveName + "/";
                    dirEntry.method = 0;
                    dirEntry.flags = 0x0800u; // UTF-8
                    fileTimeToDosDateTime(task.modified, dirEntry.dosTime, dirEntry.dosDate);
                    dirEntry.localHeaderOffset = streamOffset;
                    dirEntry.directory = true;
                    appendLocalHeader(dirEntry);
                    central.push_back(std::move(dirEntry));
                }
                (void)prevSize;
                continue;
            }

            // Open file strictly; locked or unreadable files immediately abort archiving.
            HANDLE raw = CreateFileW(extendedPath(task.fullPath).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (raw == INVALID_HANDLE_VALUE) {
                return mapWin32Error(GetLastError());
            }
            activeFile.reset(raw);
            activeFileEof = false;
            activeEntry = ZipCentralEntry{};
            activeEntry.name = std::move(task.archiveName);
            activeDeflate = !shouldStoreUncompressed(activeEntry.name);
            activeEntry.method = activeDeflate ? 8u : 0u;
            activeEntry.flags = 0x0808u; // UTF-8 + Data Descriptor
            fileTimeToDosDateTime(task.modified, activeEntry.dosTime, activeEntry.dosDate);
            activeEntry.crc = static_cast<std::uint32_t>(crc32(0L, Z_NULL, 0));
            activeEntry.localHeaderOffset = streamOffset;
            activeEntry.directory = false;

            if (activeDeflate) {
                std::memset(&zstream, 0, sizeof(zstream));
                if (deflateInit2(&zstream, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                    activeFile.reset();
                    return wire::FILE_ERROR_IO_FAILED;
                }
                deflateInitialized = true;
            }
            appendLocalHeader(activeEntry);
        }
        return wire::FILE_ERROR_NONE;
    }
};

FileDownloadStream::FileDownloadStream(std::unique_ptr<State> state) : state_(std::move(state)) {}
FileDownloadStream::~FileDownloadStream() = default;

const std::string& FileDownloadStream::filename() const noexcept { return state_->filename; }
std::uint64_t FileDownloadStream::declaredSize() const noexcept { return state_->declaredSize; }
bool FileDownloadStream::isArchiveZip() const noexcept { return state_->archiveZip; }
std::uint64_t FileDownloadStream::bytesProduced() const noexcept { return state_->bytesProduced; }

std::expected<std::unique_ptr<FileDownloadStream>, wire::FileErrorCode> FileDownloadStream::open(std::string_view utf8Path) {
    std::wstring widePath;
    std::string normalized;
    std::string parent;
    if (!normalizeWindowsPath(utf8Path, widePath, normalized, parent)) {
        return std::unexpected(wire::FILE_ERROR_INVALID_PATH);
    }
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(extendedPath(widePath).c_str(), GetFileExInfoStandard, &attrs)) {
        return std::unexpected(mapWin32Error(GetLastError()));
    }

    auto state = std::make_unique<State>();
    std::string leaf = wideToUtf8(std::filesystem::path(widePath).filename().wstring());
    if (leaf.empty()) {
        // Drive root such as C:\ -> C
        leaf = std::string{static_cast<char>(normalized[0])};
    }

    if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        // Verify directory is accessible before committing to streaming ZIP.
        PendingZipTask rootTask;
        rootTask.fullPath = widePath;
        rootTask.archiveName = leaf;
        rootTask.directory = true;
        rootTask.depth = 0;
        rootTask.modified = attrs.ftLastWriteTime;

        const auto check = state->enqueueDirectoryChildren(rootTask);
        if (check != wire::FILE_ERROR_NONE) {
            return std::unexpected(check);
        }
        state->filename = leaf + ".zip";
        state->declaredSize = 0;
        state->archiveZip = true;
        return std::unique_ptr<FileDownloadStream>(new FileDownloadStream(std::move(state)));
    }

    HANDLE raw = CreateFileW(extendedPath(widePath).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return std::unexpected(mapWin32Error(GetLastError()));
    }
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(raw, &fileSize) || fileSize.QuadPart < 0) {
        const DWORD error = GetLastError();
        CloseHandle(raw);
        return std::unexpected(mapWin32Error(error));
    }
    state->singleFile.reset(raw);
    state->filename = std::move(leaf);
    state->declaredSize = static_cast<std::uint64_t>(fileSize.QuadPart);
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
        DWORD readBytes = 0;
        if (!ReadFile(state_->singleFile.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &readBytes, nullptr)) {
            return std::unexpected(mapWin32Error(GetLastError()));
        }
        chunk.resize(readBytes);
        if (readBytes == 0) {
            state_->finished = true;
            state_->singleFile.reset();
        } else {
            state_->bytesProduced += readBytes;
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
    std::wstring targetPath;
    std::wstring tempPath;
    std::string filename;
    bool overwrite = false;
    bool committed = false;
    std::uint64_t bytesWritten = 0;
    UniqueHandle file;

    ~State() {
        file.reset();
        if (!committed && !tempPath.empty()) {
            DeleteFileW(extendedPath(tempPath).c_str());
        }
    }
};

FileUploadSink::FileUploadSink(std::unique_ptr<State> state) : state_(std::move(state)) {}
FileUploadSink::~FileUploadSink() = default;

const std::string& FileUploadSink::filename() const noexcept { return state_->filename; }
std::uint64_t FileUploadSink::bytesWritten() const noexcept { return state_->bytesWritten; }

std::expected<std::unique_ptr<FileUploadSink>, wire::FileErrorCode> FileUploadSink::open(std::string_view utf8Path, bool overwrite) {
    std::wstring widePath;
    std::string normalized;
    std::string parent;
    if (!normalizeWindowsPath(utf8Path, widePath, normalized, parent)) {
        return std::unexpected(wire::FILE_ERROR_INVALID_PATH);
    }
    if (widePath.size() <= 3) {
        return std::unexpected(wire::FILE_ERROR_IS_DIRECTORY);
    }
    const DWORD attrs = GetFileAttributesW(extendedPath(widePath).c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return std::unexpected(wire::FILE_ERROR_IS_DIRECTORY);
        }
        if (!overwrite) {
            return std::unexpected(wire::FILE_ERROR_ALREADY_EXISTS);
        }
    }
    const auto parentPath = std::filesystem::path(widePath).parent_path().wstring();
    const DWORD parentAttrs = GetFileAttributesW(extendedPath(parentPath).c_str());
    if (parentAttrs == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(mapWin32Error(GetLastError()));
    }
    if ((parentAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return std::unexpected(wire::FILE_ERROR_NOT_DIRECTORY);
    }

    const auto nonce = nextUploadNonce.fetch_add(1, std::memory_order_relaxed);
    const std::wstring tempPath = widePath + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(nonce) + L".utpart";
    HANDLE raw = CreateFileW(extendedPath(tempPath).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return std::unexpected(mapWin32Error(GetLastError()));
    }

    auto state = std::make_unique<State>();
    state->targetPath = std::move(widePath);
    state->tempPath = tempPath;
    state->filename = wideToUtf8(std::filesystem::path(state->targetPath).filename().wstring());
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
        DWORD written = 0;
        const auto remaining = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, std::numeric_limits<DWORD>::max()));
        if (!WriteFile(state_->file.get(), data.data() + offset, remaining, &written, nullptr) || written == 0) {
            return mapWin32Error(GetLastError());
        }
        offset += written;
        state_->bytesWritten += written;
    }
    return wire::FILE_ERROR_NONE;
}

wire::FileErrorCode FileUploadSink::commit() {
    if (!state_->file) {
        return wire::FILE_ERROR_IO_FAILED;
    }
    if (!FlushFileBuffers(state_->file.get())) {
        return mapWin32Error(GetLastError());
    }
    state_->file.reset();
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (state_->overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(extendedPath(state_->tempPath).c_str(), extendedPath(state_->targetPath).c_str(), flags)) {
        return mapWin32Error(GetLastError());
    }
    state_->committed = true;
    return wire::FILE_ERROR_NONE;
}
} // namespace ultraterimal::agent
