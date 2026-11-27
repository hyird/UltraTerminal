#include <iostream>

#include "platform/platform.transport.h"

int main() {
    try {
        for (int i = 0; i < 3; ++i) {
            const auto result = ultraterimal::agent::checkLocalTerminal();
            if (result.exitCode != 0 || !result.markerReceived) {
                return 1;
            }
        }
        std::cout << "ConPTY creation, resize, output, exit and repeated cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
