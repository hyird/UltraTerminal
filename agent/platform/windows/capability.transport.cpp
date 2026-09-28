#include "platform/windows/capability.transport.h"

#include <format>
#include <stdexcept>
#include <system_error>

#include <windows.h>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include "platform/windows/handle.types.h"

namespace ultraterimal::agent {
Capabilities detectCapabilities() {
    Capabilities result;
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    result.conpty = kernel && GetProcAddress(kernel, "CreatePseudoConsole") && GetProcAddress(kernel, "ResizePseudoConsole") && GetProcAddress(kernel, "ClosePseudoConsole");
    // Optional DLL presence is not a claim that a usable encoder exists.
    const auto media = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    result.mediaFoundationPresent = media != nullptr;
    if (media) {
        FreeLibrary(media);
    }
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "ProcessIdToSessionId");
    }
    result.processSession = session;
    result.activeConsoleSession = WTSGetActiveConsoleSessionId();
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "OpenProcessToken");
    }
    UniqueHandle token(rawToken);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetTokenInformation");
    }
    result.elevated = elevation.TokenIsElevated != 0;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    auto status = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(status)) {
        result.graphicsError = static_cast<std::uint32_t>(status);
        return result;
    }
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        status = factory->EnumAdapters1(i, &adapter);
        if (status == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(status)) {
            result.graphicsError = static_cast<std::uint32_t>(status);
            break;
        }
        for (UINT j = 0;; ++j) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            status = adapter->EnumOutputs(j, &output);
            if (status == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(status)) {
                result.graphicsError = static_cast<std::uint32_t>(status);
                break;
            }
            DXGI_OUTPUT_DESC description{};
            status = output->GetDesc(&description);
            if (FAILED(status)) {
                result.graphicsError = static_cast<std::uint32_t>(status);
                continue;
            }
            if (description.AttachedToDesktop) {
                ++result.attachedDisplays;
            }
        }
    }
    return result;
}
std::string capabilityJson(const Capabilities& c) {
    return std::format(R"({{"conpty_api":{},"media_foundation_present":{},"elevated":{},"process_session":{},"active_console_session":{},"attached_displays":{},"graphics_error":{},"gateway_connected":false}})",
                       c.conpty, c.mediaFoundationPresent, c.elevated, c.processSession,
                       c.activeConsoleSession == 0xffffffff ? "null" : std::to_string(c.activeConsoleSession), c.attachedDisplays, c.graphicsError);
}
} // namespace ultraterimal::agent
