#pragma once
#include <evr.h>
#include <mfapi.h>
#include <mfidl.h>

#include <windows.h>

namespace ultraterimal::agent {
struct MediaRuntime {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE library = nullptr;
    decltype(&MFStartup) startup = nullptr;
    decltype(&MFShutdown) shutdown = nullptr;
    decltype(&MFTEnumEx) enumerate = nullptr;
    decltype(&MFCreateMediaType) createType = nullptr;
    decltype(&MFCreateSample) createSample = nullptr;
    decltype(&MFCreateMemoryBuffer) createBuffer = nullptr;
    decltype(&MFCreateDXGIDeviceManager) createDeviceManager = nullptr;
    decltype(&MFCreateDXGISurfaceBuffer) createSurfaceBuffer = nullptr;
    HMODULE videoSamples = nullptr;
    decltype(&MFCreateVideoSampleFromSurface) createVideoSample = nullptr;
    HRESULT status = E_NOTIMPL;
    bool started = false;

    MediaRuntime(const MediaRuntime&) = delete;
    MediaRuntime& operator=(const MediaRuntime&) = delete;

    MediaRuntime() {
        if (FAILED(com)) {
            status = com;
            return;
        }
        library = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) {
            status = HRESULT_FROM_WIN32(GetLastError());
            return;
        }
        startup = reinterpret_cast<decltype(startup)>(GetProcAddress(library, "MFStartup"));
        shutdown = reinterpret_cast<decltype(shutdown)>(GetProcAddress(library, "MFShutdown"));
        enumerate = reinterpret_cast<decltype(enumerate)>(GetProcAddress(library, "MFTEnumEx"));
        createType = reinterpret_cast<decltype(createType)>(GetProcAddress(library, "MFCreateMediaType"));
        createSample = reinterpret_cast<decltype(createSample)>(GetProcAddress(library, "MFCreateSample"));
        createBuffer = reinterpret_cast<decltype(createBuffer)>(GetProcAddress(library, "MFCreateMemoryBuffer"));
        createDeviceManager = reinterpret_cast<decltype(createDeviceManager)>(GetProcAddress(library, "MFCreateDXGIDeviceManager"));
        createSurfaceBuffer = reinterpret_cast<decltype(createSurfaceBuffer)>(GetProcAddress(library, "MFCreateDXGISurfaceBuffer"));
        if (!startup || !shutdown || !enumerate || !createType || !createSample || !createBuffer) {
            return;
        }
        status = startup(MF_VERSION, MFSTARTUP_NOSOCKET);
        started = SUCCEEDED(status);
    }
    ~MediaRuntime() {
        if (started) {
            shutdown();
        }
        if (library) {
            FreeLibrary(library);
        }
        if (videoSamples) {
            FreeLibrary(videoSamples);
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }
    bool prepareVideoSamples() {
        if (!videoSamples) {
            videoSamples = LoadLibraryExW(L"evr.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (videoSamples) {
                createVideoSample = reinterpret_cast<decltype(createVideoSample)>(GetProcAddress(videoSamples, "MFCreateVideoSampleFromSurface"));
            }
        }
        return createDeviceManager && createSurfaceBuffer && createVideoSample;
    }
};
} // namespace ultraterimal::agent
