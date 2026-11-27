// clang-format off
#define NOMINMAX
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d11_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "idd_shared.protocol.h"

using Microsoft::WRL::ComPtr;
using namespace ultraterimal::agent::idd;

namespace {

struct DisplayModeSpec {
    UINT width;
    UINT height;
    UINT vSync;
};

constexpr DisplayModeSpec kModes[] = {
    {1920, 1080, 60},
    {1280, 720, 60},
    {1280, 800, 60},
    {1366, 768, 60},
    {1440, 900, 60},
    {1600, 900, 60},
    {1680, 1050, 60},
    {1280, 1024, 60},
    {1024, 768, 60},
    {2560, 1440, 60},
    {3840, 2160, 60},
};

std::mutex gCustomModeMutex;
DisplayModeSpec gCustomMode{0, 0, 60};

std::vector<DisplayModeSpec> collectActiveModes() {
    std::vector<DisplayModeSpec> result(std::begin(kModes), std::end(kModes));
    std::lock_guard lock(gCustomModeMutex);
    if (isValidCustomMode(gCustomMode.width, gCustomMode.height)) {
        const bool exists = std::any_of(result.begin(), result.end(), [&](const DisplayModeSpec& m) {
            return m.width == gCustomMode.width && m.height == gCustomMode.height;
        });
        if (!exists) {
            result.push_back(gCustomMode);
        }
    }
    return result;
}

void fillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode, UINT width, UINT height, UINT vSync, UINT vSyncDivider) {
    mode = {};
    mode.totalSize.cx = width;
    mode.totalSize.cy = height;
    mode.activeSize.cx = width;
    mode.activeSize.cy = height;
    mode.AdditionalSignalInfo.vSyncFreqDivider = vSyncDivider;
    mode.AdditionalSignalInfo.videoStandard = 255;
    mode.vSyncFreq.Numerator = vSync;
    mode.vSyncFreq.Denominator = 1;
    mode.hSyncFreq.Numerator = vSync * height;
    mode.hSyncFreq.Denominator = 1;
    mode.pixelRate = static_cast<UINT64>(vSync) * width * height;
    mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
}

IDDCX_MONITOR_MODE createMonitorMode(UINT width, UINT height, UINT vSync) {
    IDDCX_MONITOR_MODE mode{};
    mode.Size = sizeof(mode);
    mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    // IddCx requires vSyncFreqDivider == 0 for IDDCX_MONITOR_MODE.
    fillSignalInfo(mode.MonitorVideoSignalInfo, width, height, vSync, 0);
    return mode;
}

IDDCX_TARGET_MODE createTargetMode(UINT width, UINT height, UINT vSync) {
    IDDCX_TARGET_MODE mode{};
    mode.Size = sizeof(mode);
    // IddCx requires vSyncFreqDivider != 0 for IDDCX_TARGET_MODE.
    fillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, width, height, vSync, 1);
    return mode;
}

std::uint64_t queryQpcTicks() noexcept {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    return static_cast<std::uint64_t>(qpc.QuadPart);
}

class SharedMemoryPublisher {
  public:
    SharedMemoryPublisher() = default;
    ~SharedMemoryPublisher() { close(); }
    SharedMemoryPublisher(const SharedMemoryPublisher&) = delete;
    SharedMemoryPublisher& operator=(const SharedMemoryPublisher&) = delete;

    void ensureOpen() noexcept {
        if (region_) {
            return;
        }
        mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kShmName);
        if (!mapping_) {
            return;
        }
        region_ = static_cast<SharedRegion*>(MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedRegion)));
        if (!region_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return;
        }
        event_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, kEventName);
        if (region_->header.magic.load(std::memory_order_acquire) != kShmMagic ||
            region_->header.version.load(std::memory_order_acquire) != kShmVersion) {
            close();
        }
    }

    bool needsInitialFrame() noexcept {
        ensureOpen();
        return region_ && region_->header.frameSeq.load(std::memory_order_acquire) == 0;
    }

    bool pollCustomModeRequest(UINT& outWidth, UINT& outHeight, std::uint32_t& outSeq) noexcept {
        ensureOpen();
        if (!region_) {
            return false;
        }
        const auto reqSeq = region_->header.requestedModeSeq.load(std::memory_order_acquire);
        const auto appSeq = region_->header.appliedModeSeq.load(std::memory_order_relaxed);
        if (reqSeq == 0 || reqSeq == appSeq) {
            return false;
        }
        outWidth = region_->header.requestedWidth.load(std::memory_order_relaxed);
        outHeight = region_->header.requestedHeight.load(std::memory_order_relaxed);
        outSeq = reqSeq;
        return true;
    }

    void acknowledgeCustomMode(std::uint32_t seq) noexcept {
        if (region_) {
            region_->header.appliedModeSeq.store(seq, std::memory_order_release);
        }
    }

    void markActive(bool active, bool inSysMem) noexcept {
        ensureOpen();
        if (!region_) {
            return;
        }
        region_->header.inSystemMemory.store(inSysMem ? 1u : 0u, std::memory_order_relaxed);
        region_->header.heartbeatQpc.store(queryQpcTicks(), std::memory_order_relaxed);
        region_->header.driverActive.store(active ? 1u : 0u, std::memory_order_release);
    }

    void close() noexcept {
        if (region_) {
            region_->header.driverActive.store(0u, std::memory_order_release);
            UnmapViewOfFile(region_);
            region_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    void publish(const void* src, UINT width, UINT height, UINT pitch, DXGI_FORMAT format,
                 bool inSysMem, std::uint32_t acquireUs, std::uint32_t externalCopyUs,
                 const DirtyBand& dirty) noexcept {
        ensureOpen();
        if (!region_ || !src || !width || !height || width > kMaxWidth || height > kMaxHeight) {
            return;
        }
        if (format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            return;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
        const std::size_t totalBytes = rowBytes * height;
        if (totalBytes > kMaxFrameBytes || pitch < rowBytes) {
            return;
        }
        const std::uint32_t nextSlot = 1u - (region_->header.activeSlot.load(std::memory_order_relaxed) & 1u);
        auto* dst = region_->pixels[nextSlot];
        const auto copyStart = std::chrono::steady_clock::now();
        if (pitch == rowBytes) {
            std::memcpy(dst, src, totalBytes);
        } else {
            const auto* srcBytes = static_cast<const std::uint8_t*>(src);
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * rowBytes,
                            srcBytes + static_cast<std::size_t>(y) * pitch, rowBytes);
            }
        }
        const auto measuredCopyUs = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - copyStart).count());
        const auto ticks = queryQpcTicks();
        const auto seq = region_->header.frameSeq.load(std::memory_order_relaxed) + 1u;
        auto& slot = region_->header.slots[nextSlot];
        slot.frameSeq = seq;
        slot.qpcTicks = ticks;
        slot.width = width;
        slot.height = height;
        slot.pitch = static_cast<std::uint32_t>(rowBytes);
        slot.format = static_cast<std::uint32_t>(format);
        slot.dirtyRowBegin = dirty.rowBegin;
        slot.dirtyRowEnd = dirty.rowEnd;
        slot.changedPixels = dirty.changedPixels;
        slot.fullFrame = dirty.fullFrame ? 1u : 0u;
        slot.inSystemMemory = inSysMem ? 1u : 0u;
        slot.acquireMicroseconds = acquireUs;
        slot.copyMicroseconds = externalCopyUs ? externalCopyUs : measuredCopyUs;

        region_->header.width.store(width, std::memory_order_relaxed);
        region_->header.height.store(height, std::memory_order_relaxed);
        region_->header.inSystemMemory.store(inSysMem ? 1u : 0u, std::memory_order_relaxed);
        region_->header.heartbeatQpc.store(ticks, std::memory_order_relaxed);
        region_->header.driverActive.store(1u, std::memory_order_relaxed);
        region_->header.activeSlot.store(nextSlot, std::memory_order_release);
        region_->header.frameSeq.store(seq, std::memory_order_release);
        if (event_) {
            SetEvent(event_);
        }
    }

  private:
    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    SharedRegion* region_ = nullptr;
};

class SwapChainProcessor {
  public:
    SwapChainProcessor(IDDCX_MONITOR monitor, IDDCX_SWAPCHAIN swapChain, HANDLE availableEvent, LUID adapterLuid)
        : monitor_(monitor), swapChain_(swapChain), availableEvent_(availableEvent), adapterLuid_(adapterLuid) {
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this] { run(); });
    }
    ~SwapChainProcessor() {
        if (stopEvent_) {
            SetEvent(stopEvent_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (stopEvent_) {
            CloseHandle(stopEvent_);
        }
    }
    SwapChainProcessor(const SwapChainProcessor&) = delete;
    SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

  private:
    DirtyBand queryDirtyBand(UINT dirtyCount, UINT width, UINT height) noexcept {
        if (dirtyCount == 0 || dirtyCount > 64u) {
            return {0u, height, static_cast<std::uint64_t>(width) * height, true};
        }
        rectScratch_.resize(dirtyCount);
        IDARG_IN_GETDIRTYRECTS in{};
        in.DirtyRectInCount = dirtyCount;
        in.pDirtyRects = rectScratch_.data();
        IDARG_OUT_GETDIRTYRECTS out{};
        if (FAILED(IddCxSwapChainGetDirtyRects(swapChain_, &in, &out)) || out.DirtyRectOutCount == 0) {
            return {0u, height, static_cast<std::uint64_t>(width) * height, true};
        }
        const auto count = std::min<UINT>(out.DirtyRectOutCount, dirtyCount);
        dirtyScratch_.resize(count);
        for (UINT i = 0; i < count; ++i) {
            dirtyScratch_[i] = {rectScratch_[i].left, rectScratch_[i].top, rectScratch_[i].right, rectScratch_[i].bottom};
        }
        return computeDirtyBand(width, height, dirtyScratch_);
    }

    void run() noexcept {
        struct SwapChainRelease {
            IDDCX_SWAPCHAIN handle = nullptr;
            ~SwapChainRelease() {
                if (handle) {
                    WdfObjectDelete(handle);
                }
            }
        } swapChainGuard{swapChain_};

        ComPtr<IDXGIFactory5> factory;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        auto trySetDevice = [&](ComPtr<ID3D11Device>& dev) -> bool {
            if (!dev) {
                return false;
            }
            ComPtr<IDXGIDevice> dxgiDevice;
            if (FAILED(dev.As(&dxgiDevice))) {
                return false;
            }
            IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
            setDevice.pDevice = dxgiDevice.Get();
            return SUCCEEDED(IddCxSwapChainSetDevice(swapChain_, &setDevice));
        };

        bool deviceBound = false;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            if (SUCCEEDED(factory->EnumAdapterByLuid(adapterLuid_, IID_PPV_ARGS(&adapter))) && adapter) {
                if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                                &device, nullptr, &context))) {
                    deviceBound = trySetDevice(device);
                }
            }
        }
        if (!deviceBound) {
            device.Reset();
            context.Reset();
            if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                            &device, nullptr, &context))) {
                deviceBound = trySetDevice(device);
            }
        }
        if (!deviceBound) {
            return;
        }
        BOOL inSysMem = FALSE;
        (void)IddCxSwapChainInSystemMemory(swapChain_, &inSysMem);
        SharedMemoryPublisher publisher;
        publisher.markActive(true, inSysMem != FALSE);
        ComPtr<ID3D11Texture2D> staging;
        std::vector<std::uint8_t> cachedFrame;
        UINT cachedWidth = 0;
        UINT cachedHeight = 0;
        DXGI_FORMAT cachedFormat = DXGI_FORMAT_UNKNOWN;

        auto cacheFrame = [&](const void* src, UINT width, UINT height, UINT pitch, DXGI_FORMAT format) {
            if (!src || !width || !height || format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                return;
            }
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
            const std::size_t totalBytes = rowBytes * height;
            if (totalBytes > kMaxFrameBytes || pitch < rowBytes) {
                return;
            }
            cachedFrame.resize(totalBytes);
            if (pitch == rowBytes) {
                std::memcpy(cachedFrame.data(), src, totalBytes);
            } else {
                const auto* srcBytes = static_cast<const std::uint8_t*>(src);
                for (UINT y = 0; y < height; ++y) {
                    std::memcpy(cachedFrame.data() + static_cast<std::size_t>(y) * rowBytes,
                                srcBytes + static_cast<std::size_t>(y) * pitch, rowBytes);
                }
            }
            cachedWidth = width;
            cachedHeight = height;
            cachedFormat = format;
        };

        auto handleCustomModeRequest = [&]() noexcept {
            UINT reqW = 0;
            UINT reqH = 0;
            std::uint32_t reqSeq = 0;
            if (!publisher.pollCustomModeRequest(reqW, reqH, reqSeq)) {
                return;
            }
            if (monitor_ && isValidCustomMode(reqW, reqH)) {
                {
                    std::lock_guard lock(gCustomModeMutex);
                    gCustomMode = {reqW, reqH, 60};
                }
                const auto modes = collectActiveModes();
                std::vector<IDDCX_TARGET_MODE> targetModes;
                targetModes.reserve(modes.size());
                for (const auto& m : modes) {
                    targetModes.push_back(createTargetMode(m.width, m.height, m.vSync));
                }
                IDARG_IN_UPDATEMODES updateIn{};
                updateIn.Reason = IDDCX_UPDATE_REASON_HOST_LINK_BANDWIDTH;
                updateIn.TargetModeCount = static_cast<UINT>(targetModes.size());
                updateIn.pTargetModes = targetModes.data();
                (void)IddCxMonitorUpdateModes(monitor_, &updateIn);
            }
            publisher.acknowledgeCustomMode(reqSeq);
        };

        HANDLE waitHandles[2] = {availableEvent_, stopEvent_};
        while (true) {
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
                break;
            }
            handleCustomModeRequest();
            const auto acqStart = std::chrono::steady_clock::now();
            if (inSysMem) {
                IDDCX_METADATA meta{};
                meta.Size = sizeof(meta);
                IDDCX_SYSTEM_BUFFER_INFO sysInfo{};
                sysInfo.Size = sizeof(sysInfo);
                IDARG_OUT_RELEASEANDACQUIRESYSTEMBUFFER out{};
                out.pMetaData = &meta;
                out.pBufferInfo = &sysInfo;
                const HRESULT hr = IddCxSwapChainReleaseAndAcquireSystemBuffer(swapChain_, &out);
                const auto acqUs = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acqStart).count());
                if (hr == E_PENDING) {
                    const DWORD waited = WaitForMultipleObjects(2, waitHandles, FALSE, 25);
                    if (waited == WAIT_OBJECT_0 + 1) {
                        break;
                    }
                    if (waited == WAIT_TIMEOUT) {
                        publisher.markActive(true, true);
                        if (!cachedFrame.empty() && publisher.needsInitialFrame()) {
                            const DirtyBand full{0u, cachedHeight, static_cast<std::uint64_t>(cachedWidth) * cachedHeight, true};
                            publisher.publish(cachedFrame.data(), cachedWidth, cachedHeight, cachedWidth * 4u,
                                              cachedFormat, true, 0, 0, full);
                        }
                    }
                    continue;
                }
                if (FAILED(hr)) {
                    break;
                }
                const auto dirty = queryDirtyBand(meta.DirtyRectCount, sysInfo.Width, sysInfo.Height);
                cacheFrame(sysInfo.pBuffer, sysInfo.Width, sysInfo.Height, sysInfo.Pitch, sysInfo.Format);
                publisher.publish(sysInfo.pBuffer, sysInfo.Width, sysInfo.Height, sysInfo.Pitch,
                                  sysInfo.Format, true, acqUs, 0, dirty);
                (void)IddCxSwapChainFinishedProcessingFrame(swapChain_);
            } else {
                IDARG_OUT_RELEASEANDACQUIREBUFFER out{};
                const HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(swapChain_, &out);
                const auto acqUs = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acqStart).count());
                if (hr == E_PENDING) {
                    const DWORD waited = WaitForMultipleObjects(2, waitHandles, FALSE, 25);
                    if (waited == WAIT_OBJECT_0 + 1) {
                        break;
                    }
                    if (waited == WAIT_TIMEOUT) {
                        publisher.markActive(true, false);
                        if (!cachedFrame.empty() && publisher.needsInitialFrame()) {
                            const DirtyBand full{0u, cachedHeight, static_cast<std::uint64_t>(cachedWidth) * cachedHeight, true};
                            publisher.publish(cachedFrame.data(), cachedWidth, cachedHeight, cachedWidth * 4u,
                                              cachedFormat, false, 0, 0, full);
                        }
                    }
                    continue;
                }
                if (FAILED(hr)) {
                    break;
                }
                ComPtr<ID3D11Texture2D> tex;
                if (out.MetaData.pSurface && SUCCEEDED(out.MetaData.pSurface->QueryInterface(IID_PPV_ARGS(&tex)))) {
                    D3D11_TEXTURE2D_DESC desc{};
                    tex->GetDesc(&desc);
                    D3D11_TEXTURE2D_DESC prev{};
                    if (staging) {
                        staging->GetDesc(&prev);
                    }
                    if (!staging || prev.Width != desc.Width || prev.Height != desc.Height) {
                        staging.Reset();
                        D3D11_TEXTURE2D_DESC sdesc = desc;
                        sdesc.Usage = D3D11_USAGE_STAGING;
                        sdesc.BindFlags = 0;
                        sdesc.MiscFlags = 0;
                        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        device->CreateTexture2D(&sdesc, nullptr, &staging);
                    }
                    if (staging) {
                        const auto dirty = queryDirtyBand(out.MetaData.DirtyRectCount, desc.Width, desc.Height);
                        const auto mapStart = std::chrono::steady_clock::now();
                        context->CopyResource(staging.Get(), tex.Get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                            const auto mapUs = static_cast<std::uint32_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - mapStart).count());
                            cacheFrame(mapped.pData, desc.Width, desc.Height, mapped.RowPitch, desc.Format);
                            publisher.publish(mapped.pData, desc.Width, desc.Height, mapped.RowPitch,
                                              desc.Format, false, acqUs, mapUs, dirty);
                            context->Unmap(staging.Get(), 0);
                        }
                    }
                }
                (void)IddCxSwapChainFinishedProcessingFrame(swapChain_);
            }
        }
        publisher.markActive(false, inSysMem != FALSE);
    }

    IDDCX_MONITOR monitor_ = nullptr;
    IDDCX_SWAPCHAIN swapChain_ = nullptr;
    HANDLE availableEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    LUID adapterLuid_{};
    std::vector<RECT> rectScratch_;
    std::vector<DirtyRect> dirtyScratch_;
    std::thread thread_;
};

struct DeviceContext {
    WDFDEVICE device = nullptr;
    IDDCX_ADAPTER adapter = nullptr;
    IDDCX_MONITOR monitor = nullptr;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, getDeviceContext)

struct AdapterContext {
    DeviceContext* deviceCtx = nullptr;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, getAdapterContext)

struct MonitorContext {
    std::unique_ptr<SwapChainProcessor> processor;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, getMonitorContext)

NTSTATUS EvtMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* inArgs) noexcept {
    try {
        auto* ctx = getMonitorContext(monitor);
        ctx->processor.reset();
        ctx->processor = std::make_unique<SwapChainProcessor>(monitor, inArgs->hSwapChain, inArgs->hNextSurfaceAvailable, inArgs->RenderAdapterLuid);
        return STATUS_SUCCESS;
    } catch (...) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS EvtMonitorUnassignSwapChain(IDDCX_MONITOR monitor) noexcept {
    auto* ctx = getMonitorContext(monitor);
    ctx->processor.reset();
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorGetDefaultDescriptionModes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs) noexcept {
    const auto modes = collectActiveModes();
    outArgs->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    outArgs->PreferredMonitorModeIdx = 0;
    if (inArgs->DefaultMonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (inArgs->DefaultMonitorModeBufferInputCount < modes.size()) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t i = 0; i < modes.size(); ++i) {
        inArgs->pDefaultMonitorModes[i] = createMonitorMode(modes[i].width, modes[i].height, modes[i].vSync);
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorQueryTargetModes(IDDCX_MONITOR, const IDARG_IN_QUERYTARGETMODES* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs) noexcept {
    const auto modes = collectActiveModes();
    outArgs->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (inArgs->TargetModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (inArgs->TargetModeBufferInputCount < modes.size()) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t i = 0; i < modes.size(); ++i) {
        inArgs->pTargetModes[i] = createTargetMode(modes[i].width, modes[i].height, modes[i].vSync);
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs) noexcept {
    const auto modes = collectActiveModes();
    outArgs->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    outArgs->PreferredMonitorModeIdx = 0;
    if (inArgs->MonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (inArgs->MonitorModeBufferInputCount < modes.size()) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t i = 0; i < modes.size(); ++i) {
        inArgs->pMonitorModes[i] = createMonitorMode(modes[i].width, modes[i].height, modes[i].vSync);
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) noexcept {
    return STATUS_SUCCESS;
}

void EvtMonitorContextCleanup(WDFOBJECT object) noexcept {
    auto* ctx = getMonitorContext(object);
    ctx->~MonitorContext();
}

NTSTATUS EvtAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* inArgs) noexcept {
    if (!NT_SUCCESS(inArgs->AdapterInitStatus)) {
        return STATUS_SUCCESS;
    }
    auto* devCtx = getAdapterContext(adapter)->deviceCtx;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContext);
    attr.EvtCleanupCallback = EvtMonitorContextCleanup;

    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = 0;
    info.MonitorDescription.pData = nullptr;
    CoCreateGuid(&info.MonitorContainerId);

    IDARG_IN_MONITORCREATE createIn{};
    createIn.ObjectAttributes = &attr;
    createIn.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE createOut{};
    NTSTATUS status = IddCxMonitorCreate(adapter, &createIn, &createOut);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    devCtx->monitor = createOut.MonitorObject;
    auto* monCtx = getMonitorContext(devCtx->monitor);
    new (monCtx) MonitorContext();

    IDARG_OUT_MONITORARRIVAL arrivalOut{};
    return IddCxMonitorArrival(devCtx->monitor, &arrivalOut);
}

NTSTATUS EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) noexcept {
    auto* ctx = getDeviceContext(device);
    if (ctx->adapter) {
        return STATUS_SUCCESS;
    }
    static IDDCX_ENDPOINT_VERSION fwVer{sizeof(IDDCX_ENDPOINT_VERSION), 1, 0, 0};
    static IDDCX_ENDPOINT_VERSION hwVer{sizeof(IDDCX_ENDPOINT_VERSION), 1, 0, 0};

    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = kAdapterFriendlyName;
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Ultraterimal";
    caps.EndPointDiagnostics.pEndPointModelName = L"Virtual Display";
    caps.EndPointDiagnostics.pFirmwareVersion = &fwVer;
    caps.EndPointDiagnostics.pHardwareVersion = &hwVer;
    caps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;

    WDF_OBJECT_ATTRIBUTES adapterAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttr, AdapterContext);

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = device;
    init.pCaps = &caps;
    init.ObjectAttributes = &adapterAttr;
    IDARG_OUT_ADAPTER_INIT out{};
    NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
    if (NT_SUCCESS(status)) {
        ctx->adapter = out.AdapterObject;
        getAdapterContext(ctx->adapter)->deviceCtx = ctx;
    }
    return status;
}

NTSTATUS EvtDriverDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT deviceInit) noexcept {
    WDF_PNPPOWER_EVENT_CALLBACKS pnp{};
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = EvtDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnp);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxAdapterInitFinished = EvtAdapterInitFinished;
    config.EvtIddCxParseMonitorDescription = EvtParseMonitorDescription;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = EvtMonitorGetDefaultDescriptionModes;
    config.EvtIddCxMonitorQueryTargetModes = EvtMonitorQueryTargetModes;
    config.EvtIddCxAdapterCommitModes = EvtAdapterCommitModes;
    config.EvtIddCxMonitorAssignSwapChain = EvtMonitorAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = EvtMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &config);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContext);
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    auto* ctx = getDeviceContext(device);
    ctx->device = device;
    ctx->adapter = nullptr;
    ctx->monitor = nullptr;
    return IddCxDeviceInitialize(device);
}

} // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) noexcept {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
