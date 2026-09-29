#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Each worker starts its counters at one. The native/browser connection does
// not restart: generation and probe namespaces must remain strictly monotonic.
class DesktopContinuity {
  public:
    void beginWorker() {
        generationOffset_ = generation_;
        probeOffset_ = probe_;
        resetProbe_ = 0;
        acknowledged_ = displayed_ = generationOffset_ == 0;
    }
    bool outgoing(wire::Envelope& message) {
        if (message.generation()) {
            if (message.generation() > std::numeric_limits<std::uint64_t>::max() - generationOffset_) {
                return false;
            }
            message.set_generation(message.generation() + generationOffset_);
            generation_ = std::max(generation_, message.generation());
        }
        if (message.has_desktop_probe()) {
            auto& probe = *message.mutable_desktop_probe();
            if (!probe.nonce() || probe.nonce() > std::numeric_limits<std::uint32_t>::max() - probeOffset_ || probe.accepted_response_nonce() >= probe.nonce()) {
                return false;
            }
            probe.set_nonce(probe.nonce() + probeOffset_);
            if (probe.nonce() <= probe_) {
                return false;
            }
            probe_ = probe.nonce();
            if (probe.accepted_response_nonce()) {
                acceptedProbe_ = std::max(acceptedProbe_, probe.accepted_response_nonce() + probeOffset_);
            }
            // The browser's acknowledged watermark also belongs to the native
            // connection, not the new worker's initially empty probe history.
            probe.set_accepted_response_nonce(acceptedProbe_);
            if (!resetProbe_) {
                resetProbe_ = probe_;
                probe.set_reset_input(generationOffset_ != 0);
            }
        }
        return true;
    }
    // False means an obsolete command, not a malformed wire message. Native
    // validation still occurs before this filter, including sequence checks.
    bool incoming(wire::Envelope& message) {
        if (message.has_desktop_probe()) {
            auto& probe = *message.mutable_desktop_probe();
            if (probe.nonce() <= probeOffset_ || probe.nonce() > probe_) {
                return false;
            }
            acknowledged_ = acknowledged_ || probe.nonce() == resetProbe_;
            probe.set_nonce(probe.nonce() - probeOffset_);
            return true;
        }
        if (message.has_quality_feedback()) {
            auto& feedback = *message.mutable_quality_feedback();
            if (feedback.displayed_generation() <= generationOffset_ || feedback.displayed_generation() > generation_ || !feedback.displayed_frame()) {
                return false;
            }
            displayed_ = true;
            feedback.set_displayed_generation(feedback.displayed_generation() - generationOffset_);
            return true;
        }
        if (!acknowledged_ || !displayed_) {
            return false;
        }
        if (message.has_input_event()) {
            if (message.generation() <= generationOffset_ || message.generation() > generation_) {
                return false;
            }
            message.set_generation(message.generation() - generationOffset_);
        }
        return true;
    }

  private:
    std::uint64_t generation_ = 0, generationOffset_ = 0;
    std::uint32_t probe_ = 0, probeOffset_ = 0, resetProbe_ = 0, acceptedProbe_ = 0;
    bool acknowledged_ = false, displayed_ = false;
};
} // namespace ultraterimal::agent
