#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include <ruvia/core/Channel.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// One producer and one writer, owned by one Worker. Budget includes the active write.
class DesktopSendQueue {
  public:
    DesktopSendQueue(ruvia::WorkerHandle worker, std::size_t capacity, std::size_t maximumBytes)
        : queue_(ruvia::makeChannel<wire::Envelope>(worker, {.capacity = capacity})),
          space_(ruvia::makeChannel<bool>(std::move(worker), {.capacity = 1})), capacity_(capacity), maximumBytes_(maximumBytes) {}
    ruvia::Task<bool> send(wire::Envelope message) {
        const auto bytes = message.ByteSizeLong();
        if (bytes > maximumBytes_) {
            co_return false;
        }
        while (!closed_ && (count_ >= capacity_ || bytes > maximumBytes_ - bytes_)) {
            if (!(co_await space_.second.receive()).hasValue()) {
                co_return false;
            }
        }
        if (closed_) {
            co_return false;
        }
        if (!queue_.first.send(std::move(message)).accepted()) {
            co_return false;
        }
        ++count_;
        bytes_ += bytes;
        co_return true;
    }
    ruvia::Task<std::optional<wire::Envelope>> receive() {
        auto received = co_await queue_.second.receive();
        if (!received.hasValue() || closed_) {
            co_return std::nullopt;
        }
        co_return std::move(received).takeValue();
    }
    void complete(std::size_t bytes) {
        --count_;
        bytes_ -= bytes;
        (void)space_.first.send(true);
    }
    void close() {
        closed_ = true;
        queue_.first.close();
        space_.first.close();
    }

  private:
    std::pair<ruvia::ChannelSender<wire::Envelope>, ruvia::ChannelReceiver<wire::Envelope>> queue_;
    std::pair<ruvia::ChannelSender<bool>, ruvia::ChannelReceiver<bool>> space_;
    std::size_t capacity_, maximumBytes_, count_ = 0, bytes_ = 0;
    bool closed_ = false;
};
} // namespace ultraterimal::agent
