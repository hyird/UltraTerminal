#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ruvia/core/asio_task.h>
#include <ruvia/core/event_loop_attachment.h>

#include "features/desktop/desktop.transport.h"
#include "features/desktop/frame_flow.runtime.h"
#include "features/desktop/relay.runtime.h"
#include "ultraterimal/protocol/desktop.protocol.h"
#include "ultraterimal/protocol/desktop_ipc.protocol.h"

// Link only the production Relay, Continuity and send queues. Replace the
// native Transport boundary, not their logic; run the real Native/IPC validators
// on both sides, with controllable native writes and Browser cleanup delivery.
namespace fixture {
using namespace ultraterimal;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
wire::Envelope packet() {
    wire::Envelope message;
    message.set_protocol_version(protocol::kVersion);
    message.set_session_id("relay");
    return message;
}
wire::Envelope config(std::uint64_t generation, bool v2 = true) {
    auto message = packet();
    message.set_generation(generation);
    auto* body = message.mutable_video_config();
    body->set_codec("avc1.42E01F");
    body->set_width(256);
    body->set_height(128);
    body->set_desktop_optimization_contract(v2 ? 2 : 0);
    return message;
}
wire::Envelope frame(std::uint64_t generation, std::uint64_t id) {
    auto message = packet();
    message.set_generation(generation);
    message.set_frame_id(id);
    message.mutable_video_frame()->set_data("key");
    message.mutable_video_frame()->set_key_frame(true);
    message.mutable_video_frame()->set_timestamp_microseconds(id * 1000);
    return message;
}
void patch(wire::PatchRect& body, std::uint32_t x = 0) {
    body.mutable_destination()->set_x(x);
    body.mutable_destination()->set_width(128);
    body.mutable_destination()->set_height(64);
    std::string png(33, '\0');
    png.replace(0, 8, "\211PNG\r\n\032\n", 8);
    png[11] = 13;
    png.replace(12, 4, "IHDR");
    png[19] = static_cast<char>(128);
    png[23] = 64;
    body.set_png(png); // Validator fixture: no native PNG decoder is replaced.
}
wire::Envelope detail(std::uint64_t generation, std::uint64_t id, std::uint64_t batch, std::uint32_t x = 0) {
    auto message = packet();
    message.set_generation(generation);
    message.set_frame_id(id);
    message.set_base_frame_id(id);
    auto* body = message.mutable_video_refinement();
    body->set_batch_id(batch);
    body->set_kind(batch ? wire::STATIC_DETAIL : wire::UNSPECIFIED);
    patch(*body->add_patches(), x);
    return message;
}
wire::Envelope ack(std::uint64_t generation, std::uint64_t id, std::uint64_t batch) {
    auto message = packet();
    message.set_generation(generation);
    message.set_frame_id(id);
    message.mutable_desktop_refinement_ack()->set_batch_id(batch);
    message.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::SUPERSEDED);
    return message;
}
wire::Envelope capabilities() {
    auto message = packet();
    message.mutable_desktop_video_capabilities()->add_codecs(wire::AVC);
    message.mutable_desktop_video_capabilities()->set_desktop_optimization_contract(2);
    return message;
}
struct Tuple {
    std::uint64_t generation, frame, batch;
    bool matches(const wire::Envelope& message) const {
        return generation == message.generation() && frame == message.frame_id() && batch == message.desktop_refinement_ack().batch_id();
    }
};
struct Boundary {
    agent::DesktopSendQueue input;
    std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>> changed;
    asio::steady_timer writeHold, watch;
    protocol::DesktopChannelValidator nativeVideo{{"relay", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, 2}};
    protocol::DesktopChannelValidator nativeInput{{"relay", "input", wire::DESKTOP_INPUT, protocol::ChannelSource::kBrowser, false, 2}};
    protocol::DesktopChannelValidator nativeControl{{"relay", "input", wire::DESKTOP_INPUT, protocol::ChannelSource::kAgent, false, 2}};
    std::vector<wire::Envelope> videos, controls;
    std::optional<Tuple> globalOutstanding;
    std::uint64_t videoSequence = 1, inputSequence = 1, controlSequence = 1;
    std::size_t handled = 0, cleanups = 0;
    bool delivered = false, holdNext = false, blocked = false;
    explicit Boundary(ruvia::event_loop loop)
        : input(loop.handle(), 128, 2 * 1024 * 1024), changed(ruvia::make_channel<bool>(loop.handle(), {.capacity_ = 512})), writeHold(loop.executor()), watch(loop.executor()) {
        watch.expires_at(asio::steady_timer::time_point::max());
    }
    void notify() { require(changed.first.send(true).accepted(), "Fixture observation queue overflow"); }
    template <class Predicate>
    asio::awaitable<void> until(Predicate ready) {
        while (!ready()) {
            require((co_await ruvia::as_awaitable(changed.second.receive())).has_value(), "Fixture observation closed");
        }
    }
    asio::awaitable<void> inject(wire::Envelope message) {
        message.set_channel_id("input");
        message.set_sequence(++inputSequence);
        require(co_await ruvia::as_awaitable(input.send(std::move(message))), "Fixture Browser queue full");
    }
    asio::awaitable<void> processed(wire::Envelope message) {
        const auto expected = handled + 1;
        co_await inject(std::move(message));
        co_await until([&] { return handled >= expected; });
    }
    void releaseWrite() { writeHold.cancel(); }
    std::size_t batches() const {
        std::size_t count = 0;
        for (const auto& message : videos) {
            count += message.has_video_refinement() && message.video_refinement().batch_id() != 0;
        }
        return count;
    }
};
Boundary* activeBoundary = nullptr;
struct Worker {
    std::shared_ptr<agent::DesktopWorkerChannel> channel;
    protocol::DesktopPipeValidator output{{"relay", 1, false, protocol::DesktopPipeSource::kDesktop, 2}};
    protocol::DesktopPipeValidator serviceOutput{{"relay", 1, false, protocol::DesktopPipeSource::kService, 2}};
    protocol::DesktopPipeValidator workerInput{{"relay", 1, false, protocol::DesktopPipeSource::kService, 2}};
    agent::DesktopFrameFlow flow;
    std::uint64_t outputSequence = 1, inputSequence = 1;
    explicit Worker(agent::DesktopRelay& relay) : channel(relay.attach()) {
        auto ready = packet();
        ready.set_sequence(1);
        ready.mutable_desktop_ready();
        require(output.accept(ready).has_value(), "IPC readiness rejected");
        auto start = packet();
        start.set_sequence(1);
        start.mutable_desktop_start()->set_windows_session_id(1);
        start.mutable_desktop_start()->set_desktop_optimization_contract(2);
        require(serviceOutput.accept(start).has_value() && workerInput.accept(start).has_value(), "IPC start rejected");
    }
    asio::awaitable<void> send(wire::Envelope message, bool trackDetail = false) {
        message.set_sequence(++outputSequence);
        message.set_channel_id("video");
        const auto accepted = output.accept(message);
        if (!accepted) {
            std::cerr << "Rejected IPC output: body=" << message.body_case() << " generation=" << message.generation() << " frame=" << message.frame_id() << " batch=" << message.video_refinement().batch_id() << " error=" << static_cast<int>(accepted.error()) << '\n';
        }
        require(accepted.has_value(), "Worker IPC output rejected");
        const auto now = agent::DesktopFrameFlow::Clock::now();
        if (message.has_video_config()) {
            flow.reset(message.generation());
        } else if (message.has_video_frame() || message.has_desktop_region_update()) {
            flow.sent(message.generation(), message.frame_id(), now, message.ByteSizeLong(), message.has_desktop_region_update() ? agent::DesktopFrameFlow::FrameType::kRegion : message.video_frame().key_frame() ? agent::DesktopFrameFlow::FrameType::kKeyFrame
                                                                                                                                                                                                                   : agent::DesktopFrameFlow::FrameType::kVideo);
        } else if (trackDetail) {
            require(flow.sentRefinement(message.generation(), message.frame_id(), message.video_refinement().batch_id(), now, message.ByteSizeLong()), "Worker Flow could not send fixture detail");
        }
        co_await channel->sendVideo(std::move(message), 2);
    }
    asio::awaitable<wire::Envelope> receive() {
        auto entry = co_await channel->receiveInput();
        auto message = std::move(entry.message);
        message.set_protocol_version(protocol::kVersion);
        message.set_session_id("relay");
        message.set_channel_id("input");
        message.set_sequence(++inputSequence);
        require(serviceOutput.accept(message).has_value() && workerInput.accept(message).has_value(), "Service/Worker IPC input rejected");
        channel->completeInput(entry);
        co_return message;
    }
    asio::awaitable<void> cleanup(std::uint64_t generation, std::uint64_t id, std::uint64_t batch) {
        auto message = co_await receive();
        require(message.has_desktop_refinement_ack() && Tuple{generation, id, batch}.matches(message) && message.desktop_refinement_ack().status() == wire::DesktopRefinementAck::SUPERSEDED, "Refusal/cleanup lost exact local tuple or status");
        require(flow.acknowledgeRefinement(generation, id, batch, agent::DesktopFrameFlow::Clock::now()), "Exact local ACK did not release the real Worker Flow");
    }
    asio::awaitable<void> marker(Boundary& boundary) {
        co_await boundary.processed(capabilities());
        require((co_await receive()).has_desktop_video_capabilities(), "Old/duplicate ACK leaked into replacement IPC queue");
    }
};
asio::awaitable<void> start(Worker& worker, Boundary& boundary, std::uint64_t generation = 1, bool v2 = true) {
    const auto count = boundary.videos.size() + 2;
    co_await worker.send(config(generation, v2));
    co_await worker.send(frame(generation, 1));
    co_await boundary.until([&] { return boundary.videos.size() >= count; });
}
asio::awaitable<void> handoff(agent::DesktopRelay& relay, Boundary& boundary, bool cleanupInGap) {
    Worker a(relay);
    co_await start(a, boundary);
    co_await a.send(detail(1, 1, 1), true);
    co_await boundary.until([&] { return boundary.batches() == 1; });
    // A configuration barrier must not clear its service lease either.
    co_await start(a, boundary, 2);
    relay.detach();
    if (cleanupInGap) {
        co_await boundary.processed(ack(1, 1, 1));
        require(boundary.cleanups == 1, "Gap cleanup was not consumed");
    }
    Worker b(relay);
    co_await start(b, boundary);
    require(boundary.videos.back().generation() == 3, "Real Continuity did not map replacement generation");
    auto feedback = packet();
    feedback.mutable_quality_feedback()->set_displayed_generation(3);
    feedback.mutable_quality_feedback()->set_displayed_frame(1);
    co_await boundary.processed(std::move(feedback));
    auto localFeedback = co_await b.receive();
    require(localFeedback.has_quality_feedback() && localFeedback.quality_feedback().displayed_generation() == 1, "Replacement feedback lost local generation");
    require(b.flow.acknowledge(1, 1, 0, 0, agent::DesktopFrameFlow::Clock::now()), "Real Flow feedback failed");
    co_await b.send(detail(1, 1, 1), true);
    if (!cleanupInGap) {
        co_await b.cleanup(1, 1, 1);
        require(boundary.batches() == 1, "B refinement reached native while A PNG was held");
        // Video and dependent reliable region continue while old PNG is held.
        co_await b.send(frame(1, 2));
        auto region = packet();
        region.set_generation(1);
        region.set_frame_id(3);
        region.set_base_frame_id(2);
        region.mutable_desktop_region_update()->set_target_frames_per_second(60);
        region.mutable_desktop_region_update()->set_target_bitrate(24000000);
        patch(*region.mutable_desktop_region_update()->add_patches());
        co_await b.send(std::move(region));
        co_await boundary.until([&] { return boundary.videos.back().has_desktop_region_update(); });
        require(boundary.videos.back().generation() == 3 && boundary.videos.back().frame_id() == 3, "Reliable region stalled or dropped for old PNG");
        co_await boundary.processed(ack(1, 1, 1));
        co_await b.marker(boundary);
        co_await b.send(detail(1, 3, 2), true);
    }
    co_await boundary.until([&] { return boundary.batches() == 2; });
    const auto current = boundary.videos.back();
    // Duplicate A ACK plus near-miss current tuples cannot clear B's lease.
    co_await boundary.processed(ack(1, 1, 1));
    co_await boundary.processed(ack(3, current.frame_id() + 1, current.video_refinement().batch_id()));
    co_await boundary.processed(ack(3, current.frame_id(), 99));
    co_await b.marker(boundary);
    require(boundary.cleanups == 1, "Old/near-miss ACK released new native tuple");
    // Deliberately bypass Flow admission to verify same-worker service safety.
    co_await b.send(detail(1, current.frame_id(), 3, 128));
    auto refusal = co_await b.receive();
    require(refusal.has_desktop_refinement_ack() && Tuple{1, current.frame_id(), 3}.matches(refusal), "Same-worker occupied lease was not refused");
    require(boundary.batches() == 2 && b.flow.refinementOutstanding(agent::DesktopFrameFlow::Clock::now()), "Local refusal released a different outstanding tuple");
    co_await boundary.processed(ack(3, current.frame_id(), current.video_refinement().batch_id()));
    co_await b.cleanup(1, current.frame_id(), current.video_refinement().batch_id());
    co_await boundary.processed(ack(3, current.frame_id(), current.video_refinement().batch_id()));
    co_await b.marker(boundary);
    co_await b.send(frame(1, 4));
    co_await b.send(detail(1, 4, 4), true);
    co_await boundary.until([&] { return boundary.batches() == 3; });
    // Same worker, new generation: old cleanup still maps to its retired Flow.
    co_await start(b, boundary, 2);
    co_await b.send(detail(2, 1, 1));
    refusal = co_await b.receive();
    require(refusal.has_desktop_refinement_ack() && Tuple{2, 1, 1}.matches(refusal) && boundary.batches() == 3, "Configuration cleared service lease");
    co_await boundary.processed(ack(3, 4, 4));
    co_await b.cleanup(1, 4, 4);
    co_await b.send(frame(2, 2));
    co_await b.send(detail(2, 2, 2), true);
    co_await boundary.until([&] { return boundary.batches() == 4; });
    require(boundary.cleanups == 3, "Exact Browser cleanup was not released once per tuple");
}
asio::awaitable<void> attempted(Worker& worker, std::uint64_t id, std::uint64_t batch, bool& failed) {
    try {
        co_await worker.send(detail(1, id, batch, 128));
    } catch (const std::runtime_error&) {
        failed = true;
    }
}
asio::awaitable<void> queueFull(agent::DesktopRelay& relay, Boundary& boundary) {
    Worker worker(relay);
    boundary.holdNext = true;
    co_await worker.send(config(1));
    co_await boundary.until([&] { return boundary.blocked; });
    co_await worker.send(frame(1, 1)); // Both queue slots, including active write.
    co_await worker.send(detail(1, 1, 1), true);
    co_await worker.cleanup(1, 1, 1);
    require(boundary.batches() == 0, "Queue-full batch reached native");
    boundary.releaseWrite();
    co_await boundary.until([&] { return boundary.videos.size() == 2; });
    co_await worker.send(frame(1, 2));
    co_await worker.send(detail(1, 2, 2), true);
    co_await boundary.until([&] { return boundary.batches() == 1; });
    // A cleanup for the never-forwarded batch must not touch this new lease.
    co_await boundary.processed(ack(1, 1, 1));
    co_await worker.marker(boundary);
    for (unsigned index = 0; index < 64; ++index) {
        co_await boundary.processed(capabilities());
    }
    bool done = false, failed = false;
    std::exception_ptr taskError;
    asio::co_spawn(boundary.watch.get_executor(), attempted(worker, 2, 3, failed), [&](std::exception_ptr error) {
        taskError = error;
        done = true;
        boundary.notify();
    });
    co_await asio::post(boundary.watch.get_executor(), asio::use_awaitable);
    require(!done && boundary.batches() == 1, "Full local IPC queue dropped ACK or bypassed backpressure");
    require((co_await worker.receive()).has_desktop_video_capabilities(), "Input FIFO changed under refusal");
    co_await boundary.until([&] { return done; });
    require(!failed && !taskError, "Refusal did not resume after input capacity release");
    for (unsigned index = 0; index < 63; ++index) {
        require((co_await worker.receive()).has_desktop_video_capabilities(), "Local ACK reordered ordinary IPC input");
    }
    auto refusal = co_await worker.receive();
    require(refusal.has_desktop_refinement_ack() && Tuple{1, 2, 3}.matches(refusal), "Backpressured refusal lost exact tuple");
    for (unsigned index = 0; index < 64; ++index) {
        co_await boundary.processed(capabilities());
    }
    co_await worker.send(frame(1, 3));
    done = failed = false;
    asio::co_spawn(boundary.watch.get_executor(), attempted(worker, 3, 4, failed), [&](std::exception_ptr error) {
        taskError = error;
        done = true;
        boundary.notify();
    });
    co_await asio::post(boundary.watch.get_executor(), asio::use_awaitable);
    require(!done, "Second full IPC queue did not backpressure");
    relay.detach();
    co_await boundary.until([&] { return done; });
    require(failed && !taskError && boundary.batches() == 1, "Detach did not wake blocked refusal without native forwarding");
    co_await boundary.processed(ack(1, 2, 2));
    Worker replacement(relay);
    co_await start(replacement, boundary);
    co_await replacement.send(detail(1, 1, 1), true);
    co_await boundary.until([&] { return boundary.batches() == 2; });
}
asio::awaitable<void> legacy(agent::DesktopRelay& relay, Boundary& boundary) {
    Worker worker(relay);
    co_await start(worker, boundary, 1, false);
    co_await worker.send(detail(1, 1, 0));
    co_await worker.send(frame(1, 2));
    co_await worker.send(detail(1, 2, 0));
    co_await boundary.until([&] { return boundary.videos.size() == 5; });
    require(!boundary.globalOutstanding && boundary.batches() == 0, "Legacy refinement acquired v2 lease");
    co_await worker.marker(boundary); // No local v2 ACK for batch ID zero.
}
} // namespace fixture

namespace ultraterimal::agent {
struct DesktopTransport::State {
    fixture::Boundary& boundary;
    explicit State(fixture::Boundary& value) : boundary(value) {}
};
DesktopTransport::DesktopTransport(ruvia::event_loop, ConnectionConfig, wire::Envelope) : state_(std::make_shared<State>(*fixture::activeBoundary)) {}
DesktopTransport::~DesktopTransport() = default;
asio::awaitable<std::chrono::microseconds> DesktopTransport::sendVideo(wire::Envelope message, std::uint32_t contract) {
    auto& boundary = state_->boundary;
    message.set_channel_id("video");
    message.set_protocol_version(protocol::kVersion);
    message.set_session_id("relay");
    message.set_sequence(++boundary.videoSequence);
    if (message.has_video_config()) {
        boundary.nativeVideo.setOptimizationContract(contract);
    }
    fixture::require(boundary.nativeVideo.accept(message).has_value(), "Relay output rejected by real native validator");
    if (boundary.holdNext) {
        boundary.holdNext = false;
        boundary.blocked = true;
        boundary.writeHold.expires_at(asio::steady_timer::time_point::max());
        boundary.notify();
        std::error_code error;
        co_await boundary.writeHold.async_wait(asio::redirect_error(asio::use_awaitable, error));
        boundary.blocked = false;
    }
    if (message.has_video_refinement() && message.video_refinement().batch_id()) {
        fixture::require(!boundary.globalOutstanding, "Native global outstanding credit conflict");
        boundary.globalOutstanding = fixture::Tuple{message.generation(), message.frame_id(), message.video_refinement().batch_id()};
    }
    boundary.videos.push_back(std::move(message));
    boundary.notify();
    co_return std::chrono::microseconds(0);
}
asio::awaitable<void> DesktopTransport::sendCursor(wire::Envelope message) {
    auto& boundary = state_->boundary;
    message.set_channel_id("input");
    message.set_protocol_version(protocol::kVersion);
    message.set_session_id("relay");
    message.set_sequence(++boundary.controlSequence);
    fixture::require(boundary.nativeControl.accept(message).has_value(), "Relay pipeline/control output rejected");
    boundary.controls.push_back(std::move(message));
    boundary.notify();
    co_return;
}
asio::awaitable<wire::Envelope> DesktopTransport::receiveInput() {
    auto& boundary = state_->boundary;
    if (boundary.delivered) {
        ++boundary.handled;
        boundary.notify(); // Relay finished filtering/queuing the preceding input.
    }
    auto entry = co_await ruvia::as_awaitable(boundary.input.receive());
    if (!entry) {
        throw std::runtime_error("Fixture transport closed");
    }
    auto message = std::move(entry->message);
    boundary.input.complete(*entry);
    fixture::require(boundary.nativeInput.accept(message).has_value(), "Old Browser cleanup rejected before Relay");
    if (message.has_desktop_refinement_ack() && boundary.globalOutstanding && boundary.globalOutstanding->matches(message)) {
        boundary.globalOutstanding.reset();
        ++boundary.cleanups;
    }
    boundary.delivered = true;
    co_return message;
}
asio::awaitable<void> DesktopTransport::watchVideo() {
    std::error_code error;
    co_await state_->boundary.watch.async_wait(asio::redirect_error(asio::use_awaitable, error));
}
std::uint32_t DesktopTransport::serverOptimizationContract() const { return 2; }
bool DesktopTransport::serverContinuousFrames() const { return false; }
void DesktopTransport::cancel() {
    state_->boundary.input.close();
    state_->boundary.watch.cancel();
    state_->boundary.writeHold.cancel();
}
} // namespace ultraterimal::agent

namespace fixture {
asio::awaitable<void> exercise(ruvia::event_loop loop, unsigned mode) {
    Boundary boundary(loop);
    activeBoundary = &boundary;
    agent::DesktopTransport transport(loop, {}, {});
    agent::DesktopRelay relay(loop, transport);
    bool joined = false;
    std::exception_ptr relayError, scenarioError;
    asio::co_spawn(loop.executor(), relay.run(), [&](std::exception_ptr error) {
        relayError = error;
        joined = true;
        boundary.notify();
    });
    try {
        if (mode < 2) {
            co_await handoff(relay, boundary, mode == 0);
        } else if (mode == 2) {
            co_await queueFull(relay, boundary);
        } else {
            co_await legacy(relay, boundary);
        }
    } catch (...) {
        scenarioError = std::current_exception();
    }
    relay.stop();
    co_await boundary.until([&] { return joined; });
    if (scenarioError) {
        std::rethrow_exception(scenarioError);
    }
    if (relayError) {
        std::rethrow_exception(relayError);
    }
}
} // namespace fixture
int main() {
    try {
        for (unsigned mode = 0; mode < 4; ++mode) {
            asio::io_context io;
            auto owner = ruvia::attach_event_loop(io);
            std::exception_ptr failure;
            asio::steady_timer deadline(io, std::chrono::seconds(5));
            deadline.async_wait([&](std::error_code error) {
                if (!error) {
                    failure = std::make_exception_ptr(std::runtime_error("Relay fixture deadline exceeded"));
                    io.stop();
                }
            });
            asio::co_spawn(owner.loop().executor(), fixture::exercise(owner.loop(), mode), [&](std::exception_ptr error) {
                failure = error;
                deadline.cancel();
                io.stop();
            });
            owner.run();
            if (failure) {
                std::rethrow_exception(failure);
            }
            std::cout << "Relay credit scenario " << mode << " passed\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
