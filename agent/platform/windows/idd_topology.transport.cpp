#include "idd_topology.transport.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <vector>

#include <windows.h>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include "idd/idd_shared.protocol.h"

namespace ultraterimal::agent {
namespace {

using Microsoft::WRL::ComPtr;

bool containsIgnoreCase(std::wstring_view haystack, std::wstring_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != haystack.end();
}

std::wstring queryDisplayAdapterName(const LUID& adapterId) {
    DISPLAYCONFIG_ADAPTER_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = adapterId;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
        return {};
    }
    return name.adapterDevicePath;
}

std::wstring queryDisplaySourceGdiName(const LUID& adapterId, std::uint32_t sourceId) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = adapterId;
    name.header.id = sourceId;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
        return {};
    }
    return name.viewGdiDeviceName;
}

std::wstring queryDisplayDeviceString(std::wstring_view gdiDeviceName) {
    if (gdiDeviceName.empty()) {
        return {};
    }
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (gdiDeviceName == device.DeviceName) {
            return device.DeviceString;
        }
        device.cb = sizeof(device);
    }
    return {};
}

bool adapterIsHardwareGpu(const LUID& adapterId, std::wstring_view gdiDeviceName) {
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) != S_OK) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                desc.AdapterLuid.LowPart == adapterId.LowPart &&
                desc.AdapterLuid.HighPart == adapterId.HighPart) {
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    return false;
                }
                if (isUltraterimalIddAdapter(desc.Description)) {
                    return false;
                }
                return !isSoftwareDisplayAdapter(desc.Description, desc.VendorId);
            }
        }
    }
    const auto deviceString = queryDisplayDeviceString(gdiDeviceName);
    if (deviceString.empty() || isUltraterimalIddAdapter(deviceString)) {
        return false;
    }
    return !isSoftwareDisplayAdapter(deviceString, 0);
}

bool pathIsUltraterimalIdd(const DISPLAYCONFIG_PATH_INFO& path) {
    const auto gdiName = queryDisplaySourceGdiName(path.sourceInfo.adapterId, path.sourceInfo.id);
    if (!gdiName.empty() && isDisplayUltraterimalIdd(gdiName)) {
        return true;
    }
    const auto adapterPath = queryDisplayAdapterName(path.sourceInfo.adapterId);
    return containsIgnoreCase(adapterPath, L"ultraterimalidd");
}

} // namespace

bool isUltraterimalIddAdapter(std::wstring_view description) noexcept {
    return containsIgnoreCase(description, idd::kAdapterFriendlyName) ||
           containsIgnoreCase(description, L"UltraterimalIdd");
}

bool isSoftwareDisplayAdapter(std::wstring_view description, std::uint32_t vendorId) noexcept {
    if (isUltraterimalIddAdapter(description)) {
        return false;
    }
    // Known software / virtual GPU PCI vendor IDs:
    // 0x1414 Microsoft (Basic Display / WARP / Hyper-V)
    // 0x1AF4 Red Hat VirtIO
    // 0x1B36 Red Hat QEMU/QXL
    // 0x1234 QEMU/Bochs stdvga
    // 0x15AD VMware
    // 0x80EE VirtualBox
    // 0x1AB8 Parallels
    if (vendorId == 0x1414u || vendorId == 0x1AF4u || vendorId == 0x1B36u ||
        vendorId == 0x1234u || vendorId == 0x15ADu || vendorId == 0x80EEu || vendorId == 0x1AB8u) {
        return true;
    }
    constexpr std::array<std::wstring_view, 11> kSoftwareKeywords = {
        L"Microsoft Basic Display",
        L"Microsoft Basic Render",
        L"Microsoft Hyper-V Video",
        L"Microsoft Remote Display",
        L"VirtIO",
        L"QXL",
        L"Bochs",
        L"Standard VGA",
        L"VMware SVGA",
        L"VirtualBox",
        L"Parallels",
    };
    for (const auto keyword : kSoftwareKeywords) {
        if (containsIgnoreCase(description, keyword)) {
            return true;
        }
    }
    return false;
}

bool isDisplayUltraterimalIdd(std::wstring_view displayDeviceName) noexcept {
    const auto deviceString = queryDisplayDeviceString(displayDeviceName);
    return isUltraterimalIddAdapter(deviceString);
}

bool ensureIddPrimaryIfSoftwareDisplay() noexcept {
    try {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0) {
            return false;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
            return false;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);

        bool hasHardwareActive = false;
        std::size_t activePathCount = 0;
        const DISPLAYCONFIG_PATH_INFO* iddCandidate = nullptr;
        bool iddAlreadyActive = false;
        UINT32 preferredWidth = 1920;
        UINT32 preferredHeight = 1080;

        for (const auto& path : paths) {
            const bool isActive = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
            const bool isIdd = pathIsUltraterimalIdd(path);
            if (isIdd) {
                if (!iddCandidate || isActive) {
                    iddCandidate = &path;
                }
                if (isActive) {
                    iddAlreadyActive = true;
                }
            }
            if (isActive) {
                ++activePathCount;
                if (!isIdd) {
                    const auto gdiName = queryDisplaySourceGdiName(path.sourceInfo.adapterId, path.sourceInfo.id);
                    if (adapterIsHardwareGpu(path.sourceInfo.adapterId, gdiName)) {
                        hasHardwareActive = true;
                    }
                    if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                        path.sourceInfo.modeInfoIdx < modes.size() &&
                        modes[path.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                        const auto& src = modes[path.sourceInfo.modeInfoIdx].sourceMode;
                        if (src.width >= 1024 && src.width <= 3840 && src.height >= 720 && src.height <= 2160) {
                            preferredWidth = std::max(preferredWidth, src.width);
                            preferredHeight = std::max(preferredHeight, src.height);
                        }
                    }
                }
            }
        }

        if (!iddCandidate || hasHardwareActive) {
            return false;
        }
        if (iddAlreadyActive && activePathCount == 1) {
            return true;
        }

        DISPLAYCONFIG_PATH_INFO newPath = *iddCandidate;
        newPath.flags = DISPLAYCONFIG_PATH_ACTIVE;
        newPath.sourceInfo.modeInfoIdx = 0;
        newPath.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        newPath.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
        newPath.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
        newPath.targetInfo.refreshRate.Numerator = 60;
        newPath.targetInfo.refreshRate.Denominator = 1;
        newPath.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

        DISPLAYCONFIG_MODE_INFO newSourceMode{};
        newSourceMode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        newSourceMode.id = newPath.sourceInfo.id;
        newSourceMode.adapterId = newPath.sourceInfo.adapterId;
        newSourceMode.sourceMode.width = preferredWidth;
        newSourceMode.sourceMode.height = preferredHeight;
        newSourceMode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
        newSourceMode.sourceMode.position.x = 0;
        newSourceMode.sourceMode.position.y = 0;

        const LONG status = SetDisplayConfig(
            1, &newPath, 1, &newSourceMode,
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        return status == ERROR_SUCCESS;
    } catch (...) {
        return false;
    }
}

} // namespace ultraterimal::agent
