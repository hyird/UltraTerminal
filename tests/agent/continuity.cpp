#include <iostream>
#include <limits>
#include <stdexcept>

#include "features/desktop/continuity.protocol.h"
#include "features/desktop/handoff.runtime.h"

namespace {
using namespace ultraterimal;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
wire::Envelope configuration(std::uint64_t generation) {
    wire::Envelope message;
    message.set_generation(generation);
    message.mutable_video_config();
    return message;
}
wire::Envelope probe(std::uint32_t nonce, std::uint32_t accepted = 0) {
    wire::Envelope message;
    message.mutable_desktop_probe()->set_nonce(nonce);
    message.mutable_desktop_probe()->set_accepted_response_nonce(accepted);
    return message;
}
wire::Envelope feedback(std::uint64_t generation) {
    wire::Envelope message;
    message.mutable_quality_feedback()->set_displayed_generation(generation);
    message.mutable_quality_feedback()->set_displayed_frame(1);
    return message;
}
wire::Envelope key(std::uint64_t generation) {
    wire::Envelope message;
    message.set_generation(generation);
    message.mutable_input_event()->set_kind(wire::InputEvent::KEY);
    return message;
}
} // namespace
int main() {
    try {
        agent::DesktopContinuity stream;
        stream.beginWorker();
        auto config = configuration(1);
        require(stream.outgoing(config) && config.generation() == 1, "Initial generation changed");
        auto input = key(1);
        require(stream.incoming(input), "Initial worker input was unnecessarily delayed");
        auto first = probe(1);
        require(stream.outgoing(first) && !first.desktop_probe().reset_input(), "Initial probe changed");
        auto response = probe(1);
        require(stream.incoming(response), "Probe acknowledgement rejected");
        auto second = probe(2, 1);
        require(stream.outgoing(second) && second.desktop_probe().accepted_response_nonce() == 1, "Accepted probe watermark lost");
        auto sameWorker = configuration(2);
        require(stream.outgoing(sameWorker) && sameWorker.generation() == 2, "Resolution generation changed");

        stream.beginWorker();
        auto oldInput = key(2);
        auto oldFeedback = feedback(2);
        auto oldProbe = probe(2);
        require(!stream.incoming(oldInput) && !stream.incoming(oldFeedback) && !stream.incoming(oldProbe), "Retired worker commands leaked");
        config = configuration(1);
        require(stream.outgoing(config) && config.generation() == 3, "New worker reused old generation");
        first = probe(1);
        require(stream.outgoing(first) && first.desktop_probe().nonce() == 3 && first.desktop_probe().reset_input(), "New worker did not reset browser input");
        require(first.desktop_probe().accepted_response_nonce() == 1, "Browser's acknowledged nonce went backwards");
        auto newInput = key(3);
        require(!stream.incoming(newInput), "Input admitted before new binding");
        auto displayed = feedback(3);
        require(stream.incoming(displayed) && displayed.quality_feedback().displayed_generation() == 1, "Feedback not rebased");
        require(!stream.incoming(newInput), "Video feedback bypassed input reset barrier");
        response = probe(3);
        require(stream.incoming(response) && response.desktop_probe().nonce() == 1, "New probe response not rebased");
        require(stream.incoming(newInput) && newInput.generation() == 1, "New input binding not translated");
        require(!stream.incoming(oldInput), "Late previous-generation input admitted");
        auto duplicate = probe(3);
        require(stream.incoming(duplicate) && duplicate.desktop_probe().nonce() == 1, "Current-worker replay hidden from worker validation");
        second = probe(2, 1);
        require(stream.outgoing(second) && second.desktop_probe().nonce() == 4 && second.desktop_probe().accepted_response_nonce() == 3, "New acknowledgement not translated");
        auto resize = wire::Envelope{};
        resize.mutable_desktop_resize()->set_request_id(123);
        require(stream.incoming(resize), "Bound resize rejected");
        stream.beginWorker();
        require(!stream.incoming(resize), "Untagged old resize replayed during handoff");

        agent::DesktopContinuity overflow;
        overflow.beginWorker();
        config = configuration(std::numeric_limits<std::uint64_t>::max());
        require(overflow.outgoing(config), "Maximum generation rejected prematurely");
        first = probe(std::numeric_limits<std::uint32_t>::max());
        require(overflow.outgoing(first), "Maximum nonce rejected prematurely");
        overflow.beginWorker();
        config = configuration(1);
        first = probe(1);
        require(!overflow.outgoing(config) && !overflow.outgoing(first), "Namespace wrapped on handoff");
        agent::DesktopHandoffState binding(1);
        require(binding.observe(UINT32_MAX), "Transient console disappearance was ignored");
        require(!binding.select(UINT32_MAX) && binding.changed(1), "Invalid selection erased pending transition");
        require(binding.select(1) && !binding.changed(1), "Notification from selection leaked into the new launch");
        require(!binding.replaceableExit(1, 1, false, std::nullopt), "Ordinary bootstrap failure retried without console change");
        require(binding.select(2) && binding.observe(1), "Rapid return to the user console was ignored");
        require(binding.replaceableExit(1, 1, false, std::nullopt), "Known pre-start cancellation treated as worker failure");
        require(!binding.replaceableExit(1, 1, true, std::nullopt), "Running worker crash was hidden as startup cancellation");
        require(!binding.replaceableExit(1, 1430522123, false, wire::DesktopFault::ENCODE_FAILED), "Independent worker failure was erased");
        require(binding.replaceableExit(1, 1430522123, true, wire::DesktopFault::IPC_FAILED), "Expected old-worker pipe teardown rejected");
        require(binding.select(1) && !binding.changed(1), "Second selection retained obsolete handoff state");
        agent::DesktopHandoffBudget budget;
        require(budget.admit() && budget.admit() && budget.admit(), "Handoff budget too small");
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            require(!budget.admit(), "Flapping console replenished handoff budget");
        }
        std::cout << "Desktop continuity, stale-input barriers and finite handoff budget passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
