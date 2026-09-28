#include <chrono>
#include <iostream>
#include <stdexcept>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>

#include "features/desktop/send_queue.runtime.h"

namespace {
using ultraterimal::agent::DesktopSendQueue;
using ultraterimal::wire::Envelope;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
ruvia::Task<void> enqueue(DesktopSendQueue& queue, Envelope message, bool& completed, bool& accepted) {
    accepted = co_await queue.send(std::move(message));
    completed = true;
}
ruvia::Task<void> exercise(ruvia::WorkerHandle worker) {
    DesktopSendQueue video(worker, 1, 1024), control(worker, 4, 1024);
    Envelope frame;
    frame.set_sequence(1);
    frame.mutable_video_frame()->set_data(std::string(900, 'v'));
    require(co_await video.send(frame), "First video frame rejected");
    auto active = co_await video.receive();
    require(active && active->sequence() == 1, "Video ordering failed");
    frame.set_sequence(2);
    bool completed = false, accepted = false;
    ruvia::TaskScope writes(worker);
    writes.spawn(enqueue(video, frame, completed, accepted));
    (void)co_await ruvia::sleepFor(worker, std::chrono::milliseconds(5));
    require(!completed, "Budget did not include active video write");
    Envelope probe;
    probe.mutable_desktop_probe()->set_nonce(1);
    require(co_await control.send(probe), "Video congestion blocked control enqueue");
    auto reply = co_await control.receive();
    require(reply && reply->desktop_probe().nonce() == 1 && !completed, "Control waited for video completion");
    control.complete(reply->ByteSizeLong());
    video.complete(active->ByteSizeLong());
    co_await writes.join();
    require(completed && accepted, "Video backpressure did not resume");
    active = co_await video.receive();
    require(active && active->sequence() == 2, "Video frame lost under backpressure");
    completed = accepted = false;
    ruvia::TaskScope closing(worker);
    closing.spawn(enqueue(video, frame, completed, accepted));
    (void)co_await ruvia::sleepFor(worker, std::chrono::milliseconds(5));
    video.close();
    co_await closing.join();
    require(completed && !accepted, "Close did not wake blocked producer");
    video.complete(active->ByteSizeLong());
    require(!(co_await video.receive()), "Closed queue remained readable");
    frame.mutable_video_frame()->set_data(std::string(2048, 'v'));
    require(!(co_await control.send(frame)), "Oversized message bypassed byte budget");
    control.close();
}
} // namespace
int main() {
    try {
        ruvia::EventLoopPool pool({.loopCount = 1});
        pool.start();
        auto task = pool.loop(0).start(exercise(pool.loop(0).handle()));
        task.get();
        pool.join();
        std::cout << "Independent control/video writers, in-flight budget, FIFO backpressure and close passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
