#include <iostream>
#include <stdexcept>

#include "ultraterimal/protocol/desktop_ipc.protocol.h"

namespace {
using namespace ultraterimal;
void require(bool value) {
    if (!value) {
        throw std::runtime_error("Desktop IPC invariant failed");
    }
}
wire::Envelope message(std::uint64_t sequence, const char* channel = "") {
    wire::Envelope value;
    value.set_protocol_version(protocol::kVersion);
    value.set_session_id("session");
    value.set_sequence(sequence);
    value.set_channel_id(channel);
    return value;
}
} // namespace
int main() {
    try {
        protocol::DesktopPipeValidator service({"session", 7, false, protocol::DesktopPipeSource::kService});
        auto start = message(1);
        start.mutable_desktop_start()->set_windows_session_id(8);
        require(!service.accept(start));
        start.mutable_desktop_start()->set_windows_session_id(7);
        start.mutable_desktop_start()->set_write_control(true);
        require(!service.accept(start));
        start.mutable_desktop_start()->set_write_control(false);
        require(service.accept(start).has_value());
        require(!service.accept(start));
        protocol::DesktopPipeValidator writable({"session", 7, true, protocol::DesktopPipeSource::kService});
        auto writableStart = start;
        writableStart.mutable_desktop_start()->set_write_control(true);
        require(!writable.accept(writableStart)); // Missing negotiated input contract.
        writableStart.mutable_desktop_start()->set_desktop_input_contract(1);
        require(!writable.accept(writableStart)); // Old Workers must not silently accept v1.
        writableStart.mutable_desktop_start()->set_desktop_input_contract(2);
        require(writable.accept(writableStart).has_value());
        auto browserProbe = message(2, "input");
        browserProbe.mutable_desktop_probe()->set_nonce(2);
        browserProbe.mutable_desktop_probe()->set_reset_input(true);
        require(!writable.accept(browserProbe));
        browserProbe.mutable_desktop_probe()->set_reset_input(false);
        browserProbe.mutable_desktop_probe()->set_accepted_response_nonce(1);
        require(!writable.accept(browserProbe));
        browserProbe.mutable_desktop_probe()->set_accepted_response_nonce(0);
        require(writable.accept(browserProbe).has_value());
        auto input = message(2, "input");
        input.set_generation(1);
        input.mutable_input_event()->set_kind(wire::InputEvent::KEY);
        input.mutable_input_event()->set_code(65);
        require(!service.accept(input));
        input.mutable_request_refresh();
        require(service.accept(input).has_value());
        auto ticket = message(3, "input");
        ticket.mutable_channel_open()->set_purpose(wire::DESKTOP_INPUT);
        ticket.mutable_channel_open()->set_ticket("not-an-ipc-field");
        require(!service.accept(ticket));
        protocol::DesktopPipeValidator desktop({"session", 7, false, protocol::DesktopPipeSource::kDesktop});
        require(!desktop.accept(start));
        auto ready = message(1);
        ready.mutable_desktop_ready();
        require(desktop.accept(ready).has_value());
        protocol::DesktopPipeValidator sasDesktop({"session", 7, true, protocol::DesktopPipeSource::kDesktop});
        require(sasDesktop.accept(ready).has_value());
        auto sas = message(2, "input");
        sas.set_generation(1);
        sas.mutable_input_event()->set_kind(wire::InputEvent::SECURE_ATTENTION);
        require(!desktop.accept(sas));
        require(sasDesktop.accept(sas).has_value());
        require(!sasDesktop.accept(sas));
        sas.set_sequence(3);
        sas.mutable_input_event()->set_kind(wire::InputEvent::KEY);
        sas.mutable_input_event()->set_code(65);
        require(!sasDesktop.accept(sas));
        auto config = message(2, "video");
        config.set_generation(1);
        config.mutable_video_config()->set_codec("avc1.42C01E");
        config.mutable_video_config()->set_width(32);
        config.mutable_video_config()->set_height(32);
        require(desktop.accept(config).has_value());
        require(!service.accept(config));
        auto cursor = message(2, "input");
        cursor.mutable_cursor_update();
        require(!desktop.accept(cursor)); // The sequence is shared across input/video on one pipe.
        cursor.set_sequence(3);
        require(desktop.accept(cursor).has_value());
        auto stop = message(4);
        stop.mutable_session_close()->set_reason("completed");
        require(desktop.accept(stop).has_value());
        cursor.set_sequence(5);
        require(!desktop.accept(cursor));
        protocol::DesktopPipeValidator failed({"session", 7, false, protocol::DesktopPipeSource::kDesktop});
        auto error = message(1);
        error.mutable_session_error()->set_code("capture_unavailable");
        require(failed.accept(error).has_value());
        require(!failed.accept(ready));
        std::cout << "Desktop IPC startup binding, direction, no tickets, shared sequencing and terminal state passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
