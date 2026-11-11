#pragma once
#include <chrono>
#include <cstdint>
#include <deque>

namespace ultraterimal::agent {
// Owner-loop state. Tokens expire on the issuing Agent's monotonic clock;
// transport buffering and controller clock changes cannot extend their life.
class DesktopInputFreshness {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kLifetime = std::chrono::seconds(2);
    void issued(std::uint32_t nonce, Clock::time_point now) {
        while (!tokens_.empty() && now - tokens_.front().sent >= kLifetime) {
            tokens_.pop_front();
        }
        if (tokens_.size() == 32) {
            tokens_.pop_front();
        }
        tokens_.push_back({nonce, now});
    }
    bool accepts(std::uint32_t nonce, Clock::time_point now) const {
        for (const auto& token : tokens_) {
            if (token.nonce == nonce) {
                return now >= token.sent && now - token.sent < kLifetime;
            }
        }
        return false;
    }
    // Expiry retires the whole stale batch. Recovery requires a fresh probe.
    void reset() { tokens_.clear(); }

  private:
    struct Token {
        std::uint32_t nonce;
        Clock::time_point sent;
    };
    std::deque<Token> tokens_;
};
} // namespace ultraterimal::agent
