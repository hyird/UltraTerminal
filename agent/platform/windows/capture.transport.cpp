#include "capture.transport.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "desktop.transport.h"
#include "handle.types.h"

namespace ultraterimal::agent {
namespace {
using Microsoft::WRL::ComPtr;
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
    DXGI_OUTDUPL_DESC description{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerInfo{};
    DXGI_OUTDUPL_POINTER_POSITION pointerPosition{};
    std::vector<std::uint8_t> pointerShape;
    RECT bounds{};
    std::wstring displayName;
    std::uint64_t desktopGeneration = 0;
    bool pixelsCaptured = false;
    void copyPixels(const ComPtr<ID3D11Texture2D>& source, CapturedFrame& frame, std::vector<std::uint8_t>* reusablePixels) {
        D3D11_TEXTURE2D_DESC textureDescription{};
        source->GetDesc(&textureDescription);
        const auto byteCount = static_cast<std::uint64_t>(textureDescription.Width) * textureDescription.Height * 4;
        if (!textureDescription.Width || !textureDescription.Height || byteCount > 64 * 1024 * 1024 || textureDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            textureDescription.Width != frame.width || textureDescription.Height != frame.height) {
            throw CaptureReset();
        }
        if (!staging) {
            auto stagingDescription = textureDescription;
            stagingDescription.BindFlags = 0;
            stagingDescription.MiscFlags = 0;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            require(device->CreateTexture2D(&stagingDescription, nullptr, &staging), "Create staging texture");
        }
        if (reusablePixels) {
            frame.bgra.swap(*reusablePixels);
        }
        frame.bgra.resize(static_cast<std::size_t>(byteCount));
        context->CopyResource(staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map desktop texture");
        {
            MappedTexture mapping{context.Get(), staging.Get()};
            const auto stride = static_cast<std::size_t>(frame.width) * 4;
            if (mapped.RowPitch < stride) {
                throw std::runtime_error("Invalid desktop row pitch");
            }
            for (std::size_t row = 0; row < frame.height; ++row) {
                std::memcpy(frame.bgra.data() + row * stride, static_cast<const std::uint8_t*>(mapped.pData) + row * mapped.RowPitch, stride);
            }
        }
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
std::optional<CapturedFrame> DesktopCapture::nextFrame(std::chrono::milliseconds timeout, std::vector<std::uint8_t>* reusablePixels) {
    if (timeout.count() < 0 || timeout.count() > 5000) {
        throw std::invalid_argument("Capture wait must be between 0 and 5000 milliseconds");
    }
    if (requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const auto result = state_->duplication->AcquireNextFrame(static_cast<UINT>(timeout.count()), &info, &resource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
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
    if (!state_->pixelsCaptured || info.LastPresentTime.QuadPart != 0) {
        state_->copyPixels(source, frame, reusablePixels);
        state_->pixelsCaptured = true;
    }
    // The encoder consumes complete snapshots and performs its own inter-frame
    // prediction. DXGI dirty/move lists are not needed for this capture path.
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
    // Do not publish pixels from a desktop that changed during acquisition.
    if (requireInputDesktop() != state_->desktopGeneration) {
        throw CaptureReset();
    }
    return frame;
}
} // namespace ultraterimal::agent
