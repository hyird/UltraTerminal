#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <ruvia/core/event_loop.h>
#include <ruvia/core/task_scope.h>

#include "features/desktop/optimization.runtime.h"
#include "features/desktop/pipeline.runtime.h"
#include "features/desktop/send_queue.runtime.h"

namespace {
using ultraterimal::agent::DesktopSendQueue;
using ultraterimal::wire::Envelope;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
ruvia::task<void> enqueue(DesktopSendQueue& queue, Envelope message, bool& completed, bool& accepted, std::uint32_t optimizationContract = 0) {
    accepted = co_await queue.send(std::move(message), false, optimizationContract);
    completed = true;
}
ruvia::task<void> exercise(ruvia::worker_ref worker) {
    DesktopSendQueue video(worker, 1, 1024), control(worker, 4, 1024);
    Envelope frame;
    frame.set_sequence(1);
    frame.mutable_video_frame()->set_data(std::string(900, 'v'));
    require(co_await video.send(frame), "First video frame rejected");
    auto active = co_await video.receive();
    require(active && active->message.sequence() == 1, "Video ordering failed");
    require(active->receivedAt <= active->enqueuedAt && active->enqueuedAt <= DesktopSendQueue::Clock::now(), "Queue lost receive/enqueue sidecar");
    const auto originalBytes = active->accountedBytes();
    active->message.set_session_id("headers-added-after-budget-accounting");
    active->message.set_sequence(90000000);
    require(video.pendingBytes() == originalBytes, "Mutation changed active-write accounting");
    require(!video.trySend(frame), "Diagnostic bypassed active-write capacity");
    bool wrongOwner = false;
    try {
        control.complete(*active);
    } catch (const std::logic_error&) {
        wrongOwner = true;
    }
    require(wrongOwner && video.pendingCount() == 1, "Wrong queue released someone else's write");
    frame.set_sequence(2);
    bool completed = false, accepted = false;
    {
        const auto writesWorker = worker;
        ruvia::task_scope writes(writesWorker);
        bool writesJoined = false;
        std::exception_ptr writesFailure;
        try {
            ultraterimal::agent::DesktopOptimizationGate gate(2);
            gate.request(2);
            writes.spawn(enqueue(video, frame, completed, accepted, gate.allowedContract()));
            gate.request(99); // Newer capabilities cannot rewrite a suspended enqueue's snapshot.
            (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(5));
            require(!completed, "Budget did not include active video write");
            Envelope probe;
            probe.mutable_desktop_probe()->set_nonce(1);
            require(co_await control.send(probe), "Video congestion blocked control enqueue");
            auto reply = co_await control.receive();
            require(reply && reply->message.desktop_probe().nonce() == 1 && !completed, "Control waited for video completion");
            control.complete(*reply);
            video.complete(*active);
            bool duplicate = false;
            try {
                video.complete(*active);
            } catch (const std::logic_error&) {
                duplicate = true;
            }
            require(duplicate && video.pendingBytes() == 0, "Duplicate completion underflowed queue budget");
            writesJoined = true;
            co_await writes.join();
            require(completed && accepted, "Video backpressure did not resume");
            active.reset();
            auto second = co_await video.receive();
            require(second && second->message.sequence() == 2, "Video frame lost under backpressure");
            require(second->optimizationContract == 2 && gate.allowedContract() == 0, "Queue replaced the configuration's gate snapshot with a newer request");
            require(second->enqueuedAt - second->receivedAt >= std::chrono::milliseconds(5), "Admission backpressure disappeared from queue timing sidecar");
            completed = accepted = false;
            {
                const auto closingWorker = worker;
                ruvia::task_scope closing(closingWorker);
                bool closingJoined = false;
                std::exception_ptr closingFailure;
                try {
                    closing.spawn(enqueue(video, frame, completed, accepted));
                    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(5));
                    video.close();
                    closingJoined = true;
                    co_await closing.join();
                    require(completed && !accepted, "Close did not wake blocked producer");
                    video.complete(*second);
                    require(video.pendingBytes() == 0 && video.pendingCount() == 0, "Mutated/closed writes leaked byte credit");
                    require(!(co_await video.receive()), "Closed queue remained readable");
                    frame.mutable_video_frame()->set_data(std::string(2048, 'v'));
                    require(!(co_await control.send(frame)), "Oversized message bypassed byte budget");
                    control.close();

                } catch (...) {
                    closingFailure = std::current_exception();
                }
                if (!closingJoined) {
                    closing.request_stop();
                    try {
                        co_await closing.join();
                    } catch (...) {
                        if (!closingFailure) {
                            closingFailure = std::current_exception();
                        }
                    }
                }
                if (closingFailure) {
                    std::rethrow_exception(closingFailure);
                }
            }

        } catch (...) {
            writesFailure = std::current_exception();
        }
        if (!writesJoined) {
            writes.request_stop();
            try {
                co_await writes.join();
            } catch (...) {
                if (!writesFailure) {
                    writesFailure = std::current_exception();
                }
            }
        }
        if (writesFailure) {
            std::rethrow_exception(writesFailure);
        }
    }
}
void diagnostics() {
    using ultraterimal::agent::DesktopPipelineBuffer;
    using ultraterimal::wire::DesktopPipelineSample;
    DesktopPipelineBuffer buffer;
    const auto now = DesktopPipelineBuffer::Clock::time_point(std::chrono::seconds(1));
    Envelope frame;
    frame.set_generation(7);
    frame.set_frame_id(9);
    frame.mutable_video_refinement()->set_batch_id(13);
    auto sample = DesktopPipelineBuffer::sample(frame, DesktopPipelineSample::DESKTOP_WORKER);
    require(sample && sample->generation() == 7 && sample->frame_id() == 9 && sample->batch_id() == 13, "Pipeline key lost refinement tuple");
    sample->set_ipc_write_microseconds(0);
    for (unsigned index = 0; index < 100; ++index) {
        buffer.add(*sample);
    }
    auto report = buffer.pending(now);
    require(report && report->desktop_pipeline_report().samples_size() == 32 && report->desktop_pipeline_report().dropped_samples() == 68, "Diagnostic cache is not bounded/drop-counted");
    require(!DesktopPipelineBuffer::sample(*report, DesktopPipelineSample::AGENT_RELAY), "Diagnostic recursively sampled itself");
    require(report->desktop_pipeline_report().samples(0).has_ipc_write_microseconds(), "Measured zero lost presence");
    buffer.sent(now);
    buffer.add(*sample);
    require(!buffer.pending(now + std::chrono::milliseconds(499)) && buffer.pending(now + std::chrono::milliseconds(500)), "Pipeline report exceeded event rate bound");
    require(DesktopPipelineBuffer::duration(std::chrono::seconds(61)) == 60000000, "Diagnostic duration overflowed schema bound");
    buffer.clear();
    require(!buffer.pending(now + std::chrono::seconds(1)), "Retired diagnostic cache survived disable");
}
} // namespace
int main() {
    try {
        diagnostics();
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        auto task = pool.loop(0).start(exercise(pool.loop(0).worker()));
        std::move(task).get();
        pool.join();
        std::cout << "Independent control/video writers, in-flight budget, FIFO backpressure and close passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
