#include "idd_topology.transport.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include "idd/idd_shared.protocol.h"

namespace ultraterimal::agent {
namespace {

struct SavedTopology {
    std::mutex mutex;
    bool valid = false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

SavedTopology& savedTopology() {
    static SavedTopology instance;
    return instance;
}

constexpr UINT32 kLeaseMagic = 0x5554544Cu; // "UTTL"
constexpr std::size_t kMaxLeaseSlots = 8;

struct SharedLeaseTable {
    std::uint32_t magic;
    std::array<std::uint32_t, kMaxLeaseSlots> pids;
};

bool isProcessAlive(DWORD pid) noexcept {
    if (pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
}

HANDLE createNamedMutexWithFallback(const wchar_t* globalName, const wchar_t* localName) noexcept {
    HANDLE handle = CreateMutexW(nullptr, FALSE, globalName);
    if (!handle && GetLastError() == ERROR_ACCESS_DENIED) {
        handle = CreateMutexW(nullptr, FALSE, localName);
    }
    return handle;
}

HANDLE createNamedMappingWithFallback(const wchar_t* globalName, const wchar_t* localName, DWORD size, bool& created) noexcept {
    created = false;
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, globalName);
    if (!handle && GetLastError() == ERROR_ACCESS_DENIED) {
        handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, localName);
    }
    if (handle && GetLastError() != ERROR_ALREADY_EXISTS) {
        created = true;
    }
    return handle;
}

bool sameLuid(const LUID& a, const LUID& b) noexcept {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

void snapshotCurrentActiveTopologyIfNeeded() noexcept {
    auto& saved = savedTopology();
    std::lock_guard lock(saved.mutex);
    if (saved.valid) {
        return;
    }
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0) {
        return;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
        return;
    }
    paths.resize(pathCount);
    modes.resize(modeCount);
    saved.paths = std::move(paths);
    saved.modes = std::move(modes);
    saved.valid = true;
}

void clearSavedTopology() noexcept {
    auto& saved = savedTopology();
    std::lock_guard lock(saved.mutex);
    saved.valid = false;
    saved.paths.clear();
    saved.modes.clear();
}

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
        UINT32 iddActiveWidth = 0;
        UINT32 iddActiveHeight = 0;
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
                    if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                        path.sourceInfo.modeInfoIdx < modes.size() &&
                        modes[path.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                        const auto& src = modes[path.sourceInfo.modeInfoIdx].sourceMode;
                        if (src.width >= idd::kMinCustomWidth && src.width <= idd::kMaxWidth &&
                            src.height >= idd::kMinCustomHeight && src.height <= idd::kMaxHeight) {
                            iddActiveWidth = src.width;
                            iddActiveHeight = src.height;
                        }
                    }
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

        const bool hadNonIddActive = activePathCount > (iddAlreadyActive ? 1u : 0u);
        if (hadNonIddActive) {
            snapshotCurrentActiveTopologyIfNeeded();
        }

        const UINT32 targetWidth = iddActiveWidth ? iddActiveWidth : preferredWidth;
        const UINT32 targetHeight = iddActiveHeight ? iddActiveHeight : preferredHeight;

        DISPLAYCONFIG_PATH_INFO newPath{};
        newPath.sourceInfo.adapterId = iddCandidate->sourceInfo.adapterId;
        newPath.sourceInfo.id = iddCandidate->sourceInfo.id;
        newPath.sourceInfo.modeInfoIdx = 0;
        newPath.targetInfo.adapterId = iddCandidate->targetInfo.adapterId;
        newPath.targetInfo.id = iddCandidate->targetInfo.id;
        newPath.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        newPath.targetInfo.outputTechnology = iddCandidate->targetInfo.outputTechnology;
        newPath.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
        newPath.targetInfo.scaling = DISPLAYCONFIG_SCALING_PREFERRED;
        newPath.targetInfo.refreshRate.Numerator = 60;
        newPath.targetInfo.refreshRate.Denominator = 1;
        newPath.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
        newPath.flags = DISPLAYCONFIG_PATH_ACTIVE;

        DISPLAYCONFIG_MODE_INFO newSourceMode{};
        newSourceMode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        newSourceMode.id = newPath.sourceInfo.id;
        newSourceMode.adapterId = newPath.sourceInfo.adapterId;
        newSourceMode.sourceMode.width = targetWidth;
        newSourceMode.sourceMode.height = targetHeight;
        newSourceMode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
        newSourceMode.sourceMode.position.x = 0;
        newSourceMode.sourceMode.position.y = 0;

        // Intentionally omit SDC_SAVE_TO_DATABASE so temporary IDD hijacking is
        // not persisted as the permanent OS display topology.
        LONG status = SetDisplayConfig(
            1, &newPath, 1, &newSourceMode,
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        if (status != ERROR_SUCCESS) {
            newPath.targetInfo.scaling = DISPLAYCONFIG_SCALING{};
            newPath.targetInfo.refreshRate.Numerator = 0;
            newPath.targetInfo.refreshRate.Denominator = 0;
            newPath.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED;
            status = SetDisplayConfig(
                1, &newPath, 1, &newSourceMode,
                SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        }
        if (status == ERROR_SUCCESS) {
            Sleep(120);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool restoreDisplayTopologyAfterIdd() noexcept {
    try {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0) {
            return false;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> allPaths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> allModes(modeCount);
        if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, allPaths.data(), &modeCount, allModes.data(), nullptr) != ERROR_SUCCESS) {
            return false;
        }
        allPaths.resize(pathCount);
        allModes.resize(modeCount);

        bool iddActive = false;
        bool hasHardwareActive = false;
        for (const auto& path : allPaths) {
            if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
                continue;
            }
            if (pathIsUltraterimalIdd(path)) {
                iddActive = true;
                continue;
            }
            const auto gdiName = queryDisplaySourceGdiName(path.sourceInfo.adapterId, path.sourceInfo.id);
            if (adapterIsHardwareGpu(path.sourceInfo.adapterId, gdiName)) {
                hasHardwareActive = true;
            }
        }
        if (!iddActive || hasHardwareActive) {
            clearSavedTopology();
            return false;
        }

        {
            auto& saved = savedTopology();
            std::lock_guard lock(saved.mutex);
            if (saved.valid && !saved.paths.empty()) {
                bool savedHasNonIdd = false;
                for (const auto& path : saved.paths) {
                    if (!pathIsUltraterimalIdd(path)) {
                        savedHasNonIdd = true;
                        break;
                    }
                }
                if (savedHasNonIdd) {
                    auto restorePaths = saved.paths;
                    auto restoreModes = saved.modes;
                    const LONG status = SetDisplayConfig(
                        static_cast<UINT32>(restorePaths.size()),
                        restorePaths.data(),
                        static_cast<UINT32>(restoreModes.size()),
                        restoreModes.data(),
                        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
                    if (status == ERROR_SUCCESS) {
                        saved.valid = false;
                        saved.paths.clear();
                        saved.modes.clear();
                        Sleep(120);
                        return true;
                    }
                }
                saved.valid = false;
                saved.paths.clear();
                saved.modes.clear();
            }
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> hardwareCandidates;
        std::vector<DISPLAYCONFIG_PATH_INFO> fallbackCandidates;
        const auto addUnique = [](std::vector<DISPLAYCONFIG_PATH_INFO>& dest, const DISPLAYCONFIG_PATH_INFO& candidate) {
            for (const auto& existing : dest) {
                if (sameLuid(existing.sourceInfo.adapterId, candidate.sourceInfo.adapterId) &&
                    existing.sourceInfo.id == candidate.sourceInfo.id) {
                    return;
                }
                if (sameLuid(existing.targetInfo.adapterId, candidate.targetInfo.adapterId) &&
                    existing.targetInfo.id == candidate.targetInfo.id) {
                    return;
                }
            }
            DISPLAYCONFIG_PATH_INFO cleaned = candidate;
            cleaned.sourceInfo.cloneGroupId = DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;
            cleaned.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            cleaned.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            cleaned.flags = DISPLAYCONFIG_PATH_ACTIVE;
            dest.push_back(cleaned);
        };

        for (const auto& path : allPaths) {
            if (!path.targetInfo.targetAvailable || pathIsUltraterimalIdd(path)) {
                continue;
            }
            const auto gdiName = queryDisplaySourceGdiName(path.sourceInfo.adapterId, path.sourceInfo.id);
            if (adapterIsHardwareGpu(path.sourceInfo.adapterId, gdiName)) {
                addUnique(hardwareCandidates, path);
            } else {
                addUnique(fallbackCandidates, path);
            }
        }

        auto& chosen = !hardwareCandidates.empty() ? hardwareCandidates : fallbackCandidates;
        if (chosen.empty()) {
            return false;
        }

        LONG status = SetDisplayConfig(
            static_cast<UINT32>(chosen.size()),
            chosen.data(),
            0,
            nullptr,
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (status != ERROR_SUCCESS && chosen.size() > 1) {
            status = SetDisplayConfig(
                1,
                chosen.data(),
                0,
                nullptr,
                SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        }
        if (status == ERROR_SUCCESS) {
            Sleep(120);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

struct DesktopDisplayTopologyLease::State {
    HANDLE lock = nullptr;
    HANDLE mapping = nullptr;
    DWORD pid = 0;
    bool acquired = false;
};

DesktopDisplayTopologyLease::DesktopDisplayTopologyLease() noexcept : state_(std::make_unique<State>()) {
    state_->pid = GetCurrentProcessId();
    state_->lock = createNamedMutexWithFallback(
        L"Global\\UltraterimalDesktopTopologyLock",
        L"Local\\UltraterimalDesktopTopologyLock");
    bool created = false;
    state_->mapping = createNamedMappingWithFallback(
        L"Global\\UltraterimalDesktopTopologyState",
        L"Local\\UltraterimalDesktopTopologyState",
        static_cast<DWORD>(sizeof(SharedLeaseTable)),
        created);
    if (!state_->lock || !state_->mapping) {
        return;
    }
    const DWORD wait = WaitForSingleObject(state_->lock, 2000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        return;
    }
    auto* table = static_cast<SharedLeaseTable*>(MapViewOfFile(state_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedLeaseTable)));
    if (table) {
        if (created || table->magic != kLeaseMagic) {
            table->magic = kLeaseMagic;
            table->pids.fill(0);
        }
        bool present = false;
        for (auto& slot : table->pids) {
            if (slot == state_->pid) {
                present = true;
            } else if (slot != 0 && !isProcessAlive(slot)) {
                slot = 0;
            }
        }
        if (!present) {
            for (auto& slot : table->pids) {
                if (slot == 0) {
                    slot = state_->pid;
                    present = true;
                    break;
                }
            }
        }
        state_->acquired = present;
        UnmapViewOfFile(table);
    }
    ReleaseMutex(state_->lock);
}

DesktopDisplayTopologyLease::~DesktopDisplayTopologyLease() {
    if (!state_) {
        return;
    }
    if (state_->acquired && state_->lock && state_->mapping) {
        const DWORD wait = WaitForSingleObject(state_->lock, 2000);
        const bool locked = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
        bool shouldRestore = false;
        if (auto* table = static_cast<SharedLeaseTable*>(MapViewOfFile(state_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedLeaseTable)))) {
            if (table->magic == kLeaseMagic) {
                std::size_t liveCount = 0;
                for (auto& slot : table->pids) {
                    if (slot == state_->pid || (slot != 0 && !isProcessAlive(slot))) {
                        slot = 0;
                    } else if (slot != 0) {
                        ++liveCount;
                    }
                }
                shouldRestore = (liveCount == 0);
            }
            UnmapViewOfFile(table);
        }
        if (shouldRestore) {
            (void)restoreDisplayTopologyAfterIdd();
        }
        if (locked) {
            ReleaseMutex(state_->lock);
        }
    }
    if (state_->mapping) {
        CloseHandle(state_->mapping);
    }
    if (state_->lock) {
        CloseHandle(state_->lock);
    }
}

} // namespace ultraterimal::agent
