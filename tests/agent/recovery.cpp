#include <chrono>
#include <iostream>
#include <stdexcept>

#include "features/desktop/recovery.runtime.h"

namespace {
void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
} // namespace
int main() {
    using namespace std::chrono_literals;
    using ultraterimal::agent::CaptureRecovery;
    try {
        CaptureRecovery recovery;
        const auto now = CaptureRecovery::Clock::time_point{} + 1s;
        require(recovery.ready(now), "Initial capture delayed");
        require(recovery.restart(now), "First recovery rejected");
        require(!recovery.ready(now + 249ms) && recovery.ready(now + 250ms), "First retry backoff incorrect");
        require(recovery.restart(now + 250ms), "Second recovery rejected");
        require(!recovery.ready(now + 749ms) && recovery.ready(now + 750ms), "Second retry backoff incorrect");
        require(recovery.restart(now + 1s), "Third recovery rejected");
        require(!recovery.ready(now + 1999ms) && recovery.ready(now + 2000ms), "Third retry backoff incorrect");
        for (int i = 3; i < 10; ++i) {
            require(recovery.restart(now + std::chrono::seconds(i)), "Bounded recovery attempt rejected");
        }
        require(!recovery.restart(now + 14999ms), "Recovery budget allowed a restart storm");
        require(recovery.restart(now + 15s), "Recovery budget did not reset after its window");
        require(!recovery.ready(now + 15249ms) && recovery.ready(now + 15250ms), "New recovery window retained old backoff");
        std::cout << "Capture recovery budget, bounded backoff and window reset passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
