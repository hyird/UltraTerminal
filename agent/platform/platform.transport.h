#pragma once
#include <asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <event_loop.h>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "ultraterimal/protocol/file.protocol.h"
#include "ultraterimal/protocol/message.protocol.h"
#include "ultraterimal/protocol/update.protocol.h"

#ifdef _WIN32
#include "platform/windows/capability.transport.h"
#include "platform/windows/credential.transport.h"
#include "platform/windows/file.transport.h"
#include "platform/windows/service.runtime.h"
#include "platform/windows/system_restart.transport.h"
#include "platform/windows/terminal.transport.h"
#include "platform/windows/update.transport.h"
#endif

namespace ultraterimal::agent {
inline std::string_view platformName() noexcept {
#ifdef _WIN32
    return "windows-x64";
#else
    return "linux-x64";
#endif
}

class PlatformTerminal {
  public:
    static std::unique_ptr<PlatformTerminal> spawn(ruvia::event_loop loop);
    virtual ~PlatformTerminal() = default;
    virtual asio::awaitable<std::string> read() = 0;
    virtual asio::awaitable<void> write(std::string_view bytes) = 0;
    virtual void resize(std::uint32_t columns, std::uint32_t rows) = 0;
    virtual std::uint32_t waitExit() = 0;
    virtual void close() noexcept = 0;
};

#ifdef _WIN32
inline wire::FileListResult listPlatformFiles(const wire::FileListRequest& request) { return listWindowsFiles(request); }
inline wire::FileStatResult statPlatformFile(const wire::FileStatRequest& request) { return statWindowsFile(request); }
inline wire::WindowsRestartResult handleSystemRestart(std::string_view deviceId, const wire::WindowsRestartRequest& request,
                                                      SystemRestartPlatform& platform, std::uint64_t nowUnixSeconds) {
    return handleWindowsRestart(deviceId, request, platform, nowUnixSeconds);
}
inline bool canManageSystemPower() noexcept { return nativeSystemRestartPlatform().isSessionZeroLocalSystem(); }
using PlatformAgentUpdate = WindowsAgentUpdate;
#else
struct Capabilities {
    bool conpty = true;
    bool mediaFoundationPresent = false;
    bool elevated = false;
    std::uint32_t processSession = 0;
    std::uint32_t activeConsoleSession = 0xffffffff;
    std::uint32_t attachedDisplays = 0;
    std::uint32_t graphicsError = 0;
};
Capabilities detectCapabilities();
std::string capabilityJson(const Capabilities& capabilities);

void saveCredential(const std::filesystem::path& path, std::string_view value);
std::string loadCredential(const std::filesystem::path& path);

wire::FileListResult listPlatformFiles(const wire::FileListRequest& request);
wire::FileStatResult statPlatformFile(const wire::FileStatRequest& request);

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
    std::expected<std::string, wire::FileErrorCode> readChunk(std::size_t maxBytes);

  private:
    struct State;
    explicit FileDownloadStream(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

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

class SystemRestartPlatform {
  public:
    virtual ~SystemRestartPlatform() = default;
    virtual bool isEligible() noexcept = 0;
    virtual bool persistOperation(std::string_view operationId) = 0;
    virtual std::uint32_t initiatePlannedRestart() noexcept = 0;
};
wire::WindowsRestartResult handleSystemRestart(std::string_view deviceId, const wire::WindowsRestartRequest& request,
                                               SystemRestartPlatform& platform, std::uint64_t nowUnixSeconds);
SystemRestartPlatform& nativeSystemRestartPlatform();
std::uint64_t systemBootTimeUnixSeconds() noexcept;
bool canManageSystemPower() noexcept;

bool launchAgentUpdate(const std::filesystem::path& executable, const wire::AgentUpdate& update);
class PlatformAgentUpdate {
  public:
    PlatformAgentUpdate();
    ~PlatformAgentUpdate();
    std::filesystem::path root() const;
    std::filesystem::path configPath() const;
    std::filesystem::path stagePath() const;
    const wire::AgentUpdate& request() const;
    int install();
    void failed(std::uint32_t code) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
std::optional<std::uint32_t> takeAgentUpdateResult(const std::filesystem::path& root);

class ConsoleSessionEvents {};
using ServiceTask = std::function<void(std::stop_token, ConsoleSessionEvents&)>;
void dispatchService(ServiceTask task);
std::filesystem::path serviceConfigPath();
std::filesystem::path agentExecutablePath();
void installService();
void uninstallService();
void startService();
void stopService();
std::string serviceStatus();

struct TerminalDiagnostic {
    std::uint32_t exitCode = 0;
    bool markerReceived = false;
};
TerminalDiagnostic checkLocalTerminal();
#endif
} // namespace ultraterimal::agent
