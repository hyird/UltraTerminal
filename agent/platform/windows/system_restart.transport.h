#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
class RegistryRestartOperationStore {
  public:
    RegistryRestartOperationStore(void* rootKey, std::wstring keyPath, std::vector<std::wstring> trustedSidStrings);
    bool persistOperation(std::string_view operationId) const;

  private:
    void* rootKey_;
    std::wstring keyPath_;
    std::vector<std::wstring> trustedSidStrings_;
};

class SystemRestartPlatform {
  public:
    virtual ~SystemRestartPlatform() = default;
    virtual bool isSessionZeroLocalSystem() noexcept = 0;
    virtual bool persistOperation(std::string_view operationId) = 0;
    virtual std::uint32_t initiatePlannedRestart() noexcept = 0;
};

// A false persistence result means this operation ID has already been reserved.
wire::WindowsRestartResult handleWindowsRestart(std::string_view deviceId, const wire::WindowsRestartRequest& request,
                                                SystemRestartPlatform& platform,
                                                std::uint64_t nowUnixSeconds,
                                                std::function<std::uint64_t()> postPersistNow = {});
SystemRestartPlatform& nativeSystemRestartPlatform();
std::uint64_t systemBootTimeUnixSeconds() noexcept;
} // namespace ultraterimal::agent
