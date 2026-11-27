#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "ultraterimal/protocol/file.protocol.h"

namespace ultraterimal::agent {
wire::FileListResult listWindowsFiles(const wire::FileListRequest& request);
wire::FileStatResult statWindowsFile(const wire::FileStatRequest& request);

// Streams a single Windows file or an on-the-fly ZIP64 archive of a directory
// without staging a temporary archive on disk. Any locked or unreadable file
// immediately aborts the stream with an explicit FileErrorCode.
class FileDownloadStream {
  public:
    static std::expected<std::unique_ptr<FileDownloadStream>, wire::FileErrorCode> open(std::string_view utf8Path);
    ~FileDownloadStream();
    FileDownloadStream(const FileDownloadStream&) = delete;
    FileDownloadStream& operator=(const FileDownloadStream&) = delete;

    const std::string& filename() const noexcept;
    std::uint64_t declaredSize() const noexcept;
    bool isArchiveZip() const noexcept;
    std::uint64_t bytesProduced() const noexcept;

    // Returns an empty string on clean EOF.
    std::expected<std::string, wire::FileErrorCode> readChunk(std::size_t maxBytes);

  private:
    struct State;
    explicit FileDownloadStream(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// Stages uploaded bytes in a sibling temporary file and atomically moves it
// into place on commit(). Uncommitted files are deleted automatically.
class FileUploadSink {
  public:
    static std::expected<std::unique_ptr<FileUploadSink>, wire::FileErrorCode> open(std::string_view utf8Path, bool overwrite);
    ~FileUploadSink();
    FileUploadSink(const FileUploadSink&) = delete;
    FileUploadSink& operator=(const FileUploadSink&) = delete;

    const std::string& filename() const noexcept;
    std::uint64_t bytesWritten() const noexcept;

    wire::FileErrorCode writeChunk(std::string_view data);
    wire::FileErrorCode commit();

  private:
    struct State;
    explicit FileUploadSink(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
