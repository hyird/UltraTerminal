#include "capture.transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "capture_pixels.protocol.h"
#include "cursor.transport.h"
#include "desktop.transport.h"
#include "handle.types.h"
#include "idd/idd_shared.protocol.h"
#include "idd_capture.transport.h"
#include "idd_topology.transport.h"

namespace ultraterimal::agent {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
std::atomic<std::uint64_t> nextCaptureEpoch{1};
std::uint32_t microseconds(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(), 0, 60000000));
}
double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
void require(HRESULT status, const char* operation) {
    if (status == DXGI_ERROR_ACCESS_LOST || status == DXGI_ERROR_DEVICE_REMOVED || status == DXGI_ERROR_DEVICE_RESET || status == DXGI_ERROR_SESSION_DISCONNECTED) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_DEVICE_LOST);
    }
    if (status == E_ACCESSDENIED) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_ACCESS_DENIED);
    }
    if (status == DXGI_ERROR_UNSUPPORTED) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_UNSUPPORTED);
    }
    if (status == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_BUSY);
    }
    if (FAILED(status)) {
        throw std::system_error(static_cast<int>(status), std::system_category(), operation);
    }
}
struct AcquiredFrame {
    IDXGIOutputDuplication* duplication;
    void release() {
        if (duplication) {
            duplication->ReleaseFrame();
            duplication = nullptr;
        }
    }
    ~AcquiredFrame() { release(); }
};
struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ~MappedTexture() { context->Unmap(texture, 0); }
};
struct MappedDesktop {
    IDXGIOutputDuplication* duplication;
    ~MappedDesktop() { duplication->UnMapDesktopSurface(); }
};
class DisplayActivity {
  public:
    DisplayActivity() {
        wchar_t description[] = L"Ultraterimal 远程桌面会话";
        REASON_CONTEXT reason{};
        reason.Version = POWER_REQUEST_CONTEXT_VERSION;
        reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        reason.Reason.SimpleReasonString = description;
        request_.reset(PowerCreateRequest(&reason));
        if (request_) {
            (void)PowerSetRequest(request_.get(), PowerRequestSystemRequired);
            (void)PowerSetRequest(request_.get(), PowerRequestDisplayRequired);
        }
        (void)SetThreadExecutionState(ES_DISPLAY_REQUIRED);
    }
    ~DisplayActivity() {
        if (request_) {
            PowerClearRequest(request_.get(), PowerRequestDisplayRequired);
            PowerClearRequest(request_.get(), PowerRequestSystemRequired);
        }
    }

  private:
    UniqueHandle request_;
};
} // namespace
struct DesktopCapture::State {
    std::optional<DisplayActivity> activity;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    std::array<ComPtr<ID3D11Texture2D>, 2> staging;
    std::size_t stagingIndex = 0;
    ComPtr<ID3D11Texture2D> latest;
    std::optional<CapturedFrame> deferred;
    DXGI_OUTDUPL_DESC description{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerInfo{};
    DXGI_OUTDUPL_POINTER_POSITION pointerPosition{};
    std::vector<std::uint8_t> pointerShape;
    RECT bounds{};
    std::wstring displayName;
    std::uint64_t desktopGeneration = 0;
    const std::uint64_t captureEpoch = nextCaptureEpoch.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t sourceSerial = 0;
    std::uint64_t lastReadbackSerial = 0;
    std::uint64_t lastDirectSerial = 0;
    const std::uint8_t* lastReadbackData = nullptr;
    std::vector<std::uint8_t> metadataBuffer;
    std::vector<CapturePixelRect> dirtyRects;
    bool pixelsCaptured = false;
    std::uint32_t lastPixelsWidth = 0, lastPixelsHeight = 0;
    SystemCursorSuppression cursorSuppression;
    std::unique_ptr<IddSharedCapture> iddCapture;
    bool extractDirtyRects(const DXGI_OUTDUPL_FRAME_INFO& info, std::uint32_t width, std::uint32_t height, std::uint64_t& totalArea) {
        totalArea = 0;
        dirtyRects.clear();
        if (!info.TotalMetadataBufferSize || info.TotalMetadataBufferSize > 256 * 1024) {
            return false;
        }
        metadataBuffer.resize(info.TotalMetadataBufferSize);
        auto addClamped = [&](std::int64_t left, std::int64_t top, std::int64_t right, std::int64_t bottom) {
            const auto l = static_cast<std::uint32_t>(std::clamp<std::int64_t>(left, 0, width));
            const auto t = static_cast<std::uint32_t>(std::clamp<std::int64_t>(top, 0, height));
            const auto r = static_cast<std::uint32_t>(std::clamp<std::int64_t>(right, 0, width));
            const auto b = static_cast<std::uint32_t>(std::clamp<std::int64_t>(bottom, 0, height));
            if (l < r && t < b) {
                dirtyRects.push_back({l, t, r, b});
                totalArea += static_cast<std::uint64_t>(r - l) * (b - t);
            }
        };
        UINT moveBytes = 0;
        if (FAILED(duplication->GetFrameMoveRects(static_cast<UINT>(metadataBuffer.size()), reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadataBuffer.data()), &moveBytes))) {
            return false;
        }
        const auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(metadataBuffer.data());
        const auto moveCount = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
        for (std::size_t index = 0; index < moveCount; ++index) {
            const auto& move = moves[index];
            const auto w = static_cast<std::int64_t>(move.DestinationRect.right) - move.DestinationRect.left;
            const auto h = static_cast<std::int64_t>(move.DestinationRect.bottom) - move.DestinationRect.top;
            addClamped(move.DestinationRect.left, move.DestinationRect.top, move.DestinationRect.right, move.DestinationRect.bottom);
            if (w > 0 && h > 0) {
                addClamped(move.SourcePoint.x, move.SourcePoint.y, static_cast<std::int64_t>(move.SourcePoint.x) + w, static_cast<std::int64_t>(move.SourcePoint.y) + h);
            }
        }
        UINT dirtyBytes = 0;
        if (FAILED(duplication->GetFrameDirtyRects(static_cast<UINT>(metadataBuffer.size()), reinterpret_cast<RECT*>(metadataBuffer.data()), &dirtyBytes))) {
            return false;
        }
        const auto* rects = reinterpret_cast<const RECT*>(metadataBuffer.data());
        const auto rectCount = dirtyBytes / sizeof(RECT);
        for (std::size_t index = 0; index < rectCount; ++index) {
            addClamped(rects[index].left, rects[index].top, rects[index].right, rects[index].bottom);
        }
        return !dirtyRects.empty() && dirtyRects.size() <= 64 && totalArea <= static_cast<std::uint64_t>(width) * height * 3 / 5;
    }
    ID3D11Texture2D* ensureStaging(const D3D11_TEXTURE2D_DESC& textureDescription) {
        auto& slot = staging[stagingIndex];
        stagingIndex = (stagingIndex + 1) % staging.size();
        D3D11_TEXTURE2D_DESC previous{};
        if (slot) {
            slot->GetDesc(&previous);
        }
        if (!slot || previous.Width != textureDescription.Width || previous.Height != textureDescription.Height) {
            if (slot && (previous.Width != textureDescription.Width || previous.Height != textureDescription.Height)) {
                for (auto& entry : staging) {
                    entry.Reset();
                }
            }
            auto stagingDescription = textureDescription;
            stagingDescription.BindFlags = 0;
            stagingDescription.MiscFlags = 0;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            require(device->CreateTexture2D(&stagingDescription, nullptr, &slot), "Create staging texture");
        }
        return slot.Get();
    }
    void copyStagingPixels(const ComPtr<ID3D11Texture2D>& source, const D3D11_TEXTURE2D_DESC& textureDescription, CapturedFrame& frame, CaptureTiming* timing, AcquiredFrame* releaseBeforeMap = nullptr) {
        auto* target = ensureStaging(textureDescription);
        const auto copyStarted = timing ? Clock::now() : Clock::time_point{};
        context->CopyResource(target, source.Get());
        if (releaseBeforeMap) {
            context->Flush();
            releaseBeforeMap->release();
        }
        const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(target, 0, D3D11_MAP_READ, 0, &mapped), "Map desktop texture");
        const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
        Clock::time_point rowsEnded;
        {
            MappedTexture mapping{context.Get(), target};
            copyCapturePixels(frame.bgra, frame.width, frame.height, mapped.pData, mapped.RowPitch);
            rowsEnded = timing ? Clock::now() : Clock::time_point{};
        }
        if (timing) {
            timing->copyDispatchMilliseconds = milliseconds(copyStarted, mapStarted);
            timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
            timing->rowCopyMilliseconds = milliseconds(rowsStarted, rowsEnded);
            timing->stages.mapMicroseconds = microseconds(mapStarted, rowsStarted);
            timing->stages.rowCopyMicroseconds = microseconds(rowsStarted, rowsEnded);
            if (timing->verifyWithStaging) {
                timing->matchedStaging = true;
            }
        }
    }
    void copyStagingRects(const ComPtr<ID3D11Texture2D>& source, const D3D11_TEXTURE2D_DESC& textureDescription, CapturedFrame& frame, CaptureTiming* timing, AcquiredFrame* releaseBeforeMap = nullptr) {
        auto* target = ensureStaging(textureDescription);
        const auto copyStarted = timing ? Clock::now() : Clock::time_point{};
        for (const auto& rect : dirtyRects) {
            const D3D11_BOX box{rect.left, rect.top, 0, rect.right, rect.bottom, 1};
            context->CopySubresourceRegion(target, 0, rect.left, rect.top, 0, source.Get(), 0, &box);
        }
        if (releaseBeforeMap) {
            context->Flush();
            releaseBeforeMap->release();
        }
        const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(target, 0, D3D11_MAP_READ, 0, &mapped), "Map desktop texture");
        const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
        Clock::time_point rowsEnded;
        {
            MappedTexture mapping{context.Get(), target};
            copyCapturePixelRects(frame.bgra, frame.width, frame.height, mapped.pData, mapped.RowPitch, dirtyRects);
            rowsEnded = timing ? Clock::now() : Clock::time_point{};
        }
        if (timing) {
            timing->copyDispatchMilliseconds = milliseconds(copyStarted, mapStarted);
            timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
            timing->rowCopyMilliseconds = milliseconds(rowsStarted, rowsEnded);
            timing->stages.mapMicroseconds = microseconds(mapStarted, rowsStarted);
            timing->stages.rowCopyMicroseconds = microseconds(rowsStarted, rowsEnded);
        }
    }
    bool consumeDirectPixels(const ComPtr<ID3D11Texture2D>& source, const D3D11_TEXTURE2D_DESC& textureDescription, const DXGI_OUTDUPL_FRAME_INFO* frameInfo, bool allowIncremental, CapturedFrame& frame, CaptureTiming* timing, AcquiredFrame* releaseBeforeMap, const DirectPixelConsumer& consumer) {
        std::uint64_t dirtyArea = 0;
        const bool incremental = allowIncremental && frameInfo && !(timing && timing->verifyWithStaging) &&
                                 sourceSerial == lastDirectSerial + 1 && extractDirtyRects(*frameInfo, frame.width, frame.height, dirtyArea);
        std::uint32_t rowBegin = 0;
        std::uint32_t rowEnd = frame.height;
        frame.dirtyRowBegin = 0;
        frame.dirtyRowEnd = frame.height;
        frame.changedPixels = static_cast<std::uint64_t>(frame.width) * frame.height;
        if (incremental) {
            std::uint32_t minTop = frame.height;
            std::uint32_t maxBottom = 0;
            for (const auto& rect : dirtyRects) {
                minTop = std::min(minTop, rect.top);
                maxBottom = std::max(maxBottom, rect.bottom);
            }
            rowBegin = minTop & ~1u;
            rowEnd = std::min(frame.height, (maxBottom + 1u) & ~1u);
            frame.dirtyRowBegin = rowBegin;
            frame.dirtyRowEnd = rowEnd;
            frame.changedPixels = std::min<std::uint64_t>(dirtyArea, static_cast<std::uint64_t>(frame.width) * frame.height);
        }
        bool consumed = false;
        if (description.DesktopImageInSystemMemory && !(timing && timing->forceStaging)) {
            DXGI_MAPPED_RECT mapped{};
            const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
            require(duplication->MapDesktopSurface(&mapped), "Map desktop surface");
            const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
            Clock::time_point rowsEnded;
            {
                MappedDesktop mapping{duplication.Get()};
                if (mapped.Pitch < 0) {
                    throw std::runtime_error("Invalid desktop row pitch");
                }
                const auto pitch = static_cast<std::size_t>(mapped.Pitch);
                consumed = consumer(std::span<const std::uint8_t>(mapped.pBits, pitch * frame.height), pitch,
                                    frame.width, frame.height, rowBegin, rowEnd, frame.captureEpoch, frame.sourceSerial, incremental);
                rowsEnded = timing ? Clock::now() : Clock::time_point{};
            }
            if (timing) {
                timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
                timing->rowCopyMilliseconds = milliseconds(rowsStarted, rowsEnded);
                timing->stages.mapMicroseconds = microseconds(mapStarted, rowsStarted);
                timing->stages.rowCopyMicroseconds = microseconds(rowsStarted, rowsEnded);
            }
        } else {
            auto* target = ensureStaging(textureDescription);
            const auto copyStarted = timing ? Clock::now() : Clock::time_point{};
            if (incremental && rowBegin < rowEnd) {
                const D3D11_BOX box{0, rowBegin, 0, frame.width, rowEnd, 1};
                context->CopySubresourceRegion(target, 0, 0, rowBegin, 0, source.Get(), 0, &box);
            } else {
                context->CopyResource(target, source.Get());
            }
            if (releaseBeforeMap) {
                context->Flush();
                releaseBeforeMap->release();
            }
            const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
            D3D11_MAPPED_SUBRESOURCE mapped{};
            require(context->Map(target, 0, D3D11_MAP_READ, 0, &mapped), "Map desktop texture");
            const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
            Clock::time_point rowsEnded;
            {
                MappedTexture mapping{context.Get(), target};
                const auto pitch = static_cast<std::size_t>(mapped.RowPitch);
                consumed = consumer(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(mapped.pData), pitch * frame.height), pitch,
                                    frame.width, frame.height, rowBegin, rowEnd, frame.captureEpoch, frame.sourceSerial, incremental);
                rowsEnded = timing ? Clock::now() : Clock::time_point{};
            }
            if (timing) {
                timing->copyDispatchMilliseconds = milliseconds(copyStarted, mapStarted);
                timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
                timing->rowCopyMilliseconds = milliseconds(rowsStarted, rowsEnded);
                timing->stages.mapMicroseconds = microseconds(mapStarted, rowsStarted);
                timing->stages.rowCopyMicroseconds = microseconds(rowsStarted, rowsEnded);
            }
        }
        if (consumed) {
            lastDirectSerial = sourceSerial;
            lastReadbackSerial = 0;
            lastReadbackData = nullptr;
            frame.bgra.clear();
        }
        return consumed;
    }
    void copyPixels(const ComPtr<ID3D11Texture2D>& source, const DXGI_OUTDUPL_FRAME_INFO* frameInfo, bool allowIncremental, CapturedFrame& frame, std::vector<std::uint8_t>* reusablePixels, CaptureTiming* timing, AcquiredFrame* releaseBeforeMap = nullptr, const DirectPixelConsumer* directConsumer = nullptr) {
        D3D11_TEXTURE2D_DESC textureDescription{};
        source->GetDesc(&textureDescription);
        const auto byteCount = static_cast<std::uint64_t>(textureDescription.Width) * textureDescription.Height * 4;
        if (!textureDescription.Width || !textureDescription.Height || byteCount > 64 * 1024 * 1024 || textureDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            textureDescription.Width != frame.width || textureDescription.Height != frame.height) {
            throw CaptureReset();
        }
        if (directConsumer && *directConsumer && !(timing && timing->verifyWithStaging)) {
            if (consumeDirectPixels(source, textureDescription, frameInfo, allowIncremental, frame, timing, releaseBeforeMap, *directConsumer)) {
                return;
            }
        }
        lastDirectSerial = 0;
        std::uint64_t dirtyArea = 0;
        const bool incremental = allowIncremental && frameInfo && !(timing && timing->verifyWithStaging) && reusablePixels &&
                                 reusablePixels->size() == static_cast<std::size_t>(byteCount) && reusablePixels->data() == lastReadbackData &&
                                 sourceSerial == lastReadbackSerial + 1 && extractDirtyRects(*frameInfo, frame.width, frame.height, dirtyArea);
        if (reusablePixels) {
            frame.bgra.swap(*reusablePixels);
        }
        frame.bgra.resize(static_cast<std::size_t>(byteCount));
        frame.dirtyRowBegin = 0;
        frame.dirtyRowEnd = frame.height;
        frame.changedPixels = static_cast<std::uint64_t>(frame.width) * frame.height;
        if (incremental) {
            std::uint32_t minTop = frame.height;
            std::uint32_t maxBottom = 0;
            for (const auto& rect : dirtyRects) {
                minTop = std::min(minTop, rect.top);
                maxBottom = std::max(maxBottom, rect.bottom);
            }
            frame.dirtyRowBegin = minTop & ~1u;
            frame.dirtyRowEnd = std::min(frame.height, (maxBottom + 1u) & ~1u);
            frame.changedPixels = std::min<std::uint64_t>(dirtyArea, static_cast<std::uint64_t>(frame.width) * frame.height);
            if (description.DesktopImageInSystemMemory && !(timing && timing->forceStaging)) {
                DXGI_MAPPED_RECT mapped{};
                const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
                require(duplication->MapDesktopSurface(&mapped), "Map desktop surface");
                const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
                Clock::time_point rowsEnded;
                {
                    MappedDesktop mapping{duplication.Get()};
                    if (mapped.Pitch < 0) {
                        throw std::runtime_error("Invalid desktop row pitch");
                    }
                    copyCapturePixelRects(frame.bgra, frame.width, frame.height, mapped.pBits, static_cast<std::size_t>(mapped.Pitch), dirtyRects);
                    rowsEnded = timing ? Clock::now() : Clock::time_point{};
                }
                if (timing) {
                    timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
                    timing->rowCopyMilliseconds = milliseconds(rowsStarted, rowsEnded);
                    timing->stages.mapMicroseconds = microseconds(mapStarted, rowsStarted);
                    timing->stages.rowCopyMicroseconds = microseconds(rowsStarted, rowsEnded);
                }
            } else {
                copyStagingRects(source, textureDescription, frame, timing, releaseBeforeMap);
            }
            lastReadbackSerial = sourceSerial;
            lastReadbackData = frame.bgra.data();
            return;
        }
        // DXGI can expose the duplicated desktop in system memory directly.
        // Avoid a redundant GPU copy and a blocking staging-texture Map in that case.
        if (description.DesktopImageInSystemMemory && !(timing && timing->forceStaging)) {
            DXGI_MAPPED_RECT mapped{};
            const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
            require(duplication->MapDesktopSurface(&mapped), "Map desktop surface");
            const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
            Clock::time_point rowsEnded;
            {
                MappedDesktop mapping{duplication.Get()};
                if (mapped.Pitch < 0) {
                    throw std::runtime_error("Invalid desktop row pitch");
                }
                copyCapturePixels(frame.bgra, frame.width, frame.height, mapped.pBits, static_cast<std::size_t>(mapped.Pitch));
                rowsEnded = timing ? Clock::now() : Clock::time_point{};
            }
            if (timing) {
                timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
                timing->rowCopyMilliseconds = milliseconds(rowsStarted, rowsEnded);
                timing->stages.mapMicroseconds = microseconds(mapStarted, rowsStarted);
                timing->stages.rowCopyMicroseconds = microseconds(rowsStarted, rowsEnded);
                if (timing->verifyWithStaging) {
                    CapturedFrame reference;
                    reference.width = frame.width;
                    reference.height = frame.height;
                    reference.bgra.resize(frame.bgra.size());
                    CaptureTiming stagingTiming;
                    copyStagingPixels(source, textureDescription, reference, &stagingTiming);
                    timing->verificationStagingMapMilliseconds = stagingTiming.mapMilliseconds;
                    timing->verificationStagingRowCopyMilliseconds = stagingTiming.rowCopyMilliseconds;
                    timing->matchedStaging = reference.bgra == frame.bgra;
                    if (!timing->matchedStaging) {
                        throw std::runtime_error("DXGI desktop map disagrees with staging readback");
                    }
                }
            }
            lastReadbackSerial = sourceSerial;
            lastReadbackData = frame.bgra.data();
            return;
        }
        copyStagingPixels(source, textureDescription, frame, timing, releaseBeforeMap);
        lastReadbackSerial = sourceSerial;
        lastReadbackData = frame.bgra.data();
    }
};
DesktopCapture::DesktopCapture(std::uint32_t displayIndex) : state_(std::make_unique<State>()) {
    state_->desktopGeneration = requireInputDesktop();
    // Idle display power saving can leave DuplicateOutput waiting indefinitely
    // for its first frame. The request lives exactly as long as this capture.
    state_->activity.emplace();
    if (displayIndex == 0) {
        (void)ensureIddPrimaryIfSoftwareDisplay();
    }
    ComPtr<IDXGIFactory1> factory;
    require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIOutput1> selected;
    ComPtr<IDXGIAdapter1> selectedAdapter;
    std::uint32_t attachedIndex = 0;
    for (UINT i = 0; !selected; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto adapterStatus = factory->EnumAdapters1(i, &adapter);
        if (adapterStatus == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        require(adapterStatus, "EnumAdapters1");
        for (UINT j = 0;; ++j) {
            ComPtr<IDXGIOutput> output;
            const auto status = adapter->EnumOutputs(j, &output);
            if (status == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            require(status, "EnumOutputs");
            DXGI_OUTPUT_DESC description{};
            require(output->GetDesc(&description), "GetDesc output");
            if (!description.AttachedToDesktop) {
                continue;
            }
            if (attachedIndex++ == displayIndex) {
                require(output.As(&selected), "IDXGIOutput1");
                selectedAdapter = adapter;
                break;
            }
        }
    }
    if (!selected) {
        throw CaptureReset(wire::DesktopFault::DISPLAY_UNAVAILABLE);
    }
    DXGI_OUTPUT_DESC outputDescription{};
    require(selected->GetDesc(&outputDescription), "Get selected desktop bounds");
    state_->bounds = outputDescription.DesktopCoordinates;
    state_->displayName = outputDescription.DeviceName;
    if (isDisplayUltraterimalIdd(state_->displayName)) {
        auto idd = std::make_unique<IddSharedCapture>();
        if (idd->waitForDriverReady(std::chrono::milliseconds(500)) && idd->isActiveInSystemMemory()) {
            state_->iddCapture = std::move(idd);
            state_->description.DesktopImageInSystemMemory = TRUE;
            state_->description.Rotation = outputDescription.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED ? outputDescription.Rotation : DXGI_MODE_ROTATION_IDENTITY;
            return;
        }
    }
    require(D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              nullptr, 0, D3D11_SDK_VERSION, &state_->device, nullptr, &state_->context),
            "D3D11CreateDevice");
    const auto duplicationStatus = selected->DuplicateOutput(state_->device.Get(), &state_->duplication);
    // Mode/desktop transitions can temporarily deny duplication or leave the
    // output unsupported/unavailable. Use the owner's bounded recovery budget,
    // not a fatal session error or an unbounded retry inside this worker.
    if (duplicationStatus == E_ACCESSDENIED) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_ACCESS_DENIED);
    }
    if (duplicationStatus == DXGI_ERROR_UNSUPPORTED) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_UNSUPPORTED);
    }
    if (duplicationStatus == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_BUSY);
    }
    require(duplicationStatus, "DuplicateOutput (requires an accessible interactive desktop)");
    state_->duplication->GetDesc(&state_->description);
}
DesktopCapture::~DesktopCapture() = default;
std::vector<DesktopCapture::DisplayMode> DesktopCapture::displayModes() const {
    std::vector<DisplayMode> modes;
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(state_->displayName.c_str(), ENUM_CURRENT_SETTINGS, &current)) {
        return modes;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; index < 4096 && EnumDisplaySettingsW(state_->displayName.c_str(), index, &mode); ++index) {
        if (mode.dmPelsWidth < 16 || mode.dmPelsWidth > 4096 || mode.dmPelsHeight < 16 || mode.dmPelsHeight > 2160 ||
            mode.dmPelsWidth % 2 || mode.dmPelsHeight % 2 || mode.dmDisplayOrientation != current.dmDisplayOrientation || mode.dmBitsPerPel != current.dmBitsPerPel) {
            continue;
        }
        const DisplayMode value{mode.dmPelsWidth, mode.dmPelsHeight};
        if (std::ranges::none_of(modes, [value](const auto& entry) { return entry.width == value.width && entry.height == value.height; })) {
            modes.push_back(value);
            if (modes.size() == 128) {
                break;
            }
        }
    }
    std::ranges::sort(modes, [](const auto& left, const auto& right) { return left.width != right.width ? left.width < right.width : left.height < right.height; });
    return modes;
}
bool DesktopCapture::resize(std::uint32_t width, std::uint32_t height) {
    if (requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
    }
    auto hasMode = [&] {
        const auto modes = displayModes();
        return std::ranges::any_of(modes, [=](const auto& mode) { return mode.width == width && mode.height == height; });
    };
    if (!hasMode()) {
        if (!state_->iddCapture || !idd::isValidCustomMode(width, height) ||
            !state_->iddCapture->requestCustomMode(width, height)) {
            return false;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(400);
        while (!hasMode() && Clock::now() < deadline) {
            Sleep(20);
        }
        if (!hasMode()) {
            return false;
        }
    }
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(state_->displayName.c_str(), ENUM_CURRENT_SETTINGS, &current)) {
        return false;
    }
    // Preserve orientation, position, depth and refresh rate. The driver must
    // validate the complete mode before saving it for all users. Even an
    // unchanged current mode may still be a temporary, unsaved selection.
    current.dmPelsWidth = width;
    current.dmPelsHeight = height;
    if (ChangeDisplaySettingsExW(state_->displayName.c_str(), &current, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        return false;
    }
    current.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    return ChangeDisplaySettingsExW(state_->displayName.c_str(), &current, nullptr, CDS_UPDATEREGISTRY | CDS_GLOBAL, nullptr) == DISP_CHANGE_SUCCESSFUL;
}
ID3D11Device* DesktopCapture::device() const { return state_->device.Get(); }
ID3D11Texture2D* DesktopCapture::texture() const { return state_->latest.Get(); }
void DesktopCapture::readTexture(CapturedFrame& frame, CaptureTiming* timing) {
    if (!state_->latest || requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
    }
    D3D11_TEXTURE2D_DESC description{};
    state_->latest->GetDesc(&description);
    if (description.Width != frame.width || description.Height != frame.height ||
        frame.sourceSerial != state_->sourceSerial || frame.captureEpoch != state_->captureEpoch) {
        throw CaptureReset();
    }
    frame.bgra.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
    CaptureTiming localTiming;
    if (!timing) {
        timing = &localTiming;
    }
    timing->stages.mapMicroseconds.reset();
    timing->stages.rowCopyMicroseconds.reset();
    state_->copyStagingPixels(state_->latest, description, frame, timing);
    frame.dirtyRowBegin = 0;
    frame.dirtyRowEnd = frame.height;
    frame.changedPixels = static_cast<std::uint64_t>(frame.width) * frame.height;
    frame.stageTiming.mapMicroseconds = timing->stages.mapMicroseconds;
    frame.stageTiming.rowCopyMicroseconds = timing->stages.rowCopyMicroseconds;
    state_->deferred.reset();
    state_->pixelsCaptured = true;
    state_->lastPixelsWidth = frame.width;
    state_->lastPixelsHeight = frame.height;
    state_->lastReadbackSerial = state_->sourceSerial;
    state_->lastReadbackData = frame.bgra.data();
    if (requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
    }
}
std::optional<CapturedFrame> DesktopCapture::nextFrame(std::chrono::milliseconds timeout, std::vector<std::uint8_t>* reusablePixels, CaptureTiming* timing, bool readPixels, bool retainTexture, const DirectPixelConsumer* directConsumer) {
    if (timeout.count() < 0 || timeout.count() > 5000) {
        throw std::invalid_argument("Capture wait must be between 0 and 5000 milliseconds");
    }
    if (requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
    }
    CaptureTiming localTiming;
    if (!timing) {
        timing = &localTiming;
    }
    if (state_->iddCapture) {
        if (timing) {
            const bool forceStaging = timing->forceStaging;
            const bool verifyWithStaging = timing->verifyWithStaging;
            *timing = {};
            timing->forceStaging = forceStaging;
            timing->verifyWithStaging = verifyWithStaging;
            timing->desktopImageInSystemMemory = true;
        }
        if (!readPixels) {
            return std::nullopt;
        }
        CapturedFrame frame;
        frame.desktopLeft = state_->bounds.left;
        frame.desktopTop = state_->bounds.top;
        const bool gotFrame = state_->iddCapture->acquireFrame(
            timeout, !state_->pixelsCaptured, state_->captureEpoch, state_->sourceSerial,
            frame, reusablePixels, timing, directConsumer);
        if (!gotFrame) {
            if (state_->cursorSuppression.active() && state_->pixelsCaptured && state_->lastPixelsWidth && state_->lastPixelsHeight) {
                CapturedFrame cursorFrame;
                cursorFrame.width = state_->lastPixelsWidth;
                cursorFrame.height = state_->lastPixelsHeight;
                cursorFrame.desktopLeft = state_->bounds.left;
                cursorFrame.desktopTop = state_->bounds.top;
                cursorFrame.rotation = static_cast<std::uint32_t>(state_->description.Rotation);
                cursorFrame.captureEpoch = state_->captureEpoch;
                cursorFrame.sourceSerial = state_->sourceSerial;
                if (state_->cursorSuppression.pollCursorChange(cursorFrame, state_->bounds.left, state_->bounds.top)) {
                    cursorFrame.stageTiming = timing->stages;
                    return cursorFrame;
                }
            }
            return std::nullopt;
        }
        state_->cursorSuppression.applyFallbackCursor(frame, state_->bounds.left, state_->bounds.top);
        state_->pixelsCaptured = true;
        state_->lastPixelsWidth = frame.width;
        state_->lastPixelsHeight = frame.height;
        if (requireInputDesktop() != state_->desktopGeneration) {
            throw CaptureReset();
        }
        frame.stageTiming = timing->stages;
        return frame;
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    if (timing) {
        const bool forceStaging = timing->forceStaging;
        const bool verifyWithStaging = timing->verifyWithStaging;
        *timing = {};
        timing->forceStaging = forceStaging;
        timing->verifyWithStaging = verifyWithStaging;
        timing->desktopImageInSystemMemory = state_->description.DesktopImageInSystemMemory != FALSE;
    }
    const auto acquireStarted = timing ? Clock::now() : Clock::time_point{};
    const auto result = state_->duplication->AcquireNextFrame(static_cast<UINT>(timeout.count()), &info, &resource);
    if (timing) {
        const auto acquireEnded = Clock::now();
        timing->acquireMilliseconds = milliseconds(acquireStarted, acquireEnded);
        timing->stages.acquireWaitMicroseconds = microseconds(acquireStarted, acquireEnded);
    }
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        if (readPixels && state_->deferred) {
            auto frame = std::move(*state_->deferred);
            state_->deferred.reset();
            if (reusablePixels) {
                frame.bgra.swap(*reusablePixels);
            }
            frame.bgra.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
            D3D11_TEXTURE2D_DESC description{};
            state_->latest->GetDesc(&description);
            state_->copyStagingPixels(state_->latest, description, frame, timing);
            frame.dirtyRowBegin = 0;
            frame.dirtyRowEnd = frame.height;
            frame.changedPixels = static_cast<std::uint64_t>(frame.width) * frame.height;
            state_->pixelsCaptured = true;
            state_->lastPixelsWidth = frame.width;
            state_->lastPixelsHeight = frame.height;
            state_->lastReadbackSerial = state_->sourceSerial;
            state_->lastReadbackData = frame.bgra.data();
            if (requireInputDesktop() != state_->desktopGeneration) {
                throw CaptureReset();
            }
            frame.stageTiming = timing->stages;
            return frame;
        }
        if (state_->cursorSuppression.active() && state_->pixelsCaptured && state_->lastPixelsWidth && state_->lastPixelsHeight) {
            CapturedFrame cursorFrame;
            cursorFrame.width = state_->lastPixelsWidth;
            cursorFrame.height = state_->lastPixelsHeight;
            cursorFrame.desktopLeft = state_->bounds.left;
            cursorFrame.desktopTop = state_->bounds.top;
            cursorFrame.rotation = static_cast<std::uint32_t>(state_->description.Rotation);
            cursorFrame.captureEpoch = state_->captureEpoch;
            cursorFrame.sourceSerial = state_->sourceSerial;
            if (state_->cursorSuppression.pollCursorChange(cursorFrame, state_->bounds.left, state_->bounds.top)) {
                cursorFrame.stageTiming = timing->stages;
                return cursorFrame;
            }
        }
        return std::nullopt;
    }
    if (FAILED(result)) {
    }
    require(result, "AcquireNextFrame (recreate capture after access loss)");
    AcquiredFrame acquired{state_->duplication.Get()};
    ComPtr<ID3D11Texture2D> source;
    require(resource.As(&source), "Desktop texture");
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    const auto byteCount = static_cast<std::uint64_t>(description.Width) * description.Height * 4;
    if (!description.Width || !description.Height || byteCount > 64 * 1024 * 1024 || description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        throw std::runtime_error("Unsupported desktop texture dimensions or format");
    }
    CapturedFrame frame;
    frame.width = description.Width;
    frame.height = description.Height;
    frame.desktopLeft = state_->bounds.left;
    frame.desktopTop = state_->bounds.top;
    frame.rotation = static_cast<std::uint32_t>(state_->description.Rotation);
    frame.captureTicks = static_cast<std::uint64_t>(info.LastPresentTime.QuadPart);
    // DXGI reports cursor changes independently. Avoid a GPU readback, map
    // synchronization and a full-frame allocation/copy for those updates.
    const bool desktopUpdated = (!state_->pixelsCaptured && !state_->deferred) || info.LastPresentTime.QuadPart != 0;
    if (desktopUpdated) {
        if (state_->sourceSerial == std::numeric_limits<std::uint64_t>::max()) {
            throw CaptureReset();
        }
        ++state_->sourceSerial;
    }
    frame.captureEpoch = state_->captureEpoch;
    frame.sourceSerial = state_->sourceSerial;
    // A CPU encoder already owns the pixels after readback. Keep a GPU copy
    // only when requested, deferred, or new geometry may select a GPU encoder.
    const bool geometryChanged = description.Width != state_->lastPixelsWidth || description.Height != state_->lastPixelsHeight;
    if ((!readPixels || retainTexture || geometryChanged) && desktopUpdated) {
        D3D11_TEXTURE2D_DESC previous{};
        if (state_->latest) {
            state_->latest->GetDesc(&previous);
        }
        if (!state_->latest || previous.Width != description.Width || previous.Height != description.Height) {
            state_->latest.Reset();
            for (auto& entry : state_->staging) {
                entry.Reset();
            }
            auto latestDescription = description;
            latestDescription.Usage = D3D11_USAGE_DEFAULT;
            latestDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            latestDescription.CPUAccessFlags = 0;
            latestDescription.MiscFlags = 0;
            require(state_->device->CreateTexture2D(&latestDescription, nullptr, &state_->latest), "Create latest desktop texture");
        }
        state_->context->CopyResource(state_->latest.Get(), source.Get());
    }
    if (info.LastMouseUpdateTime.QuadPart) {
        state_->pointerPosition = info.PointerPosition;
    }
    if (info.PointerShapeBufferSize) {
        if (info.PointerShapeBufferSize > 4 * 1024 * 1024) {
            throw std::runtime_error("Pointer shape exceeds limit");
        }
        state_->pointerShape.resize(info.PointerShapeBufferSize);
        UINT used = 0;
        require(state_->duplication->GetFramePointerShape(info.PointerShapeBufferSize, state_->pointerShape.data(), &used, &state_->pointerInfo), "GetFramePointerShape");
        state_->pointerShape.resize(used);
    }
    frame.pointerVisible = state_->pointerPosition.Visible != FALSE;
    frame.pointerX = state_->pointerPosition.Position.x;
    frame.pointerY = state_->pointerPosition.Position.y;
    frame.pointerShape = state_->pointerShape;
    frame.pointerType = state_->pointerInfo.Type;
    frame.pointerWidth = state_->pointerInfo.Width;
    frame.pointerHeight = state_->pointerInfo.Height;
    frame.pointerPitch = state_->pointerInfo.Pitch;
    frame.hotspotX = state_->pointerInfo.HotSpot.x;
    frame.hotspotY = state_->pointerInfo.HotSpot.y;
    if (frame.pointerVisible) {
        state_->cursorSuppression.restore();
    } else {
        state_->cursorSuppression.applyFallbackCursor(frame, state_->bounds.left, state_->bounds.top);
    }
    if (readPixels && (desktopUpdated || state_->deferred)) {
        const bool hadDeferred = state_->deferred.has_value();
        if (hadDeferred && !frame.captureTicks) {
            frame.captureTicks = state_->deferred->captureTicks;
        }
        const DirectPixelConsumer* activeDirect = (directConsumer && *directConsumer && !DesktopCursor::requiresBackground(frame)) ? directConsumer : nullptr;
        state_->copyPixels(source, &info, desktopUpdated && !hadDeferred && !geometryChanged, frame, reusablePixels, timing, &acquired, activeDirect);
        state_->deferred.reset();
        state_->pixelsCaptured = true;
        state_->lastPixelsWidth = frame.width;
        state_->lastPixelsHeight = frame.height;
    } else if (!readPixels && desktopUpdated) {
        // ReleaseFrame invalidates the acquired resource. Own exactly one GPU
        // snapshot before releasing it; replace raw pixels, never encoded AVC.
        state_->pixelsCaptured = false;
        state_->lastReadbackSerial = 0;
        state_->lastDirectSerial = 0;
        state_->lastReadbackData = nullptr;
    }
    if (!readPixels && (desktopUpdated || state_->deferred)) {
        const auto present = frame.captureTicks ? frame.captureTicks : state_->deferred ? state_->deferred->captureTicks
                                                                                        : 0;
        state_->deferred = frame;
        state_->deferred->captureTicks = present;
    }
    // Do not publish pixels from a desktop that changed during acquisition.
    if (requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
    }
    frame.stageTiming = timing->stages;
    return frame;
}
} // namespace ultraterimal::agent
