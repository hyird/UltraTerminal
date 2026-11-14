#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <d3d11.h>
#include <ruvia/core/blocking_pool.h>
#include <ruvia/core/event_loop.h>

#include "capture.fixture.h"
#include "config/command_line.config.h"
#include "platform/windows/desktop.transport.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/pipe.transport.h"

namespace {
using namespace ultraterimal::agent;
using Microsoft::WRL::ComPtr;
void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
void gpu(HRESULT result) { require(SUCCEEDED(result), "DXGI cursor probe GPU operation failed"); }
struct CursorRestore {
    POINT original{};
    CursorRestore() { require(GetCursorPos(&original), "Cannot read original cursor position"); }
    ~CursorRestore() { SetCursorPos(original.x, original.y); }
};
std::string inspectPixels() {
    requireInputDesktop();
    ultraterimal::test::TestSurface surface;
    CursorRestore restore;
    ComPtr<IDXGIFactory1> factory;
    gpu(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> selected;
    for (UINT i = 0; !selected && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        for (UINT j = 0;; ++j) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(j, &output) != S_OK) {
                break;
            }
            DXGI_OUTPUT_DESC description{};
            gpu(output->GetDesc(&description));
            if (description.AttachedToDesktop) {
                gpu(output.As(&selected));
                break;
            }
        }
    }
    require(bool(selected), "No attached DXGI output");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    gpu(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<IDXGIOutputDuplication> duplication;
    gpu(selected->DuplicateOutput(device.Get(), &duplication));
    ComPtr<ID3D11Texture2D> staging;
    const auto pointer = surface.cursorPosition();
    std::string result = "PASS: raw DXGI cursor diagnostic";
    const COLORREF colors[]{RGB(237, 19, 71), RGB(17, 211, 139), RGB(29, 101, 223), RGB(211, 181, 43)};
    for (unsigned trial = 0; trial < 4; ++trial) {
        const auto color = colors[trial];
        const bool overPattern = trial >= 2;
        const POINT position{pointer.x + (overPattern ? 0 : 128), pointer.y};
        require(SetCursorPos(position.x, position.y), "Cannot position diagnostic cursor");
        surface.paint(color);
        bool sampled = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!sampled && std::chrono::steady_clock::now() < deadline) {
            surface.paint(color);
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            const auto status = duplication->AcquireNextFrame(100, &info, &resource);
            if (status == DXGI_ERROR_WAIT_TIMEOUT) {
                continue;
            }
            gpu(status);
            struct Release {
                IDXGIOutputDuplication* value;
                ~Release() { value->ReleaseFrame(); }
            } release{duplication.Get()};
            ComPtr<ID3D11Texture2D> texture;
            gpu(resource.As(&texture));
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            require(description.Width >= 80 && description.Height >= 80 && description.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
                    "Unexpected diagnostic texture geometry");
            if (!staging) {
                description.BindFlags = 0;
                description.MiscFlags = 0;
                description.Usage = D3D11_USAGE_STAGING;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                gpu(device->CreateTexture2D(&description, nullptr, &staging));
            }
            context->CopyResource(staging.Get(), texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            gpu(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            unsigned matched = 0;
            const auto* pixels = static_cast<const unsigned char*>(mapped.pData);
            for (unsigned y = 40; y < 80; ++y) {
                for (unsigned x = 40; x < 80; ++x) {
                    const auto* pixel = pixels + static_cast<std::size_t>(y) * mapped.RowPitch + x * 4;
                    matched += pixel[0] == GetBValue(color) && pixel[1] == GetGValue(color) && pixel[2] == GetRValue(color);
                }
            }
            context->Unmap(staging.Get(), 0);
            if (matched < 800) {
                continue; // A previously queued compositor color is not evidence.
            }
            CURSORINFO cursor{sizeof(CURSORINFO)};
            require(GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) && cursor.hCursor &&
                        cursor.ptScreenPos.x == position.x && cursor.ptScreenPos.y == position.y,
                    "Diagnostic cursor visibility or position changed");
            if (!overPattern) {
                require(matched == 1600, "Unobstructed DXGI test pattern differs");
            }
            result += "\ntrial=" + std::to_string(trial) + " cursor_over_pattern=" + std::to_string(overPattern) +
                      " changed_pixels=" + std::to_string(1600 - matched) +
                      " separate_pointer_visible=" + std::to_string(info.PointerPosition.Visible != FALSE) +
                      " pointer_shape_bytes=" + std::to_string(info.PointerShapeBufferSize) +
                      " desktop_present=" + std::to_string(info.LastPresentTime.QuadPart != 0);
            sampled = true;
        }
        require(sampled, "DXGI compositor test pattern was not observed");
    }
    return result;
}
asio::awaitable<void> child(ruvia::event_loop loop, const CommandLine& options) {
    DesktopParent parent(options.windowsSession, options.parentProcess);
    auto pipe = NamedPipeTransport::connect(loop.executor(), options.pipeName, options.parentProcess);
    std::string result;
    try {
        result = inspectPixels();
    } catch (const std::exception& error) {
        result = error.what();
    }
    co_await pipe->send(std::move(result));
    (void)co_await pipe->receive();
}
asio::awaitable<void> parent(ruvia::event_loop loop, std::filesystem::path executable) {
    DesktopIdentity identity(currentDesktopSession());
    NamedPipeTransport pipe(loop.executor(), identity.pipeSid());
    DesktopProcess process(loop.executor(), identity, executable, pipe.name());
    asio::steady_timer deadline(loop.executor(), std::chrono::seconds(15));
    deadline.async_wait([&](std::error_code error) { if (!error) { pipe.cancel(); process.terminate(); } });
    try {
        co_await pipe.accept(process.id(), identity.session());
        const auto result = co_await pipe.receive();
        co_await pipe.send("ack");
        const auto code = co_await process.wait();
        deadline.cancel();
        require(code == 0 && result.starts_with("PASS:"), result.c_str());
        std::cout << result << '\n';
    } catch (...) {
        deadline.cancel();
        throw;
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        require(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr, "Cannot select physical cursor probe coordinates");
        std::vector<std::wstring_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        const auto options = parseCommandLine(arguments);
        if (!options || (argc > 1 && options->command != Command::kDesktop)) {
            return 2;
        }
        asio::io_context io;
        ruvia::blocking_pool blockingPool{{.thread_count_ = 1, .queue_capacity_ = 4}};
        auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &blockingPool});
        std::exception_ptr failure;
        asio::co_spawn(attachment.loop().executor(), argc == 1 ? parent(attachment.loop(), std::filesystem::absolute(argv[0])) : child(attachment.loop(), *options),
                       [&](std::exception_ptr error) { failure = error; attachment.stop(); });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
