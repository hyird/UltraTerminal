#include <array>
#include <iostream>
#include <stdexcept>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>

#include "features/desktop/desktop.runtime.h"

namespace {
using namespace ultraterimal;
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Desktop pairing invariant failed");
    }
}
server::ChannelAuthorization authorization(bool video = false) {
    return {"session", video ? "video" : "input", "user", "device", wire::DESKTOP,
            video ? wire::DESKTOP_VIDEO : wire::DESKTOP_INPUT, true};
}
wire::Envelope refresh(std::uint64_t sequence) {
    wire::Envelope message;
    message.set_protocol_version(protocol::kVersion);
    message.set_session_id("session");
    message.set_channel_id("input");
    message.set_sequence(sequence);
    message.mutable_request_refresh();
    return message;
}
ruvia::Task<void> awaitClose(server::DesktopEndpoint& endpoint, bool& finished) {
    require(!(co_await endpoint.receive({})));
    finished = true;
}
ruvia::Task<void> forwardBurst(server::DesktopSession& session, std::shared_ptr<server::DesktopEndpoint> sender, bool& finished) {
    for (std::uint64_t sequence = 10; sequence <= 100; ++sequence) {
        require(co_await session.forward(sender, refresh(sequence)));
    }
    finished = true;
}
ruvia::Task<void> awaitRejected(server::DesktopSession& session, std::shared_ptr<server::DesktopEndpoint> sender, ruvia::StopToken stop, bool& finished) {
    require(!(co_await session.forward(sender, refresh(10), stop)));
    finished = true;
}
ruvia::Task<std::array<std::shared_ptr<server::DesktopEndpoint>, 4>> pair(server::DesktopSession& session, bool writable = true) {
    std::array<std::shared_ptr<server::DesktopEndpoint>, 4> endpoints;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto auth = authorization(i >= 2);
        auth.writeControl = writable;
        endpoints[i] = session.attach(auth, i % 2 ? protocol::ChannelSource::kAgent : protocol::ChannelSource::kBrowser);
        require(bool(endpoints[i]));
        require(session.ready() == (i == 3));
    }
    for (auto& endpoint : endpoints) {
        auto ready = co_await endpoint->receive({});
        require(ready && ready->ready());
    }
    co_return endpoints;
}
ruvia::Task<void> exercise(ruvia::WorkerHandle worker) {
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    {
        server::DesktopSession session(worker, budget);
        auto browser = session.attach(authorization(), protocol::ChannelSource::kBrowser);
        require(bool(browser));
        require(!(co_await session.forward(browser, refresh(2))));
        require(!session.attach(authorization(), protocol::ChannelSource::kBrowser));
        for (int mismatch = 0; mismatch < 6; ++mismatch) {
            auto auth = authorization();
            switch (mismatch) {
            case 0:
                auth.sessionId = "other";
                break;
            case 1:
                auth.userId = "other";
                break;
            case 2:
                auth.deviceId = "other";
                break;
            case 3:
                auth.channelId = "other";
                break;
            case 4:
                auth.writeControl = false;
                break;
            case 5:
                auth.kind = wire::SSH;
                break;
            }
            require(!session.attach(auth, protocol::ChannelSource::kAgent));
        }
        ruvia::StopSource stop;
        stop.requestStop();
        require(!(co_await browser->receive(stop.token())));
        session.close();
        require(!(co_await browser->receive({})));
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        auto feedback = refresh(2);
        feedback.mutable_quality_feedback()->set_decode_microseconds(18000);
        feedback.mutable_quality_feedback()->set_queue_depth(4);
        feedback.mutable_quality_feedback()->set_background(true);
        require(co_await session.forward(endpoints[0], feedback));
        require(session.diagnostics().feedbackMessages == 1);
        require(session.diagnostics().maximumDecodeMicroseconds == 18000);
        require(session.diagnostics().latestBrowserQueueDepth == 4);
        require(session.diagnostics().latestBackground);
        session.logHealth();
        session.close(server::DesktopCloseReason::kPeerClosed, server::DesktopCloseOrigin::kBrowserInput);
        session.close(server::DesktopCloseReason::kReadFailed, server::DesktopCloseOrigin::kAgentVideo);
        require(session.closeReason() == server::DesktopCloseReason::kPeerClosed);
        require(session.closeOrigin() == server::DesktopCloseOrigin::kBrowserInput);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
            require(co_await session.forward(endpoints[0], refresh(sequence)));
        }
        auto inFlight = co_await endpoints[1]->receive({});
        require(inFlight && !inFlight->ready());
        const auto encoded = protocol::encodeMessage(refresh(2));
        require(encoded && inFlight->bytes() == *encoded);
        const auto occupied = budget->used;
        require(occupied > inFlight->bytes().size());
        require(co_await session.forward(endpoints[0], refresh(10)));
        require(budget->used > occupied); // Receiving is not completion of a socket write.
        inFlight.reset();
        require(budget->used == occupied);
        for (std::uint64_t sequence = 3; sequence <= 10; ++sequence) {
            auto delivery = co_await endpoints[1]->receive({});
            const auto message = delivery ? protocol::decodeMessage(delivery->bytes()) : std::unexpected(protocol::MessageError::kInvalidField);
            require(message && message->sequence() == sequence);
        }
        require(budget->used == 0);
        require(!(co_await session.forward(endpoints[0], refresh(10)))); // Replay closes every endpoint.
        require(!session.ready());
        require(session.closeReason() == server::DesktopCloseReason::kMessageReplay);
        require(session.closeOrigin() == server::DesktopCloseOrigin::kBrowserInput);
        require(session.diagnostics().forwardedMessages == 9);
        require(session.diagnostics().peakPendingBytes > 0);
        for (auto& endpoint : endpoints) {
            require(!(co_await endpoint->receive({})));
        }
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
            require(co_await session.forward(endpoints[0], refresh(sequence)));
        }
        bool finished = false;
        ruvia::TaskScope scope(worker);
        scope.spawn(forwardBurst(session, endpoints[0], finished));
        co_await ruvia::sleepFor(worker, std::chrono::milliseconds(10));
        require(session.ready() && !finished);
        require(budget->used == 9 * protocol::encodeMessage(refresh(2))->size());
        for (std::uint64_t sequence = 2; sequence <= 100; ++sequence) {
            auto delivery = co_await endpoints[1]->receive({});
            require(delivery && delivery->bytes() == *protocol::encodeMessage(refresh(sequence)));
        }
        co_await scope.join();
        require(finished && session.ready() && budget->used == 0);
        require(session.diagnostics().backpressureEvents > 0);
        require(session.diagnostics().forwardedMessages == 99);
    }
    for (bool cancel : {false, true}) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
            require(co_await session.forward(endpoints[0], refresh(sequence)));
        }
        bool finished = false;
        ruvia::TaskScope scope(worker);
        ruvia::StopSource stop;
        scope.spawn(awaitRejected(session, endpoints[0], stop.token(), finished));
        co_await ruvia::sleepFor(worker, std::chrono::milliseconds(10));
        require(!finished && session.ready());
        if (cancel) {
            stop.requestStop();
        } else {
            session.close();
        }
        co_await scope.join();
        require(finished && !session.ready());
        require(!(co_await endpoints[1]->receive({})));
        require(budget->used == 0);
    }
    require(budget->used == 0);
    {
        auto session = std::make_unique<server::DesktopSession>(worker, budget);
        auto endpoints = co_await pair(*session);
        std::array<bool, 4> finished{};
        ruvia::TaskScope scope(worker);
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            scope.spawn(awaitClose(*endpoints[i], finished[i]));
        }
        co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));
        session.reset(); // Destruction must wake all four writers while endpoint handles survive.
        co_await scope.join();
        for (bool complete : finished) {
            require(complete);
        }
    }
    {
        auto small = std::make_shared<server::DesktopBudget>(protocol::encodeMessage(refresh(2))->size());
        server::DesktopSession first(worker, small);
        server::DesktopSession second(worker, small);
        auto one = co_await pair(first);
        auto two = co_await pair(second);
        require(co_await first.forward(one[0], refresh(2)));
        auto inFlight = co_await one[1]->receive({});
        require(inFlight && small->used == small->limit);
        require(!(co_await second.forward(two[0], refresh(2)))); // Shared limit includes another session's write.
        require(!second.ready() && first.ready());
        require(second.closeReason() == server::DesktopCloseReason::kGlobalBudgetExceeded);
        first.close();
        require(small->used == small->limit);
        inFlight.reset();
        require(small->used == 0);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        auto config = refresh(2);
        config.set_channel_id("video");
        config.set_generation(1);
        config.mutable_video_config()->set_width(16);
        config.mutable_video_config()->set_height(16);
        config.mutable_video_config()->set_codec("avc1.42C01E");
        require(co_await session.forward(endpoints[3], config));
        require(bool(co_await endpoints[2]->receive({})));
        auto frame = config;
        frame.set_sequence(3);
        frame.set_frame_id(1);
        frame.mutable_video_frame()->set_key_frame(true);
        frame.mutable_video_frame()->set_data("frame");
        frame.mutable_video_frame()->set_timestamp_microseconds(1);
        require(co_await session.forward(endpoints[3], frame));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().encoding.summary().samples == 0); // Legacy Agent.
        frame.set_sequence(4);
        frame.set_frame_id(2);
        frame.mutable_video_frame()->set_timestamp_microseconds(2);
        frame.mutable_video_frame()->set_encode_microseconds(1234);
        frame.mutable_video_frame()->set_hardware_encoding(true);
        require(co_await session.forward(endpoints[3], frame));
        auto received = co_await endpoints[2]->receive({});
        require(received && received->bytes() == *protocol::encodeMessage(frame));
        received.reset();
        require(session.diagnostics().encoding.summary().samples == 1 && session.diagnostics().latestHardwareEncoding);
        config.set_sequence(5);
        config.set_generation(2);
        config.mutable_video_config()->set_width(32);
        require(co_await session.forward(endpoints[3], config));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().encoding.summary().samples == 0 && session.diagnostics().videoWidth == 32);
        require(session.diagnostics().latestKeyFrameEncodeMicroseconds == 0);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        auto config = refresh(2);
        config.set_channel_id("video");
        config.set_generation(1);
        config.mutable_video_config()->set_width(1920);
        config.mutable_video_config()->set_height(1080);
        config.mutable_video_config()->set_codec("avc1.42C01E");
        require(co_await session.forward(endpoints[3], config));
        auto received = co_await endpoints[2]->receive({});
        require(received && received->bytes() == *protocol::encodeMessage(config));
        received.reset();
        auto frame = config;
        frame.mutable_video_frame()->set_key_frame(true);
        frame.mutable_video_frame()->set_data(std::string(8 * 1024 * 1024, 'v'));
        for (std::uint64_t i = 1; i <= 3; ++i) {
            frame.set_sequence(i + 2);
            frame.set_frame_id(i);
            frame.mutable_video_frame()->set_timestamp_microseconds(i);
            frame.mutable_video_frame()->set_encode_microseconds(static_cast<std::uint32_t>(i * 1000));
            require(co_await session.forward(endpoints[3], frame));
        }
        require(budget->used > 24 * 1024 * 1024);
        frame.set_sequence(6);
        frame.set_frame_id(4);
        frame.mutable_video_frame()->set_timestamp_microseconds(4);
        require(!(co_await session.forward(endpoints[3], frame))); // Per-session bytes, well below eight slots.
        require(!session.ready());
        require(session.closeReason() == server::DesktopCloseReason::kSessionBudgetExceeded);
        require(session.diagnostics().videoFrames == 3);
        const auto encoding = session.diagnostics().encoding.summary();
        require(encoding.samples == 3 && encoding.meanMicroseconds == 2000 && encoding.p95Microseconds == 3000 && encoding.maximumMicroseconds == 3000);
        require(session.diagnostics().videoWidth == 1920 && session.diagnostics().videoHeight == 1080);
        require(session.diagnostics().latestKeyFrameEncodeMicroseconds == 3000 && !session.diagnostics().latestHardwareEncoding);
        require(!(co_await endpoints[2]->receive({})));
        require(budget->used == 0);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session, false);
        require(co_await session.forward(endpoints[0], refresh(2)));
        require(bool(co_await endpoints[1]->receive({})));
        auto cursor = refresh(2);
        cursor.mutable_cursor_update()->set_visible(false);
        require(co_await session.forward(endpoints[1], cursor));
        auto received = co_await endpoints[0]->receive({});
        require(received && received->bytes() == *protocol::encodeMessage(cursor));
        received.reset();
        auto input = refresh(3);
        input.set_generation(1);
        input.mutable_input_event()->set_kind(wire::InputEvent::KEY);
        input.mutable_input_event()->set_code(65);
        require(!(co_await session.forward(endpoints[0], input)));
        require(!session.ready());
    }
}
} // namespace
int main() {
    try {
        ruvia::EventLoopPool pool({.loopCount = 1});
        pool.start();
        auto task = pool.loop(0).start(exercise(pool.loop(0).handle()));
        task.get();
        pool.join();
        std::cout << "Desktop four-endpoint pairing, binding, FIFO, replay, read-only access and bounded in-flight ownership passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
