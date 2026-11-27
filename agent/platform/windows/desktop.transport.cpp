#include "desktop.transport.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "handle.types.h"

namespace ultraterimal::agent {
namespace {
std::wstring objectName(HANDLE object) {
    wchar_t name[256]{};
    DWORD required = 0;
    if (!object || !GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &required)) {
        throw std::runtime_error("Interactive desktop is inaccessible");
    }
    return name;
}
bool systemIdentity() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        throw std::runtime_error("Cannot verify desktop privileges");
    }
    UniqueHandle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) {
        throw std::runtime_error("Invalid desktop identity");
    }
    std::vector<BYTE> bytes(size);
    if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size)) {
        throw std::runtime_error("Cannot read desktop privileges");
    }
    return IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid, WinLocalSystemSid);
}
struct ThreadDesktop {
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK attached = nullptr;
    std::uint64_t generation = 1;
    ~ThreadDesktop() {
        if (attached && SetThreadDesktop(original)) {
            CloseDesktop(attached);
        }
    }
};
} // namespace
std::uint64_t requireInputDesktop() {
    static const bool system = systemIdentity();
    thread_local ThreadDesktop context;
    struct InputDesktop {
        HDESK handle;
        ~InputDesktop() {
            if (handle) {
                CloseDesktop(handle);
            }
        }
    } input{OpenInputDesktop(0, FALSE, system ? GENERIC_ALL : DESKTOP_READOBJECTS)};
    if (!input.handle && system) {
        throw DesktopChanged("Input desktop is temporarily inaccessible");
    }
    std::wstring target, current, station;
    try {
        target = objectName(input.handle);
        current = objectName(GetThreadDesktop(GetCurrentThreadId()));
        station = objectName(GetProcessWindowStation());
    } catch (const std::runtime_error&) {
        if (system) {
            throw DesktopChanged("Interactive desktop is temporarily unavailable");
        }
        throw;
    }
    if (station != L"WinSta0") {
        throw std::runtime_error("Interactive desktop changed or locked");
    }
    if (!system && (target != L"Default" || target != current)) {
        throw std::runtime_error("Interactive desktop changed or locked");
    }
    if (system && target != L"Default" && target != L"Winlogon") {
        throw DesktopChanged("Interactive desktop is in transition");
    }
    if (system && target != current) {
        if (context.generation == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Desktop generation exhausted");
        }
        if (!SetThreadDesktop(input.handle)) {
            throw DesktopChanged("Cannot attach the input desktop");
        }
        if (context.attached) {
            CloseDesktop(context.attached);
        }
        context.attached = std::exchange(input.handle, nullptr);
        ++context.generation;
    }
    return context.generation;
}
} // namespace ultraterimal::agent
