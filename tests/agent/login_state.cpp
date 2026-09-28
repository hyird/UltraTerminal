#include <iostream>
#include <string_view>

#include <windows.h>

#include <wtsapi32.h>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--lock") {
        DWORD current = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &current) || !current || !LockWorkStation()) {
            std::cerr << "Cannot request lock from this interactive session\n";
            return 1;
        }
        return 0;
    }
    if (argc != 1) {
        return 2;
    }
    const auto session = WTSGetActiveConsoleSessionId();
    LPWSTR raw = nullptr;
    DWORD size = 0;
    if (session == 0xffffffff || !WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &raw, &size)) {
        std::cerr << "Cannot query Windows console state\n";
        return 1;
    }
    const auto* info = reinterpret_cast<const WTSINFOEXW*>(raw);
    if (size < sizeof(WTSINFOEXW) || info->Level != 1) {
        WTSFreeMemory(raw);
        return 1;
    }
    const auto& value = info->Data.WTSInfoExLevel1;
    std::cout << "{\"session\":" << session << ",\"locked\":" << (value.SessionFlags == WTS_SESSIONSTATE_LOCK ? "true" : "false")
              << ",\"flags\":" << value.SessionFlags << ",\"user_present\":" << (value.UserName[0] ? "true" : "false") << "}\n";
    WTSFreeMemory(raw);
}
