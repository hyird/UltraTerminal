#pragma once
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ultraterimal::agent {
std::uint32_t currentDesktopSession();
// Session 0 LocalSystem duplicates its own token into the current console
// session, including before user logon. Interactive development runs select
// only their own logon. pipeSid() is SYSTEM for the service or the exact logon
// SID for interactive runs.
class DesktopIdentity {
  public:
    explicit DesktopIdentity(std::uint32_t windowsSession);
    ~DesktopIdentity();
    DesktopIdentity(const DesktopIdentity&) = delete;
    DesktopIdentity& operator=(const DesktopIdentity&) = delete;
    const std::wstring& pipeSid() const;
    std::uint32_t session() const;

  private:
    friend class DesktopProcess;
    struct State;
    std::unique_ptr<State> state_;
};
// Owns a suspended-then-contained desktop child. No inherited handles, console,
// credentials or caller-provided arguments. Launch is a blocking Windows API;
// production callers dispatch construction through their bounded blocking pool.
// On shutdown close IPC, await exit, and terminate if the owner's grace expires.
// Destruction kills the contained process tree as a last-resort cleanup.
class DesktopProcess {
  public:
    DesktopProcess(asio::any_io_executor executor, const DesktopIdentity& user,
                   const std::filesystem::path& executable, const std::wstring& pipeName);
    ~DesktopProcess();
    DesktopProcess(const DesktopProcess&) = delete;
    DesktopProcess& operator=(const DesktopProcess&) = delete;
    std::uint32_t id() const;
    asio::awaitable<std::uint32_t> wait();
    void terminate(); // Owning event loop; still await wait() afterwards.
  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
