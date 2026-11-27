#include "platform/windows/desktop_tool.transport.h"

#include <algorithm>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio_bridge.h>
#include <blocking_pool.h>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include "features/desktop/png.protocol.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/desktop.transport.h"
#include "platform/windows/idd_topology.transport.h"
#include "platform/windows/input.transport.h"
#include "platform/windows/pipe.transport.h"

namespace ultraterimal::agent {
namespace {
using Microsoft::WRL::ComPtr;

struct DisplayBounds {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

wire::DesktopToolErrorCode mapCaptureError(wire::DesktopFault::Code code) noexcept {
    switch (code) {
    case wire::DesktopFault::DISPLAY_UNAVAILABLE:
    case wire::DesktopFault::DESKTOP_UNAVAILABLE:
        return wire::DESKTOP_TOOL_ERROR_UNAVAILABLE;
    case wire::DesktopFault::CAPTURE_BUSY:
        return wire::DESKTOP_TOOL_ERROR_BUSY;
    case wire::DesktopFault::CAPTURE_UNSUPPORTED:
        return wire::DESKTOP_TOOL_ERROR_UNSUPPORTED;
    default:
        return wire::DESKTOP_TOOL_ERROR_FAILED;
    }
}

DisplayBounds resolveAttachedDisplayBounds(std::uint32_t displayIndex) {
    (void)requireInputDesktop();
    if (displayIndex == 0) {
        (void)ensureIddPrimaryIfSoftwareDisplay();
    }
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        throw DesktopChanged("Cannot enumerate displays");
    }
    std::uint32_t attachedIndex = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto adapterStatus = factory->EnumAdapters1(i, &adapter);
        if (adapterStatus == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapterStatus)) {
            throw DesktopChanged("Display enumeration failed");
        }
        for (UINT j = 0;; ++j) {
            ComPtr<IDXGIOutput> output;
            const auto outputStatus = adapter->EnumOutputs(j, &output);
            if (outputStatus == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(outputStatus)) {
                throw DesktopChanged("Output enumeration failed");
            }
            DXGI_OUTPUT_DESC description{};
            if (FAILED(output->GetDesc(&description)) || !description.AttachedToDesktop) {
                continue;
            }
            if (attachedIndex++ == displayIndex) {
                const auto w = static_cast<std::int64_t>(description.DesktopCoordinates.right) - description.DesktopCoordinates.left;
                const auto h = static_cast<std::int64_t>(description.DesktopCoordinates.bottom) - description.DesktopCoordinates.top;
                if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
                    throw DesktopChanged("Invalid display bounds");
                }
                return {description.DesktopCoordinates.left, description.DesktopCoordinates.top, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
            }
        }
    }
    throw CaptureReset(wire::DesktopFault::DISPLAY_UNAVAILABLE);
}

std::int32_t mapCoordinate(std::int32_t value, std::uint32_t extent, bool normalized) noexcept {
    if (extent <= 1) {
        return 0;
    }
    if (!normalized) {
        return std::clamp<std::int32_t>(value, 0, static_cast<std::int32_t>(extent - 1));
    }
    const std::int64_t clamped = std::clamp<std::int64_t>(value, 0, protocol::kNormalizedCoordinateMax);
    const std::int64_t mapped = (clamped * static_cast<std::int64_t>(extent - 1) + 500) / protocol::kNormalizedCoordinateMax;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(mapped, 0, static_cast<std::int64_t>(extent - 1)));
}

void movePointer(DesktopInput& input, std::int32_t x, std::int32_t y) {
    wire::InputEvent ev;
    ev.set_kind(wire::InputEvent::POINTER_MOVE);
    ev.set_x(x);
    ev.set_y(y);
    input.apply(ev);
}

void sendMouseButton(DesktopInput& input, std::uint32_t button, bool pressed) {
    wire::InputEvent ev;
    ev.set_kind(wire::InputEvent::BUTTON);
    ev.set_code(button);
    ev.set_pressed(pressed);
    input.apply(ev);
}

void sendKey(DesktopInput& input, std::uint32_t vk, bool pressed) {
    wire::InputEvent ev;
    ev.set_kind(wire::InputEvent::KEY);
    ev.set_code(vk);
    ev.set_pressed(pressed);
    input.apply(ev);
}

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::invalid_argument("Text must be valid UTF-8");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed) != needed) {
        throw std::invalid_argument("Text conversion failed");
    }
    return out;
}

void sendUnicodeCodeUnit(wchar_t unit) {
    INPUT seq[2]{};
    seq[0].type = INPUT_KEYBOARD;
    seq[0].ki.wScan = static_cast<WORD>(unit);
    seq[0].ki.dwFlags = KEYEVENTF_UNICODE;
    seq[0].ki.dwExtraInfo = kDesktopInputMarker;
    seq[1].type = INPUT_KEYBOARD;
    seq[1].ki.wScan = static_cast<WORD>(unit);
    seq[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    seq[1].ki.dwExtraInfo = kDesktopInputMarker;
    if (SendInput(2, seq, sizeof(INPUT)) != 2) {
        throw std::runtime_error("Windows rejected Unicode text input");
    }
}
} // namespace

wire::DesktopSnapshotResult executeDesktopSnapshot(const wire::DesktopSnapshotStart& request) {
    wire::DesktopSnapshotResult result;
    result.set_snapshot_id(request.snapshot_id());
    if (!protocol::validDesktopSnapshotStart(request)) {
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_INVALID_REQUEST);
        return result;
    }

    const std::uint32_t targetMaxW = request.max_width() ? request.max_width() : 1280;
    const std::uint32_t targetMaxH = request.max_height() ? request.max_height() : 720;

    try {
        std::optional<CapturedFrame> captured;
        std::vector<std::uint8_t> reusable;
        for (int recreation = 0; recreation < 2 && !captured; ++recreation) {
            try {
                DesktopCapture capture(request.display_index());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
                while (std::chrono::steady_clock::now() < deadline) {
                    auto frame = capture.nextFrame(std::chrono::milliseconds(250), &reusable);
                    if (frame && !frame->bgra.empty()) {
                        captured = std::move(frame);
                        break;
                    }
                }
            } catch (const CaptureReset& reset) {
                if (recreation == 1) {
                    result.set_success(false);
                    result.set_error(mapCaptureError(reset.code()));
                    return result;
                }
            }
        }

        if (!captured) {
            result.set_success(false);
            result.set_error(wire::DESKTOP_TOOL_ERROR_TIMEOUT);
            return result;
        }

        std::uint32_t outW = 0;
        std::uint32_t outH = 0;
        auto png = encodeBgraPng(captured->bgra, captured->width, captured->height, targetMaxW, targetMaxH, outW, outH);
        if (png && png->size() > protocol::kMaximumSnapshotBytes) {
            const std::uint32_t reducedW = std::max<std::uint32_t>(320, (outW * 3) / 4);
            const std::uint32_t reducedH = std::max<std::uint32_t>(180, (outH * 3) / 4);
            png = encodeBgraPng(captured->bgra, captured->width, captured->height, reducedW, reducedH, outW, outH);
        }
        if (!png || png->size() > protocol::kMaximumSnapshotBytes) {
            result.set_success(false);
            result.set_error(wire::DESKTOP_TOOL_ERROR_FAILED);
            return result;
        }

        result.set_success(true);
        result.set_error(wire::DESKTOP_TOOL_ERROR_NONE);
        result.set_width(outW);
        result.set_height(outH);
        result.set_display_width(captured->width);
        result.set_display_height(captured->height);
        result.set_cursor_x(captured->pointerX);
        result.set_cursor_y(captured->pointerY);
        result.set_cursor_visible(captured->pointerVisible);
        result.set_png_data(std::move(*png));
        return result;
    } catch (const CaptureReset& reset) {
        result.set_success(false);
        result.set_error(mapCaptureError(reset.code()));
        return result;
    } catch (const DesktopChanged& changed) {
        result.set_success(false);
        result.set_error(mapCaptureError(changed.code()));
        return result;
    } catch (...) {
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_FAILED);
        return result;
    }
}

wire::DesktopActionResult executeDesktopAction(const wire::DesktopActionRequest& request) {
    wire::DesktopActionResult result;
    result.set_request_id(request.request_id() ? request.request_id() : 1);
    if (!protocol::validDesktopActionRequest(request)) {
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_INVALID_REQUEST);
        return result;
    }

    try {
        const auto bounds = resolveAttachedDisplayBounds(request.display_index());
        DesktopInput input(bounds.left, bounds.top, bounds.width, bounds.height);
        result.set_display_width(bounds.width);
        result.set_display_height(bounds.height);

        const std::int32_t x = mapCoordinate(request.x(), bounds.width, request.normalized());
        const std::int32_t y = mapCoordinate(request.y(), bounds.height, request.normalized());
        const std::int32_t toX = mapCoordinate(request.to_x(), bounds.width, request.normalized());
        const std::int32_t toY = mapCoordinate(request.to_y(), bounds.height, request.normalized());

        switch (request.action()) {
        case wire::DesktopActionRequest::ACTION_MOVE:
            movePointer(input, x, y);
            break;
        case wire::DesktopActionRequest::ACTION_CLICK:
            movePointer(input, x, y);
            sendMouseButton(input, request.button(), true);
            sendMouseButton(input, request.button(), false);
            break;
        case wire::DesktopActionRequest::ACTION_DOUBLE_CLICK:
            movePointer(input, x, y);
            sendMouseButton(input, request.button(), true);
            sendMouseButton(input, request.button(), false);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            sendMouseButton(input, request.button(), true);
            sendMouseButton(input, request.button(), false);
            break;
        case wire::DesktopActionRequest::ACTION_MOUSE_DOWN:
            movePointer(input, x, y);
            sendMouseButton(input, request.button(), true);
            break;
        case wire::DesktopActionRequest::ACTION_MOUSE_UP:
            movePointer(input, x, y);
            sendMouseButton(input, request.button(), false);
            break;
        case wire::DesktopActionRequest::ACTION_DRAG: {
            movePointer(input, x, y);
            sendMouseButton(input, request.button(), true);
            constexpr int kSteps = 8;
            for (int step = 1; step <= kSteps; ++step) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                const std::int32_t curX = x + ((toX - x) * step) / kSteps;
                const std::int32_t curY = y + ((toY - y) * step) / kSteps;
                movePointer(input, curX, curY);
            }
            sendMouseButton(input, request.button(), false);
            break;
        }
        case wire::DesktopActionRequest::ACTION_SCROLL: {
            movePointer(input, x, y);
            wire::InputEvent wheel;
            wheel.set_kind(wire::InputEvent::WHEEL);
            wheel.set_code(request.wheel_horizontal() ? 1u : 0u);
            wheel.set_wheel_delta(request.wheel_delta());
            input.apply(wheel);
            break;
        }
        case wire::DesktopActionRequest::ACTION_KEY: {
            const std::uint32_t vk = request.keys(0);
            sendKey(input, vk, true);
            sendKey(input, vk, false);
            break;
        }
        case wire::DesktopActionRequest::ACTION_HOTKEY: {
            for (int i = 0; i < request.keys_size(); ++i) {
                sendKey(input, request.keys(i), true);
            }
            for (int i = request.keys_size(); i-- > 0;) {
                sendKey(input, request.keys(i), false);
            }
            break;
        }
        case wire::DesktopActionRequest::ACTION_TYPE_TEXT: {
            const std::wstring wide = utf8ToWide(request.text());
            for (std::size_t i = 0; i < wide.size(); ++i) {
                const wchar_t ch = wide[i];
                if (ch == L'\r') {
                    if (i + 1 < wide.size() && wide[i + 1] == L'\n') {
                        ++i;
                    }
                    sendKey(input, VK_RETURN, true);
                    sendKey(input, VK_RETURN, false);
                } else if (ch == L'\n') {
                    sendKey(input, VK_RETURN, true);
                    sendKey(input, VK_RETURN, false);
                } else if (ch == L'\t') {
                    sendKey(input, VK_TAB, true);
                    sendKey(input, VK_TAB, false);
                } else {
                    sendUnicodeCodeUnit(ch);
                }
            }
            break;
        }
        default:
            result.set_success(false);
            result.set_error(wire::DESKTOP_TOOL_ERROR_INVALID_REQUEST);
            return result;
        }

        result.set_success(true);
        result.set_error(wire::DESKTOP_TOOL_ERROR_NONE);
        return result;
    } catch (const CaptureReset& reset) {
        result.set_success(false);
        result.set_error(mapCaptureError(reset.code()));
        return result;
    } catch (const DesktopChanged& changed) {
        result.set_success(false);
        result.set_error(mapCaptureError(changed.code()));
        return result;
    } catch (const std::invalid_argument&) {
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_INVALID_REQUEST);
        return result;
    } catch (const std::runtime_error& err) {
        result.set_success(false);
        const std::string_view what(err.what());
        if (what.find("write control is unavailable") != std::string_view::npos) {
            result.set_error(wire::DESKTOP_TOOL_ERROR_BUSY);
        } else {
            result.set_error(wire::DESKTOP_TOOL_ERROR_FAILED);
        }
        return result;
    } catch (...) {
        result.set_success(false);
        result.set_error(wire::DESKTOP_TOOL_ERROR_FAILED);
        return result;
    }
}

asio::awaitable<std::uint32_t> runDesktopToolWorker(ruvia::event_loop loop, NamedPipeTransport& pipe) {
    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(std::chrono::seconds(10));
    deadline.async_wait([&pipe](std::error_code error) {
        if (!error) {
            pipe.cancel();
        }
    });
    try {
        auto raw = co_await pipe.receive();
        deadline.cancel();
        auto decoded = protocol::decodeDesktopToolMessage(raw);
        if (!decoded) {
            co_return 1;
        }
        wire::DesktopToolMessage reply;
        if (decoded->has_snapshot_request()) {
            auto snapshot = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [req = decoded->snapshot_request()] {
                                 return executeDesktopSnapshot(req);
                             }))).take_value());
            *reply.mutable_snapshot_result() = std::move(snapshot);
        } else if (decoded->has_action_request()) {
            auto action = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop.worker(), [req = decoded->action_request()] {
                               return executeDesktopAction(req);
                           }))).take_value());
            *reply.mutable_action_result() = std::move(action);
        } else {
            co_return 1;
        }
        co_await pipe.send(reply.SerializeAsString());
        co_return 0;
    } catch (...) {
        deadline.cancel();
        co_return 1;
    }
}
} // namespace ultraterimal::agent
