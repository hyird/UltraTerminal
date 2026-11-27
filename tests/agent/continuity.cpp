#include <iostream>
#include <limits>
#include <stdexcept>

#include "features/desktop/continuity.protocol.h"
#include "features/desktop/handoff.runtime.h"
#include "features/desktop/optimization.runtime.h"
#include "ultraterimal/protocol/desktop_ipc.protocol.h"

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
// Exercise the actual protocol validators at all L-owned chain boundaries.
// Config2 support is deliberately unchanged; permission is a separate snapshot.
class NegotiationLink {
  public:
    explicit NegotiationLink(std::uint32_t inputServer, std::uint32_t videoServer)
        : server_(agent::DesktopOptimizationGate::serverContract(inputServer, videoServer)),
          worker_(server_), bridge_(server_),
          workerOutput_({"negotiation", 1, false, protocol::DesktopPipeSource::kDesktop}),
          bridgeInput_({"negotiation", 1, false, protocol::DesktopPipeSource::kDesktop}),
          bridgeOutput_({"negotiation", 1, false, protocol::DesktopPipeSource::kService}),
          workerInput_({"negotiation", 1, false, protocol::DesktopPipeSource::kService}),
          nativeVideo_({"negotiation", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false}),
          browserVideo_({"negotiation", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false}),
          nativeInput_({"negotiation", "input", wire::DESKTOP_INPUT, protocol::ChannelSource::kBrowser, false}) {
        auto ready = packet(1, "");
        ready.mutable_desktop_ready();
        require(workerOutput_.accept(ready).has_value() && bridgeInput_.accept(ready).has_value(), "IPC readiness rejected");
        auto start = packet(1, "");
        start.mutable_desktop_start()->set_windows_session_id(1);
        start.mutable_desktop_start()->set_desktop_optimization_contract(server_);
        require(bridgeOutput_.accept(start).has_value() && workerInput_.accept(start).has_value(), "IPC start rejected");
    }
    void request(std::uint32_t version) {
        auto caps = packet(++inputSequence_, "input");
        caps.mutable_desktop_video_capabilities()->add_codecs(wire::AVC);
        caps.mutable_desktop_video_capabilities()->set_desktop_optimization_contract(version);
        require(nativeInput_.accept(caps).has_value() && bridgeOutput_.accept(caps).has_value() && workerInput_.accept(caps).has_value(), "Browser capabilities did not reach both IPC validators");
        bridge_.request(version);
        worker_.request(version);
        bridgeInput_.setOptimizationContract(bridge_.allowedContract());
        bridgeOutput_.setOptimizationContract(bridge_.allowedContract());
        workerOutput_.setOptimizationContract(worker_.allowedContract());
        workerInput_.setOptimizationContract(worker_.allowedContract());
    }
    void configure(std::uint64_t generation, std::uint32_t declaration = 2) {
        auto message = packet(++videoSequence_, "video");
        message.set_generation(generation);
        auto* config = message.mutable_video_config();
        config->set_codec("avc1.42E01F");
        config->set_width(256);
        config->set_height(128);
        config->set_desktop_optimization_contract(declaration);
        require(workerOutput_.accept(message).has_value() && bridgeInput_.accept(message).has_value(), "IPC capability declaration rejected");
        worker_.configure(*config);
        bridge_.configure(*config);
        // Relay captures this value with the queue entry; native validation
        // cannot substitute a newer request for the queued barrier's snapshot.
        nativeVideo_.setOptimizationContract(bridge_.activeContract());
        browserVideo_.setOptimizationContract(bridge_.activeContract());
        require(nativeVideo_.accept(message).has_value() && browserVideo_.accept(message).has_value(), "Native/browser capability declaration rejected");
        require(worker_.activeContract() == bridge_.activeContract(), "IPC activation diverged");
        generation_ = generation;
        frame_ = 0;
        frame();
    }
    void frame() {
        auto message = packet(++videoSequence_, "video");
        message.set_generation(generation_);
        message.set_frame_id(++frame_);
        auto* body = message.mutable_video_frame();
        body->set_data("key");
        body->set_key_frame(true);
        body->set_timestamp_microseconds(frame_ * 1000);
        require(acceptVideo(message), "Video progress failed across negotiation");
    }
    bool active() const { return worker_.activeContract() == 2; }

  private:
    static wire::Envelope packet(std::uint64_t sequence, const char* channel) {
        wire::Envelope message;
        message.set_protocol_version(protocol::kVersion);
        message.set_session_id("negotiation");
        message.set_channel_id(channel);
        message.set_sequence(sequence);
        return message;
    }
    bool acceptVideo(const wire::Envelope& message) {
        return workerOutput_.accept(message).has_value() && bridgeInput_.accept(message).has_value() && nativeVideo_.accept(message).has_value() && browserVideo_.accept(message).has_value();
    }
    std::uint32_t server_;
    agent::DesktopOptimizationGate worker_, bridge_;
    protocol::DesktopPipeValidator workerOutput_, bridgeInput_, bridgeOutput_, workerInput_;
    protocol::DesktopChannelValidator nativeVideo_, browserVideo_, nativeInput_;
    std::uint64_t inputSequence_ = 1, videoSequence_ = 1, generation_ = 0, frame_ = 0;
};
void negotiation() {
    NegotiationLink link(2, 2);
    link.configure(1); // FIRST Config2: no Browser request yet.
    require(!link.active(), "First capability declaration activated v2");
    for (const auto version : {0u, 1u, 3u, UINT32_MAX}) {
        link.request(version);
        link.frame();
        require(!link.active(), "Non-v2 capability activated v2");
    }
    link.request(2);
    link.frame();
    require(!link.active(), "Allow2 activated in current generation");
    link.configure(2);
    require(link.active(), "Requested v2 did not activate at the next barrier");
    link.request(99);
    link.frame();
    require(link.active(), "Revocation did not wait for a new generation");
    link.configure(3);
    require(!link.active(), "Revocation did not deactivate at the next barrier");
    link.request(2);
    link.configure(4, 99);
    require(!link.active(), "Non-v2 config activated v2");
    link.configure(5);
    require(link.active(), "Valid v2 config did not reactivate");
    for (const auto input : {0u, 2u, 99u}) {
        for (const auto video : {0u, 2u, 99u}) {
            if (input == 2 && video == 2) {
                continue;
            }
            NegotiationLink legacy(input, video);
            legacy.configure(1);
            legacy.request(2);
            legacy.configure(2);
            require(!legacy.active(), "Mixed/legacy Server channels activated v2");
        }
    }
}
} // namespace
int main() {
    try {
        negotiation();
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
        wire::Envelope report;
        auto* sample = report.mutable_desktop_pipeline_report()->add_samples();
        sample->set_generation(1);
        sample->set_frame_id(1);
        sample->set_batch_id(17);
        sample->set_source(wire::DesktopPipelineSample::DESKTOP_WORKER);
        sample->set_ipc_write_microseconds(123);
        require(stream.outgoing(report) && !report.generation() && report.desktop_pipeline_report().samples(0).generation() == 3 && report.desktop_pipeline_report().samples(0).batch_id() == 17, "Nested diagnostic generation not rebased");
        auto futureReport = wire::Envelope{};
        futureReport.mutable_desktop_pipeline_report()->add_samples()->set_generation(99);
        require(!stream.outgoing(futureReport), "Diagnostic invented a future generation");
        auto futureInput = key(101);
        require(!stream.incoming(futureInput) && !stream.incoming(newInput), "Diagnostic advanced generation or unlocked input");
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
        auto nextConfiguration = configuration(2);
        require(stream.outgoing(nextConfiguration) && nextConfiguration.generation() == 4, "Current worker generation advance failed");
        stream.beginWorker();
        require(!stream.incoming(resize), "Untagged old resize replayed during handoff");
        auto oldReport = wire::Envelope{};
        oldReport.mutable_desktop_pipeline_report()->add_samples()->set_generation(1);
        require(!stream.outgoing(oldReport), "Diagnostic without new worker configuration advanced continuity");

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
