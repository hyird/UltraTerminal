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
        require(!recovery.ready(now + 499ms) && recovery.ready(now + 500ms), "First retry backoff incorrect");
        require(recovery.restart(now + 500ms), "Second recovery rejected");
        require(!recovery.ready(now + 1499ms) && recovery.ready(now + 1500ms), "Second retry backoff incorrect");
        require(recovery.restart(now + 2s), "Third recovery rejected");
        require(!recovery.ready(now + 3999ms) && recovery.ready(now + 4000ms), "Third retry backoff incorrect");
        require(!recovery.restart(now + 9999ms), "Recovery budget allowed a restart storm");
        require(recovery.restart(now + 10s), "Recovery budget did not reset after its window");
        require(!recovery.ready(now + 10499ms) && recovery.ready(now + 10500ms), "New recovery window retained old backoff");
        std::cout << "Capture recovery budget, bounded backoff and window reset passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
