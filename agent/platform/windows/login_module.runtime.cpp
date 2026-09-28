#include "login_module.runtime.h"

#include <new>

#include "login_provider.transport.h"

namespace ultraterimal::agent {
namespace {
class LoginClassFactory final : public IClassFactory {
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) {
            return E_POINTER;
        }
        *output = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IClassFactory)) {
            return E_NOINTERFACE;
        }
        *output = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID id, void** output) override {
        if (!output) {
            return E_POINTER;
        }
        *output = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        try {
            auto* provider = new WindowsLoginProvider;
            const auto result = provider->QueryInterface(id, output);
            provider->Release();
            return result;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL acquire) override {
        return WindowsLoginModuleReference::lock(acquire != FALSE) ? S_OK : E_UNEXPECTED;
    }

  private:
    WindowsLoginModuleReference module_;
    std::atomic<ULONG> references_{1};
};
} // namespace
} // namespace ultraterimal::agent

STDAPI DllCanUnloadNow() {
    return ultraterimal::agent::WindowsLoginModuleReference::unused() ? S_OK : S_FALSE;
}
STDAPI DllGetClassObject(REFCLSID clsid, REFIID id, void** output) {
    if (!output) {
        return E_POINTER;
    }
    *output = nullptr;
    if (clsid != ultraterimal::agent::kWindowsLoginProviderId) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) ultraterimal::agent::LoginClassFactory;
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const auto result = factory->QueryInterface(id, output);
    factory->Release();
    return result;
}
