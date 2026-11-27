#include <array>
#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

#include "platform/platform.transport.h"

namespace {
void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

std::uint16_t readLe16(const std::string& data, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset])) |
           (static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset + 1])) << 8);
}

std::uint32_t readLe32(const std::string& data, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

class FakeRestartPlatform final : public ultraterimal::agent::SystemRestartPlatform {
  public:
    bool eligible = true;
    std::unordered_set<std::string> seenOps;
    std::uint32_t initiateCode = 0;
    int initiateCount = 0;

    bool isEligible() noexcept override { return eligible; }
    bool persistOperation(std::string_view opId) override { return seenOps.emplace(opId).second; }
    std::uint32_t initiatePlannedRestart() noexcept override {
        ++initiateCount;
        return initiateCode;
    }
};
} // namespace

int main() {
    using namespace ultraterimal;
    const auto tempRoot = std::filesystem::temp_directory_path() / ("ultraterimal-linux-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot / "sub", ec);
    try {
        // 1. Credential save/load with 0600 permissions.
        const auto credPath = tempRoot / "device.bin";
        agent::saveCredential(credPath, "secret-token-value");
        require(agent::loadCredential(credPath) == "secret-token-value", "Credential roundtrip failed");
        struct stat st{};
        require(::stat(credPath.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "Credential mode is not 0600");

        // 2. Capability detection reflects SSH/PTY-only Linux agent.
        const auto caps = agent::detectCapabilities();
        require(caps.conpty && !caps.mediaFoundationPresent && caps.attachedDisplays == 0, "Capabilities mismatch");

        // 3. File list, stat, upload sink, and streaming ZIP64 archive (symlinks skipped).
        {
            std::ofstream out(tempRoot / "alpha.txt", std::ios::binary);
            out << "hello linux agent";
        }
        {
            std::ofstream out(tempRoot / "sub" / "nested.bin", std::ios::binary);
            out << "nested payload";
        }
        std::filesystem::create_directory_symlink(tempRoot, tempRoot / "loop_link", ec);

        wire::FileListRequest listReq;
        listReq.set_request_id(1);
        listReq.set_path("");
        const auto rootList = agent::listPlatformFiles(listReq);
        require(rootList.success() && rootList.normalized_path() == "/", "Empty path did not list /");

        listReq.set_path(tempRoot.string());
        const auto listed = agent::listPlatformFiles(listReq);
        require(listed.success() && listed.normalized_path() == tempRoot.lexically_normal().string(), "Directory list failed");
        require(listed.entries_size() >= 2, "Missing expected directory entries");

        wire::FileStatRequest statReq;
        statReq.set_request_id(2);
        statReq.set_path((tempRoot / "alpha.txt").string());
        const auto statRes = agent::statPlatformFile(statReq);
        require(statRes.success() && statRes.exists() && !statRes.directory() && statRes.size() == 17, "Stat failed");

        auto upload = agent::FileUploadSink::open((tempRoot / "uploaded.txt").string(), false);
        require(upload.has_value(), "Upload open failed");
        require((*upload)->writeChunk("part-1-") == wire::FILE_ERROR_NONE, "Write chunk 1 failed");
        require((*upload)->writeChunk("part-2") == wire::FILE_ERROR_NONE, "Write chunk 2 failed");
        require((*upload)->commit() == wire::FILE_ERROR_NONE, "Upload commit failed");

        auto duplicate = agent::FileUploadSink::open((tempRoot / "uploaded.txt").string(), false);
        require(!duplicate.has_value() && duplicate.error() == wire::FILE_ERROR_ALREADY_EXISTS, "Duplicate non-overwrite upload was not rejected");

        auto download = agent::FileDownloadStream::open(tempRoot.string());
        require(download.has_value() && (*download)->isArchiveZip(), "Directory download did not open ZIP stream");
        std::string zipBytes;
        while (true) {
            auto chunk = (*download)->readChunk(1024);
            require(chunk.has_value(), "ZIP stream readChunk failed");
            if (chunk->empty()) {
                break;
            }
            zipBytes += *chunk;
        }
        require(zipBytes.size() > 64 && readLe32(zipBytes, 0) == 0x04034b50, "ZIP local file header missing");
        require(zipBytes.find("nested.bin") != std::string::npos, "Nested file missing from ZIP");
        require(zipBytes.find("loop_link") == std::string::npos, "Symlink was not skipped during ZIP traversal");
        const auto eocdPos = zipBytes.rfind("\x50\x4b\x05\x06");
        require(eocdPos != std::string::npos && readLe16(zipBytes, eocdPos + 10) >= 3, "ZIP EOCD missing or invalid");

        // 4. Interactive PTY terminal spawn/write/read/resize/exit.
        {
            ruvia::blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 4});
            ruvia::event_loop_pool loopPool({.loop_count_ = 1, .queue_capacity_ = 16, .blocking_pool_ = &pool});
            auto loop = loopPool.next_loop();
            loopPool.start();
            auto term = agent::PlatformTerminal::spawn(loop);
            term->resize(120, 40);
            std::string ptyOut;
            std::promise<void> done;
            auto doneFuture = done.get_future();
            asio::co_spawn(
                loop.executor(),
                [&]() -> asio::awaitable<void> {
                    co_await term->write("echo LINUX_PTY_INTERACTIVE_OK; exit 0\n");
                    for (;;) {
                        auto chunk = co_await term->read();
                        if (chunk.empty()) {
                            break;
                        }
                        ptyOut += chunk;
                    }
                },
                [&](std::exception_ptr) { done.set_value(); });
            require(doneFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "PTY session timed out");
            require(term->waitExit() == 0, "PTY exit code was non-zero");
            term->close();
            loopPool.request_stop();
            loopPool.join();
            pool.join();
            require(ptyOut.find("LINUX_PTY_INTERACTIVE_OK") != std::string::npos, "PTY interactive marker missing");
        }

        // 4b. Child PTY shell unblocks SIGINT/SIGTERM even when parent daemon blocks them.
        {
            sigset_t blocked{};
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGINT);
            sigaddset(&blocked, SIGTERM);
            sigset_t previous{};
            require(::pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0, "pthread_sigmask failed");
            struct MaskGuard {
                sigset_t oldMask;
                ~MaskGuard() { (void)::pthread_sigmask(SIG_SETMASK, &oldMask, nullptr); }
            } guard{previous};

            ruvia::blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 4});
            ruvia::event_loop_pool loopPool({.loop_count_ = 1, .queue_capacity_ = 16, .blocking_pool_ = &pool});
            auto loop = loopPool.next_loop();
            loopPool.start();
            auto term = agent::PlatformTerminal::spawn(loop);
            std::string ptyOut;
            std::promise<void> done;
            auto doneFuture = done.get_future();
            asio::co_spawn(
                loop.executor(),
                [&]() -> asio::awaitable<void> {
                    co_await term->write("sleep 10 && printf 'AFTER_%s\\n' SLEEP\n");
                    asio::steady_timer timer(loop.executor());
                    timer.expires_after(std::chrono::milliseconds(200));
                    co_await timer.async_wait(asio::use_awaitable);
                    co_await term->write("\x03");
                    timer.expires_after(std::chrono::milliseconds(100));
                    co_await timer.async_wait(asio::use_awaitable);
                    co_await term->write("echo CTRL_C_WORKED; exit 0\n");
                    for (;;) {
                        auto chunk = co_await term->read();
                        if (chunk.empty()) {
                            break;
                        }
                        ptyOut += chunk;
                    }
                },
                [&](std::exception_ptr) { done.set_value(); });
            require(doneFuture.wait_for(std::chrono::seconds(8)) == std::future_status::ready, "PTY Ctrl+C test timed out");
            require(term->waitExit() == 0, "PTY Ctrl+C exit code was non-zero");
            term->close();
            loopPool.request_stop();
            loopPool.join();
            pool.join();
            require(ptyOut.find("CTRL_C_WORKED") != std::string::npos, "Ctrl+C did not interrupt foreground command");
            require(ptyOut.find("AFTER_SLEEP") == std::string::npos, "sleep command was not interrupted by Ctrl+C");
        }

        // 5. System restart policy & state checks.
        {
            const std::string deviceId = "d-" + std::string(64, 'b');
            const auto now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            FakeRestartPlatform platform;
            wire::WindowsRestartRequest req;
            req.set_device_id(deviceId);
            req.set_operation_id(std::string(64, 'a'));
            req.set_expires_at_unix_seconds(now + 300);
            const auto ok = agent::handleSystemRestart(deviceId, req, platform, now);
            require(ok.status() == wire::WindowsRestartResult::ACCEPTED && platform.initiateCount == 1,
                    "Eligible Linux restart was not accepted");

            const auto duplicate = agent::handleSystemRestart(deviceId, req, platform, now);
            require(duplicate.status() == wire::WindowsRestartResult::UNKNOWN && platform.initiateCount == 1,
                    "Duplicate restart was not reported unknown");

            platform.eligible = false;
            req.set_operation_id(std::string(64, 'c'));
            req.set_expires_at_unix_seconds(now + 300);
            const auto denied = agent::handleSystemRestart(deviceId, req, platform, now);
            require(denied.status() == wire::WindowsRestartResult::FAILED && platform.initiateCount == 1,
                    "Non-root restart was not rejected");
        }

        std::filesystem::remove_all(tempRoot, ec);
        std::cout << "Linux platform tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(tempRoot, ec);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
