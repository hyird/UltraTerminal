#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <windows.h>

#include "platform/windows/file.transport.h"

namespace {
std::string toUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}
} // namespace

int main() {
    using namespace ultraterimal::agent;
    using namespace ultraterimal::wire;

    const auto root = std::filesystem::temp_directory_path() / ("ultraterimal_file_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / L"sub" / L"empty_dir");

    {
        std::ofstream f1(root / L"hello.txt", std::ios::binary);
        f1 << "hello ultraterimal";
        std::ofstream f2(root / L"sub" / L"中文.txt", std::ios::binary);
        f2 << "utf8-content-1234567890";
    }

    // 1. Drive listing & directory listing
    FileListRequest driveReq;
    driveReq.set_request_id(1);
    const auto drives = listWindowsFiles(driveReq);
    assert(drives.success());
    assert(drives.entries_size() > 0);

    FileListRequest dirReq;
    dirReq.set_request_id(2);
    dirReq.set_path(toUtf8(root));
    const auto listed = listWindowsFiles(dirReq);
    assert(listed.success());
    assert(listed.entries_size() == 2);
    assert(listed.entries(0).directory());
    assert(listed.entries(0).name() == "sub");
    assert(!listed.entries(1).directory());
    assert(listed.entries(1).name() == "hello.txt");

    // 2. Stat existing and missing files
    FileStatRequest statReq;
    statReq.set_request_id(3);
    statReq.set_path(toUtf8(root / L"hello.txt"));
    const auto st = statWindowsFile(statReq);
    assert(st.success() && st.exists() && !st.directory() && st.size() == 18);

    // 3. Streaming ZIP64 archive download of directory
    auto zipStream = FileDownloadStream::open(toUtf8(root));
    assert(zipStream.has_value());
    assert((*zipStream)->isArchiveZip());
    assert((*zipStream)->filename().ends_with(".zip"));
    std::string zipBytes;
    while (true) {
        auto chunk = (*zipStream)->readChunk(1024);
        assert(chunk.has_value());
        if (chunk->empty()) {
            break;
        }
        zipBytes.append(*chunk);
    }
    assert(zipBytes.size() > 100);
    // ZIP Local File Header magic "PK\x03\x04" and EOCD magic "PK\x05\x06"
    assert(zipBytes[0] == 'P' && zipBytes[1] == 'K' && zipBytes[2] == 3 && zipBytes[3] == 4);
    assert(zipBytes.find("PK\x05\x06") != std::string::npos);

    // 4. Locked file inside directory must immediately fail archiving
    const auto lockedPath = root / L"sub" / L"locked.bin";
    HANDLE lockedHandle = CreateFileW(lockedPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lockedHandle != INVALID_HANDLE_VALUE);
    auto lockedStream = FileDownloadStream::open(toUtf8(root));
    assert(lockedStream.has_value());
    FileErrorCode lockedError = FILE_ERROR_NONE;
    while (true) {
        auto chunk = (*lockedStream)->readChunk(4096);
        if (!chunk) {
            lockedError = chunk.error();
            break;
        }
        if (chunk->empty()) {
            break;
        }
    }
    CloseHandle(lockedHandle);
    assert(lockedError == FILE_ERROR_FILE_LOCKED);

    // 5. Upload overwrite=false rejects existing file, overwrite=true replaces atomically
    auto rejected = FileUploadSink::open(toUtf8(root / L"hello.txt"), false);
    assert(!rejected.has_value());
    assert(rejected.error() == FILE_ERROR_ALREADY_EXISTS);

    auto sink = FileUploadSink::open(toUtf8(root / L"hello.txt"), true);
    assert(sink.has_value());
    assert((*sink)->writeChunk("updated-body") == FILE_ERROR_NONE);
    assert((*sink)->commit() == FILE_ERROR_NONE);

    std::ifstream verify(root / L"hello.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(verify)), std::istreambuf_iterator<char>());
    assert(content == "updated-body");

    std::filesystem::remove_all(root, ec);
    return 0;
}
