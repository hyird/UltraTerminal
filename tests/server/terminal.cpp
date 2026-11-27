#include <event_loop.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "features/terminal/terminal.runtime.h"
#include "modules/devices/update.service.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ruvia::task<void> exercise(ruvia::worker_ref worker) {
    using namespace ultraterimal;
    server::TerminalSession session(worker);
    server::ChannelAuthorization browserAuth{"s-1", "c-1", "u-1", "d-1", wire::SSH, wire::SSH_STREAM, true};
    server::ChannelAuthorization agentAuth = browserAuth;
    auto browser = session.attach(browserAuth, protocol::ChannelSource::kBrowser);
    require(browser != nullptr, "Browser attach failed");
    require(!session.ready(), "Session ready before agent attached");
    require(session.attach(browserAuth, protocol::ChannelSource::kBrowser) == nullptr, "Duplicate browser attach accepted");
    auto agent = session.attach(agentAuth, protocol::ChannelSource::kAgent);
    require(agent != nullptr && session.ready(), "Agent attach or readiness failed");

    auto bReady = co_await browser->receive({});
    auto aReady = co_await agent->receive({});
    require(bReady && bReady->ready() && aReady && aReady->ready(), "Initial ready markers missing");

    wire::Envelope input;
    input.set_protocol_version(protocol::kVersion);
    input.set_sequence(1);
    input.set_session_id("s-1");
    input.set_channel_id("c-1");
    input.mutable_terminal_data()->set_data("uname -a\n");
    require(co_await session.forward(browser, input), "Browser terminal input rejected");
    auto forwardedIn = co_await agent->receive({});
    require(forwardedIn && !forwardedIn->ready(), "Agent did not receive terminal input");
    const auto decodedIn = protocol::decodeMessage(forwardedIn->bytes());
    require(decodedIn && decodedIn->terminal_data().data() == "uname -a\n", "Terminal input corrupted");

    wire::Envelope resize;
    resize.set_protocol_version(protocol::kVersion);
    resize.set_sequence(2);
    resize.set_session_id("s-1");
    resize.set_channel_id("c-1");
    resize.mutable_terminal_resize()->set_columns(120);
    resize.mutable_terminal_resize()->set_rows(40);
    require(co_await session.forward(browser, resize), "Terminal resize rejected");
    auto forwardedResize = co_await agent->receive({});
    require(forwardedResize && protocol::decodeMessage(forwardedResize->bytes())->terminal_resize().columns() == 120, "Terminal resize lost");

    wire::Envelope output;
    output.set_protocol_version(protocol::kVersion);
    output.set_sequence(1);
    output.set_session_id("s-1");
    output.set_channel_id("c-1");
    output.mutable_terminal_data()->set_data("Linux\n");
    require(co_await session.forward(agent, output), "Agent terminal output rejected");
    auto forwardedOut = co_await browser->receive({});
    require(forwardedOut && protocol::decodeMessage(forwardedOut->bytes())->terminal_data().data() == "Linux\n", "Agent terminal output lost");

    wire::Envelope badDirection = resize;
    badDirection.set_sequence(2);
    require(!(co_await session.forward(agent, badDirection)), "Agent resize direction unexpectedly accepted");
    require(session.closed(), "Session did not close on invalid direction");
}
} // namespace

int main() {
    try {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        const auto loop = pool.loop(0);
        auto task = loop.submit(exercise(loop.worker())).take_value();
        std::move(task).take_value();
        pool.join();

        const auto tempDir = std::filesystem::temp_directory_path() / "ultraterimal-server-linux-release-test";
        std::filesystem::remove_all(tempDir);
        std::filesystem::create_directories(tempDir / "assets");
        {
            std::ofstream bin(tempDir / "assets" / "ultraterimal-agent-linux-x64-1.2.3", std::ios::binary);
            bin << "ELF";
            std::ofstream manifest(tempDir / "linux-agent.json", std::ios::binary);
            manifest << "{\"file\":\"/assets/ultraterimal-agent-linux-x64-1.2.3\",\"version\":\"1.2.3\",\"sha256\":\"" << std::string(64, 'a') << "\"}";
        }
        const auto linuxRelease = ultraterimal::server::agentRelease(tempDir, "linux-x64");
        require(linuxRelease && linuxRelease->update.version() == "1.2.3" && linuxRelease->update.size() == 3, "Linux agent release manifest lookup failed");
        require(!ultraterimal::server::agentRelease(tempDir, "macos-arm64").has_value(), "Unsupported platform release accepted");
        std::filesystem::remove_all(tempDir);

        std::cout << "Server terminal session pairing, forwarding, direction validation and Linux release lookup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
