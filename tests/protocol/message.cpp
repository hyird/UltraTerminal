#include <iostream>
#include <stdexcept>

#include "ultraterimal/protocol/message.protocol.h"

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
int main() {
    using namespace ultraterimal;
    try {
        wire::Envelope hello;
        hello.set_protocol_version(1);
        hello.set_sequence(1);
        hello.mutable_hello()->set_device_id("test-device");
        hello.mutable_hello()->set_agent_version("0.1.0");
        const auto bytes = protocol::encodeMessage(hello);
        require(bytes.has_value(), "Valid hello rejected");
        auto decoded = protocol::decodeMessage(*bytes);
        require(decoded && decoded->hello().device_id() == "test-device", "Round trip failed");
        auto future = *bytes;
        future.append("\xa0\x06\x01", 3);
        require(protocol::decodeMessage(future).has_value(), "Unknown optional field broke compatibility");
        hello.mutable_hello()->mutable_capabilities()->set_desktop_input_contract(3);
        require(!protocol::encodeMessage(hello), "Unsupported input contract advertised");
        hello.mutable_hello()->mutable_capabilities()->set_desktop_input_contract(1);
        require(protocol::encodeMessage(hello).has_value(), "Legacy capability broke envelope compatibility");
        hello.mutable_hello()->mutable_capabilities()->set_desktop_input_contract(2);
        require(protocol::encodeMessage(hello).has_value(), "Negotiated input capability rejected");
        hello.set_protocol_version(2);
        require(!protocol::encodeMessage(hello), "Unsupported version accepted");
        require(!protocol::decodeMessage(std::string("\x52\xff\xff\xff\xff\x7f", 6)), "Invalid length accepted");
        require(!protocol::decodeMessage(std::string(protocol::kMaximumMessageBytes + 1, 'x')), "Oversized message accepted");
        wire::Envelope resize;
        resize.set_protocol_version(1);
        resize.set_sequence(1);
        resize.set_session_id("session");
        resize.mutable_terminal_resize()->set_columns(80);
        resize.mutable_terminal_resize()->set_rows(25);
        require(protocol::validateMessage(resize).has_value(), "Valid resize rejected");
        resize.mutable_terminal_resize()->set_columns(0);
        require(!protocol::validateMessage(resize), "Zero terminal size accepted");
        require(!protocol::purposeMatches(wire::SSH, wire::DESKTOP_INPUT), "Cross-protocol channel accepted");
        require(protocol::purposeMatches(wire::SSH, wire::SSH_STREAM), "SSH channel rejected");
        wire::Envelope open;
        open.set_protocol_version(1);
        open.set_sequence(1);
        open.set_session_id("session");
        auto* session = open.mutable_session_open();
        session->set_kind(wire::DESKTOP);
        session->set_user_id("user");
        require(!protocol::validateMessage(open), "Session without channel grants accepted");
        for (auto purpose : {wire::DESKTOP_INPUT, wire::DESKTOP_VIDEO}) {
            auto* grant = session->add_channels();
            grant->set_channel_id(purpose == wire::DESKTOP_INPUT ? "input" : "video");
            grant->set_purpose(purpose);
            grant->set_ticket("test-ticket");
        }
        require(protocol::validateMessage(open).has_value(), "Valid session grants rejected");
        session->set_write_control(true);
        require(!protocol::validateMessage(open), "Writable desktop without input contract accepted");
        session->set_desktop_input_contract(1);
        require(!protocol::validateMessage(open), "Legacy writable input contract accepted");
        session->set_desktop_input_contract(2);
        require(protocol::validateMessage(open).has_value(), "Negotiated desktop input contract rejected");
        session->set_write_control(false);
        require(!protocol::validateMessage(open), "Read-only desktop carrying a writable input contract accepted");
        session->set_desktop_input_contract(0);
        session->mutable_channels(1)->set_purpose(wire::DESKTOP_INPUT);
        require(!protocol::validateMessage(open), "Duplicate channel purpose accepted");
        session->mutable_channels(1)->set_purpose(wire::DESKTOP_VIDEO);
        session->mutable_channels(1)->set_channel_id("input");
        require(!protocol::validateMessage(open), "Duplicate channel identity accepted");
        session->mutable_channels(1)->set_channel_id("video");
        session->mutable_channels(1)->set_purpose(wire::SSH_STREAM);
        require(!protocol::validateMessage(open), "Cross-protocol session grant accepted");
        wire::Envelope ready;
        ready.set_protocol_version(1);
        ready.set_sequence(1);
        ready.set_session_id("session");
        ready.set_channel_id("input");
        ready.mutable_channel_ready()->set_purpose(wire::DESKTOP_INPUT);
        require(protocol::validateMessage(ready).has_value(), "Valid channel readiness rejected");
        ready.set_sequence(2);
        require(!protocol::validateMessage(ready), "Channel readiness with a data sequence accepted");
        ready.set_sequence(1);
        ready.mutable_channel_ready()->set_purpose(wire::SSH_STREAM);
        require(!protocol::validateMessage(ready), "Non-desktop readiness accepted");
        wire::Envelope frame;
        frame.set_protocol_version(1);
        frame.set_sequence(2);
        frame.set_session_id("session");
        frame.set_generation(1);
        frame.set_frame_id(1);
        frame.mutable_full_frame()->set_width(16384);
        frame.mutable_full_frame()->set_height(16384);
        require(!protocol::validateMessage(frame), "Invalid image storage accepted");
        wire::InputEvent input;
        require(!protocol::validateInputEvent(input), "Unspecified input accepted");
        input.set_kind(wire::InputEvent::POINTER_MOVE);
        input.set_x(16383);
        require(protocol::validateInputEvent(input).has_value(), "Valid pointer rejected");
        input.set_x(16384);
        require(!protocol::validateInputEvent(input), "Pointer bound accepted");
        input.set_x(-1);
        require(!protocol::validateInputEvent(input), "Negative pointer accepted");
        input.Clear();
        input.set_kind(wire::InputEvent::KEY);
        for (auto code : {0x41U, 0xa0U, 0xa1U, 0xa2U, 0xa3U, 0x87U, 0x10dU}) {
            input.set_code(code);
            require(protocol::validateInputEvent(input).has_value(), "Valid key rejected");
        }
        for (auto code : {0U, 1U, 0x10U, 0x11U, 0x12U, 0xe5U, 0xe7U, 0x100U, 0xffffffffU}) {
            input.set_code(code);
            require(!protocol::validateInputEvent(input), "Invalid key accepted");
        }
        input.Clear();
        input.set_kind(wire::InputEvent::WHEEL);
        require(!protocol::validateInputEvent(input), "Empty wheel event accepted");
        input.set_code(1);
        input.set_wheel_delta(-120);
        require(protocol::validateInputEvent(input).has_value(), "Horizontal wheel rejected");
        input.set_wheel_delta(12001);
        require(!protocol::validateInputEvent(input), "Excessive wheel delta accepted");
        input.Clear();
        input.set_kind(wire::InputEvent::BUTTON);
        input.set_code(4);
        require(protocol::validateInputEvent(input).has_value(), "Forward button rejected");
        input.set_code(5);
        require(!protocol::validateInputEvent(input), "Unknown button accepted");
        input.Clear();
        input.set_kind(wire::InputEvent::RELEASE_ALL);
        require(protocol::validateInputEvent(input).has_value(), "Release all rejected");
        input.set_pressed(true);
        require(!protocol::validateInputEvent(input), "Noncanonical release accepted");
        wire::Envelope login;
        login.set_protocol_version(protocol::kVersion);
        login.set_sequence(1);
        auto* account = login.mutable_windows_login_request();
        account->set_request_id(1);
        account->set_action(wire::WindowsLoginRequest::READ);
        require(protocol::encodeMessage(login).has_value(), "Windows settings read rejected");
        account->set_password("must-not-be-sent-on-read");
        require(!protocol::encodeMessage(login), "Read accepted a password payload");
        account->set_action(wire::WindowsLoginRequest::SAVE);
        account->set_username("account");
        require(protocol::encodeMessage(login).has_value(), "Windows settings save rejected");
        account->clear_password();
        require(protocol::encodeMessage(login).has_value(), "Partial credential update rejected");
        account->clear_username();
        require(protocol::encodeMessage(login).has_value(), "Empty credential update rejected");
        login.set_session_id("desktop");
        login.set_channel_id("input");
        require(!protocol::encodeMessage(login), "Windows credentials allowed on desktop channel");
        login.clear_session_id();
        login.clear_channel_id();
        account->set_password(std::string(1025, 'p'));
        require(!protocol::encodeMessage(login), "Unbounded password accepted");
        account->set_password(std::string("p\0w", 3));
        require(!protocol::encodeMessage(login), "Embedded NUL password accepted");
        const std::string operationId(64, 'a');
        const std::string deviceId = "d-" + std::string(64, 'b');
        wire::Envelope restart;
        restart.set_protocol_version(protocol::kVersion);
        restart.set_sequence(1);
        auto* restartRequest = restart.mutable_windows_restart_request();
        restartRequest->set_operation_id(operationId);
        restartRequest->set_device_id(deviceId);
        restartRequest->set_expires_at_unix_seconds(2000000000);
        auto restartBytes = protocol::encodeMessage(restart);
        require(restartBytes.has_value(), "Valid restart request rejected");
        auto restartDecoded = protocol::decodeMessage(*restartBytes);
        require(restartDecoded && restartDecoded->windows_restart_request().operation_id() == operationId &&
                    restartDecoded->windows_restart_request().device_id() == deviceId &&
                    restartDecoded->windows_restart_request().expires_at_unix_seconds() == 2000000000,
                "Restart request round trip failed");
        restart.set_session_id("session");
        require(!protocol::encodeMessage(restart), "Restart request allowed on a session");
        restart.clear_session_id();
        restart.set_channel_id("control");
        require(!protocol::encodeMessage(restart), "Restart request allowed on a channel");
        restart.clear_channel_id();
        restartRequest->clear_operation_id();
        require(!protocol::encodeMessage(restart), "Empty restart operation id accepted");
        restartRequest->set_operation_id(operationId);
        restart.mutable_windows_restart_result()->set_operation_id(operationId);
        restart.mutable_windows_restart_result()->set_status(wire::WindowsRestartResult::ACCEPTED);
        restartBytes = protocol::encodeMessage(restart);
        require(restartBytes.has_value(), "Valid restart result rejected");
        restartDecoded = protocol::decodeMessage(*restartBytes);
        require(restartDecoded && restartDecoded->windows_restart_result().status() == wire::WindowsRestartResult::ACCEPTED,
                "Restart result round trip failed");
        auto* restartResult = restart.mutable_windows_restart_result();
        restart.set_session_id("session");
        require(!protocol::encodeMessage(restart), "Restart result allowed on a session");
        restart.clear_session_id();
        restart.set_channel_id("control");
        require(!protocol::encodeMessage(restart), "Restart result allowed on a channel");
        restart.clear_channel_id();
        restartResult->set_status(wire::WindowsRestartResult::UNSPECIFIED);
        require(!protocol::encodeMessage(restart), "Unspecified restart status accepted");
        restartResult->set_status(static_cast<wire::WindowsRestartResult::Status>(99));
        require(!protocol::encodeMessage(restart), "Unknown restart status accepted");
        restartResult->set_operation_id(std::string(64, 'A'));
        restartResult->set_status(wire::WindowsRestartResult::FAILED);
        require(!protocol::encodeMessage(restart), "Non-lowercase operation id accepted");
        restart.Clear();
        restart.set_protocol_version(protocol::kVersion);
        restart.set_sequence(1);
        restartRequest = restart.mutable_windows_restart_request();
        restartRequest->set_operation_id(operationId);
        restartRequest->set_device_id(deviceId);
        restartRequest->set_expires_at_unix_seconds(2000000000);
        restartRequest->set_device_id("d-" + std::string(63, 'b') + "B");
        require(!protocol::encodeMessage(restart), "Malformed restart device id accepted");
        restartRequest->set_device_id(deviceId);
        restartRequest->set_expires_at_unix_seconds(0);
        require(!protocol::encodeMessage(restart), "Zero restart deadline accepted");
        restartRequest->set_expires_at_unix_seconds(2000000000);
        restart.set_generation(1);
        require(!protocol::encodeMessage(restart), "Restart request accepted frame metadata");
        wire::Envelope capabilities;
        capabilities.set_protocol_version(protocol::kVersion);
        capabilities.set_sequence(1);
        auto* helloCapabilities = capabilities.mutable_hello();
        helloCapabilities->set_device_id("test-device");
        helloCapabilities->set_agent_version("0.1.0");
        helloCapabilities->mutable_capabilities()->set_system_restart(true);
        helloCapabilities->mutable_capabilities()->set_boot_time_unix_seconds(123456789);
        auto capabilityBytes = protocol::encodeMessage(capabilities);
        require(capabilityBytes.has_value(), "Restart capabilities rejected");
        auto capabilityDecoded = protocol::decodeMessage(*capabilityBytes);
        require(capabilityDecoded && capabilityDecoded->hello().capabilities().system_restart() &&
                    capabilityDecoded->hello().capabilities().boot_time_unix_seconds() == 123456789,
                "Restart capabilities round trip failed");
        std::cout << "Protocol bounds, compatibility and channel type checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
