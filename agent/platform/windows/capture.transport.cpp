#include "capture.transport.h"

#include <algorithm>
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

#include "capture_regions.protocol.h"
#include "desktop.transport.h"
#include "handle.types.h"

namespace ultraterimal::agent {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
void require(HRESULT status, const char* operation) {
    if (status == DXGI_ERROR_ACCESS_LOST || status == DXGI_ERROR_DEVICE_REMOVED || status == DXGI_ERROR_DEVICE_RESET || status == DXGI_ERROR_SESSION_DISCONNECTED) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_DEVICE_LOST);
    }
    if (FAILED(status)) {
        throw std::system_error(static_cast<int>(status), std::system_category(), operation);
    }
}
struct AcquiredFrame {
    IDXGIOutputDuplication* duplication;
    ~AcquiredFrame() { duplication->ReleaseFrame(); }
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
        if (!request_ || !PowerSetRequest(request_.get(), PowerRequestSystemRequired) ||
            !PowerSetRequest(request_.get(), PowerRequestDisplayRequired) || !SetThreadExecutionState(ES_DISPLAY_REQUIRED)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Keep remote desktop display available");
        }
    }
    ~DisplayActivity() {
        PowerClearRequest(request_.get(), PowerRequestDisplayRequired);
        PowerClearRequest(request_.get(), PowerRequestSystemRequired);
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
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Texture2D> latest;
    std::optional<CapturedFrame> deferred;
    DXGI_OUTDUPL_DESC description{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerInfo{};
    DXGI_OUTDUPL_POINTER_POSITION pointerPosition{};
    std::vector<std::uint8_t> pointerShape;
    RECT bounds{};
    std::wstring displayName;
    std::uint64_t desktopGeneration = 0;
    bool pixelsCaptured = false;
    std::uint32_t lastPixelsWidth = 0, lastPixelsHeight = 0;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> moved;
    std::vector<RECT> dirty, regions;
    bool changedRegions(const DXGI_OUTDUPL_FRAME_INFO& info, const CapturedFrame& frame, CaptureTiming* timing) {
        constexpr UINT maximumMetadata = 1024 * 1024;
        if (!info.TotalMetadataBufferSize || info.TotalMetadataBufferSize > maximumMetadata) {
            return false;
        }
        const auto started = timing ? Clock::now() : Clock::time_point{};
        const auto moveCapacity = (info.TotalMetadataBufferSize + sizeof(DXGI_OUTDUPL_MOVE_RECT) - 1) / sizeof(DXGI_OUTDUPL_MOVE_RECT);
        moved.resize(moveCapacity);
        UINT moveBytes = 0;
        const auto moveResult = duplication->GetFrameMoveRects(static_cast<UINT>(moved.size() * sizeof(DXGI_OUTDUPL_MOVE_RECT)), moved.data(), &moveBytes);
        if (moveResult == DXGI_ERROR_MORE_DATA) {
            return false;
        }
        require(moveResult, "GetFrameMoveRects");
        if (moveBytes > info.TotalMetadataBufferSize || moveBytes % sizeof(DXGI_OUTDUPL_MOVE_RECT)) {
            return false;
        }
        moved.resize(moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT));
        const auto dirtyCapacity = std::max<std::size_t>(1, (info.TotalMetadataBufferSize - moveBytes + sizeof(RECT) - 1) / sizeof(RECT));
        dirty.resize(dirtyCapacity);
        UINT dirtyBytes = 0;
        const auto dirtyResult = duplication->GetFrameDirtyRects(static_cast<UINT>(dirty.size() * sizeof(RECT)), dirty.data(), &dirtyBytes);
        if (dirtyResult == DXGI_ERROR_MORE_DATA) {
            return false;
        }
        require(dirtyResult, "GetFrameDirtyRects");
        if (dirtyBytes > info.TotalMetadataBufferSize - moveBytes || dirtyBytes % sizeof(RECT)) {
            return false;
        }
        dirty.resize(dirtyBytes / sizeof(RECT));
        if (moved.size() + dirty.size() > 1024) {
            return false;
        }
        const auto area = collectCaptureRegions(frame.width, frame.height, moved, dirty, regions);
        if (!area) {
            return false;
        }
        if (timing) {
            timing->metadataMilliseconds = milliseconds(started, Clock::now());
            timing->movedRectangles = static_cast<std::uint32_t>(moved.size());
            timing->dirtyRectangles = static_cast<std::uint32_t>(dirty.size());
            timing->changedPixels = *area;
        }
        // A full copy is cheaper than many overlapping or large rectangles.
        return *area > 0 && *area < static_cast<std::uint64_t>(frame.width) * frame.height / 2;
    }
    void copyStagingPixels(const ComPtr<ID3D11Texture2D>& source, const D3D11_TEXTURE2D_DESC& textureDescription, CapturedFrame& frame, CaptureTiming* timing, const std::vector<RECT>* changed = nullptr) {
        if (!staging) {
            auto stagingDescription = textureDescription;
            stagingDescription.BindFlags = 0;
            stagingDescription.MiscFlags = 0;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            require(device->CreateTexture2D(&stagingDescription, nullptr, &staging), "Create staging texture");
        }
        const auto copyStarted = timing ? Clock::now() : Clock::time_point{};
        context->CopyResource(staging.Get(), source.Get());
        const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map desktop texture");
        const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
        {
            MappedTexture mapping{context.Get(), staging.Get()};
            copyCapturePixels(frame.bgra, frame.width, frame.height, mapped.pData, mapped.RowPitch, changed ? std::span<const RECT>(*changed) : std::span<const RECT>{});
            if (timing && timing->verifyWithStaging) {
                if (changed) {
                    std::vector<std::uint8_t> reference(frame.bgra.size());
                    copyCapturePixels(reference, frame.width, frame.height, mapped.pData, mapped.RowPitch);
                    timing->matchedStaging = reference == frame.bgra;
                } else {
                    timing->matchedStaging = true;
                }
                if (!timing->matchedStaging) {
                    throw std::runtime_error("DXGI staging partial readback disagrees with its complete frame");
                }
            }
        }
        if (timing) {
            timing->copyDispatchMilliseconds = milliseconds(copyStarted, mapStarted);
            timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
            timing->rowCopyMilliseconds = milliseconds(rowsStarted, Clock::now());
        }
    }
    void copyPixels(const ComPtr<ID3D11Texture2D>& source, const DXGI_OUTDUPL_FRAME_INFO& info, CapturedFrame& frame, std::vector<std::uint8_t>* reusablePixels, CaptureTiming* timing, bool previousPixels) {
        D3D11_TEXTURE2D_DESC textureDescription{};
        source->GetDesc(&textureDescription);
        const auto byteCount = static_cast<std::uint64_t>(textureDescription.Width) * textureDescription.Height * 4;
        if (!textureDescription.Width || !textureDescription.Height || byteCount > 64 * 1024 * 1024 || textureDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            textureDescription.Width != frame.width || textureDescription.Height != frame.height) {
            throw CaptureReset();
        }
        const auto partial = previousPixels && reusablePixels && pixelsCaptured &&
                             lastPixelsWidth == frame.width && lastPixelsHeight == frame.height && reusablePixels->size() == byteCount &&
                             changedRegions(info, frame, timing);
        if (reusablePixels) {
            frame.bgra.swap(*reusablePixels);
        }
        frame.bgra.resize(static_cast<std::size_t>(byteCount));
        if (timing) {
            timing->partialReadback = partial;
        }
        const auto* changed = partial ? &regions : nullptr;
        // DXGI can expose the duplicated desktop in system memory directly.
        // Avoid a redundant GPU copy and a blocking staging-texture Map in that case.
        if (description.DesktopImageInSystemMemory && !(timing && timing->forceStaging)) {
            DXGI_MAPPED_RECT mapped{};
            const auto mapStarted = timing ? Clock::now() : Clock::time_point{};
            require(duplication->MapDesktopSurface(&mapped), "Map desktop surface");
            const auto rowsStarted = timing ? Clock::now() : Clock::time_point{};
            {
                MappedDesktop mapping{duplication.Get()};
                if (mapped.Pitch < 0) {
                    throw std::runtime_error("Invalid desktop row pitch");
                }
                copyCapturePixels(frame.bgra, frame.width, frame.height, mapped.pBits, static_cast<std::size_t>(mapped.Pitch), changed ? std::span<const RECT>(*changed) : std::span<const RECT>{});
            }
            if (timing) {
                timing->mapMilliseconds = milliseconds(mapStarted, rowsStarted);
                timing->rowCopyMilliseconds = milliseconds(rowsStarted, Clock::now());
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
            return;
        }
        copyStagingPixels(source, textureDescription, frame, timing, changed);
    }
};
DesktopCapture::DesktopCapture(std::uint32_t displayIndex) : state_(std::make_unique<State>()) {
    state_->desktopGeneration = requireInputDesktop();
    // Idle display power saving can leave DuplicateOutput waiting indefinitely
    // for its first frame. The request lives exactly as long as this capture.
    state_->activity.emplace();
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
        throw DesktopFailure(wire::DesktopFault::DISPLAY_UNAVAILABLE, "Requested desktop display is unavailable");
    }
    DXGI_OUTPUT_DESC outputDescription{};
    require(selected->GetDesc(&outputDescription), "Get selected desktop bounds");
    state_->bounds = outputDescription.DesktopCoordinates;
    state_->displayName = outputDescription.DeviceName;
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
    const auto modes = displayModes();
    if (std::ranges::none_of(modes, [=](const auto& mode) { return mode.width == width && mode.height == height; })) {
        return false;
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
std::optional<CapturedFrame> DesktopCapture::nextFrame(std::chrono::milliseconds timeout, std::vector<std::uint8_t>* reusablePixels, CaptureTiming* timing, bool previousPixels, bool readPixels) {
    if (timeout.count() < 0 || timeout.count() > 5000) {
        throw std::invalid_argument("Capture wait must be between 0 and 5000 milliseconds");
    }
    if (requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
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
        timing->acquireMilliseconds = milliseconds(acquireStarted, Clock::now());
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
            state_->pixelsCaptured = true;
            state_->lastPixelsWidth = frame.width;
            state_->lastPixelsHeight = frame.height;
            if (requireInputDesktop() != state_->desktopGeneration) {
                throw CaptureReset();
            }
            return frame;
        }
        return std::nullopt;
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
    const bool desktopUpdated = !state_->pixelsCaptured || info.LastPresentTime.QuadPart != 0;
    if (readPixels && (desktopUpdated || state_->deferred)) {
        if (state_->deferred && !frame.captureTicks) {
            frame.captureTicks = state_->deferred->captureTicks;
        }
        state_->copyPixels(source, info, frame, reusablePixels, timing, previousPixels && !state_->deferred);
        state_->deferred.reset();
        state_->pixelsCaptured = true;
        state_->lastPixelsWidth = frame.width;
        state_->lastPixelsHeight = frame.height;
    } else if (!readPixels && desktopUpdated) {
        // ReleaseFrame invalidates the acquired resource. Own exactly one GPU
        // snapshot before releasing it; replace raw pixels, never encoded AVC.
        if (!state_->latest) {
            auto latestDescription = description;
            latestDescription.Usage = D3D11_USAGE_DEFAULT;
            latestDescription.BindFlags = 0;
            latestDescription.CPUAccessFlags = 0;
            latestDescription.MiscFlags = 0;
            require(state_->device->CreateTexture2D(&latestDescription, nullptr, &state_->latest), "Create latest desktop texture");
        }
        state_->context->CopyResource(state_->latest.Get(), source.Get());
        state_->pixelsCaptured = false;
    }
    // Always return a complete snapshot for the video encoder; move/dirty
    // metadata only reduces CPU readback when the prior pixels are available.
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
    return frame;
}
} // namespace ultraterimal::agent
