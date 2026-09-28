#include <iostream>
#include <stdexcept>

#include <wrl/client.h>

#include "platform/windows/login_credential.transport.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
struct Module {
    HMODULE value;
    ~Module() {
        if (value) {
            FreeLibrary(value);
        }
    }
};
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "Expected DLL path");
        Module module{LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
        require(module.value != nullptr, "Cannot load provider DLL");
        using GetClass = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        using CanUnload = HRESULT(WINAPI*)();
        const auto getClass = reinterpret_cast<GetClass>(GetProcAddress(module.value, "DllGetClassObject"));
        const auto canUnload = reinterpret_cast<CanUnload>(GetProcAddress(module.value, "DllCanUnloadNow"));
        require(getClass && canUnload && canUnload() == S_OK, "Missing COM exports or initial reference leak");
        void* invalid = nullptr;
        require(getClass(CLSID_NULL, __uuidof(IClassFactory), &invalid) == CLASS_E_CLASSNOTAVAILABLE && !invalid, "Unknown class accepted");
        Microsoft::WRL::ComPtr<IClassFactory> factory;
        require(getClass(ultraterimal::agent::kWindowsLoginProviderId, IID_PPV_ARGS(&factory)) == S_OK && factory, "Cannot get provider class factory");
        require(canUnload() == S_FALSE, "Live class factory can unload");
        require(factory->CreateInstance(factory.Get(), __uuidof(ICredentialProvider), &invalid) == CLASS_E_NOAGGREGATION && !invalid, "Aggregation accepted");
        Microsoft::WRL::ComPtr<ICredentialProvider> provider;
        require(factory->CreateInstance(nullptr, IID_PPV_ARGS(&provider)) == S_OK, "Cannot instantiate provider");
        require(provider->SetUsageScenario(CPUS_LOGON, 0) == S_OK, "Provider initialization failed");
        DWORD count = 1, selected = 0;
        BOOL automatic = TRUE;
        require(provider->GetCredentialCount(&count, &selected, &automatic) == S_OK && count == 0 && automatic == FALSE,
                "Provider without authorization exposed credentials");
        require(factory->LockServer(FALSE) == E_UNEXPECTED, "Unbalanced unlock accepted");
        require(factory->LockServer(TRUE) == S_OK, "Server lock failed");
        factory.Reset();
        require(canUnload() == S_FALSE, "Provider or server lock did not retain module");
        provider.Reset();
        require(canUnload() == S_FALSE, "Server lock lost when objects released");
        require(getClass(ultraterimal::agent::kWindowsLoginProviderId, IID_PPV_ARGS(&factory)) == S_OK, "Cannot reacquire factory");
        require(factory->LockServer(FALSE) == S_OK, "Server unlock failed");
        factory.Reset();
        require(canUnload() == S_OK, "COM objects or locks leaked");
        std::cout << "Provider DLL exports, factory, activation, lock balance and unload passed; no registry changes\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
