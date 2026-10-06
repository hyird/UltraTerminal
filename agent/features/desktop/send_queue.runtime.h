#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

#include <ruvia/core/channel.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// One owner-loop writer. Budget includes the active write; its immutable
// sidecar survives header/body mutation and is released only by this queue.
class DesktopSendQueue {
  public:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        wire::Envelope message;
        Clock::time_point receivedAt, enqueuedAt;
        bool samplePipeline;
        std::uint32_t optimizationContract;
        Entry(Entry&& other) noexcept
            : message(std::move(other.message)), receivedAt(other.receivedAt), enqueuedAt(other.enqueuedAt), samplePipeline(other.samplePipeline), optimizationContract(other.optimizationContract), owner_(std::exchange(other.owner_, nullptr)), bytes_(other.bytes_) {}
        Entry& operator=(Entry&&) = delete;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        std::size_t accountedBytes() const { return bytes_; }

      private:
        friend class DesktopSendQueue;
        Entry(wire::Envelope value, DesktopSendQueue* owner, std::size_t bytes, bool sample, Clock::time_point received, std::uint32_t contract)
            : message(std::move(value)), receivedAt(received), enqueuedAt(Clock::now()), samplePipeline(sample), optimizationContract(contract), owner_(owner), bytes_(bytes) {}
        DesktopSendQueue* owner_;
        std::size_t bytes_;
    };
    DesktopSendQueue(ruvia::execution_handle worker, std::size_t capacity, std::size_t maximumBytes)
        : queue_(ruvia::make_channel<Entry>(worker, {.capacity_ = capacity})),
          space_(ruvia::make_channel<bool>(std::move(worker), {.capacity_ = 1})), capacity_(capacity), maximumBytes_(maximumBytes) {}
    // Best effort diagnostics never suspend a video writer for control space.
    bool trySend(wire::Envelope message, bool samplePipeline = false, std::uint32_t optimizationContract = 0) {
        return enqueue(std::move(message), samplePipeline, Clock::now(), optimizationContract);
    }
    ruvia::task<bool> send(wire::Envelope message, bool samplePipeline = false, std::uint32_t optimizationContract = 0) {
        const auto receivedAt = Clock::now();
        const auto bytes = message.ByteSizeLong();
        if (bytes > maximumBytes_) {
            co_return false;
        }
        while (!closed_ && (count_ >= capacity_ || bytes > maximumBytes_ - bytes_)) {
            if (!(co_await space_.second.receive()).has_value()) {
                co_return false;
            }
        }
        co_return enqueue(std::move(message), samplePipeline, receivedAt, optimizationContract);
    }
    ruvia::task<std::optional<Entry>> receive() {
        auto received = co_await queue_.second.receive();
        if (!received.has_value() || closed_) {
            co_return std::nullopt;
        }
        co_return std::move(received).take_value();
    }
    void complete(Entry& entry) {
        if (entry.owner_ != this) {
            throw std::logic_error("Desktop queue completion has wrong owner or was already released");
        }
        entry.owner_ = nullptr;
        --count_;
        bytes_ -= entry.bytes_;
        (void)space_.first.send(true);
    }
    std::size_t pendingBytes() const { return bytes_; }
    std::size_t pendingCount() const { return count_; }
    void close() {
        closed_ = true;
        queue_.first.close();
        space_.first.close();
    }

  private:
    bool enqueue(wire::Envelope message, bool samplePipeline, Clock::time_point receivedAt, std::uint32_t optimizationContract) {
        const auto bytes = message.ByteSizeLong();
        if (closed_ || count_ >= capacity_ || bytes > maximumBytes_ - bytes_) {
            return false;
        }
        if (!queue_.first.send(Entry(std::move(message), this, bytes, samplePipeline, receivedAt, optimizationContract)).accepted()) {
            return false;
        }
        ++count_;
        bytes_ += bytes;
        return true;
    }
    std::pair<ruvia::channel_sender<Entry>, ruvia::channel_receiver<Entry>> queue_;
    std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>> space_;
    std::size_t capacity_, maximumBytes_, count_ = 0, bytes_ = 0;
    bool closed_ = false;
};
} // namespace ultraterimal::agent
