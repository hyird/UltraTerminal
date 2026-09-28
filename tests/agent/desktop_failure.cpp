#include <iostream>
#include <stdexcept>
#include <system_error>

#include "features/desktop/session.runtime.h"
#include "features/logging/log.runtime.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/desktop_failure.error.h"
#include "ultraterimal/protocol/desktop_failure.protocol.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() {
    using namespace ultraterimal;
    using Fault = wire::DesktopFault;
    try {
        for (int stage = Fault::WORKER_STARTUP; stage <= Fault::BRIDGE_VIDEO_RECEIVE; ++stage) {
            for (int code = Fault::DISPLAY_UNAVAILABLE; code <= Fault::PROCESS_EXIT; ++code) {
                const auto status = protocol::desktopFailureExitCode(static_cast<Fault::Stage>(stage), static_cast<Fault::Code>(code));
                const auto fault = protocol::desktopFailureFromExitCode(status, 7, 123);
                require(fault && fault->stage() == stage && fault->code() == code && fault->windows_session_id() == 7 && fault->process_id() == 123, "Worker fault round trip failed");
            }
        }
        for (auto status : {0U, 1U, 2U, 0xc0000005U, 0x55440000U, 0x5544ffffU}) {
            require(!protocol::desktopFailureFromExitCode(status, 7, 123), "Ordinary/invalid exit status forged a structured fault");
        }
        const auto captureExit = protocol::desktopFailureExitCode(Fault::CAPTURE_ENCODE, Fault::DISPLAY_UNAVAILABLE);
        require(!protocol::desktopFailureFromExitCode(captureExit, 0, 123) && !protocol::desktopFailureFromExitCode(captureExit, UINT32_MAX, 123) && !protocol::desktopFailureFromExitCode(captureExit, 7, 0), "Invalid process identity accepted");
        require(protocol::desktopFailureExitCode(Fault::STAGE_UNKNOWN, Fault::DISPLAY_UNAVAILABLE) == 1, "Unknown stage was encoded");
        require(agent::desktopFailureCode(std::make_exception_ptr(agent::DesktopFailure(Fault::DISPLAY_UNAVAILABLE, "sensitive exception text must remain local")), Fault::CAPTURE_ENCODE) == Fault::DISPLAY_UNAVAILABLE, "Typed capture cause was lost");
        require(agent::desktopFailureCode(std::make_exception_ptr(agent::CaptureReset(Fault::CAPTURE_ACCESS_DENIED)), Fault::CAPTURE_ENCODE) == Fault::CAPTURE_ACCESS_DENIED, "Retry exhausted cause was lost");
        require(agent::desktopFailureCode(std::make_exception_ptr(std::runtime_error("password=not-a-real-secret")), Fault::INPUT_VALIDATION) == Fault::PROTOCOL_REJECTED, "Protocol error classification depends on raw text");
        require(agent::desktopFailureCode(std::make_exception_ptr(42), Fault::WORKER_STARTUP) == Fault::INTERNAL_FAILURE, "Nonstandard exception not bounded");
        require(agent::desktopFailureStage("capture_encode") == Fault::CAPTURE_ENCODE && agent::desktopFailureStage("untrusted") == Fault::STAGE_UNKNOWN, "Stage allowlist failed");

        agent::DesktopSessionResult result;
        result.processId = 123;
        result.fault.emplace();
        result.fault->set_stage(Fault::BRIDGE_WORKER_READ);
        result.fault->set_code(Fault::IPC_FAILED);
        result.recordWorkerExit(captureExit, 7);
        require(result.fault && result.fault->stage() == Fault::CAPTURE_ENCODE && result.fault->code() == Fault::DISPLAY_UNAVAILABLE, "Secondary pipe failure hid the first worker fault");
        result.fault->set_stage(Fault::WORKER_READY_WAIT);
        result.fault->set_code(Fault::STARTUP_TIMEOUT);
        result.recordWorkerExit(protocol::desktopFailureExitCode(Fault::INPUT_IPC_READ, Fault::IPC_FAILED), 7);
        require(result.fault->code() == Fault::STARTUP_TIMEOUT, "Worker cancellation hid the service deadline");
        result.fault->set_stage(Fault::BRIDGE_INPUT_RECEIVE);
        result.fault->set_code(Fault::SYSTEM_FAILURE);
        result.recordWorkerExit(captureExit, 7);
        require(result.fault->stage() == Fault::BRIDGE_INPUT_RECEIVE, "Earlier network fault was overwritten");
        result.fault.reset();
        result.forcedExit = true;
        result.recordWorkerExit(1, 7);
        require(result.fault->code() == Fault::EXIT_TIMEOUT, "Forced exit lost its bounded termination cause");

        agent::AgentLogBuffer logs("fault-runtime");
        const auto expected = *protocol::desktopFailureFromExitCode(captureExit, 7, 123);
        auto original = expected;
        logs.append(wire::AgentLog::DESKTOP_FAILED, 100, "fault-session", captureExit, false, &original);
        original.Clear();
        const auto* batch = logs.next();
        require(batch && batch->entries(0).desktop_fault().SerializeAsString() == expected.SerializeAsString(), "Log queue borrowed ephemeral fault data");
        const auto bytes = batch->SerializeAsString();
        require(logs.next()->SerializeAsString() == bytes, "Reconnect retransmission changed first fault");
        require(logs.acknowledge(1) && !logs.next(), "Fault ACK did not release owned data");
        logs.append(wire::AgentLog::DESKTOP_ENDED, 101, "fault-session", 0, false, &expected);
        require(!logs.next()->entries(0).has_desktop_fault(), "Normal closure uploaded a failure");
        std::cout << "Structured worker fault ownership, first-cause precedence, legacy exits, privacy and retransmission passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
