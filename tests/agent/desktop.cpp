#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include <ruvia/core/EventLoopAttachment.h>

#include "features/desktop/desktop.transport.h"
#include "features/desktop/session.runtime.h"
#include "features/desktop/software_video.transport.h"
#include "platform/windows/credential.transport.h"

namespace {
using namespace ultraterimal;
asio::awaitable<void> exerciseBridge(agent::DesktopSession& session, std::string credential) {
    auto result = co_await session.run(std::move(credential));
    if (!result.processId || result.exitCode != 1 || result.forcedExit || !result.terminal.has_session_error()) {
        throw std::runtime_error("Desktop session did not join its child after disconnect");
    }
    std::cout << "Bridge disconnect joined desktop child\n";
    throw std::runtime_error("Expected remote disconnect");
}
asio::awaitable<void> exerciseBrowser(agent::DesktopTransport& transport, std::string credential, agent::EncodedVideoFrame frame, std::filesystem::path observations) {
    co_await transport.connect(std::move(credential));
    std::cerr << "Synthetic peer connected\n";
    wire::Envelope config;
    config.set_generation(1);
    config.mutable_video_config()->set_codec(frame.codec);
    config.mutable_video_config()->set_width(32);
    config.mutable_video_config()->set_height(32);
    co_await transport.sendVideo(std::move(config));
    std::cerr << "Synthetic video configuration sent\n";
    wire::Envelope image;
    image.set_generation(1);
    image.set_frame_id(1);
    image.mutable_video_frame()->set_data(frame.annexB.data(), frame.annexB.size());
    image.mutable_video_frame()->set_key_frame(frame.keyFrame);
    image.mutable_video_frame()->set_timestamp_microseconds(frame.timestampMicroseconds);
    co_await transport.sendVideo(std::move(image));
    std::cerr << "Synthetic first frame sent\n";
    std::ofstream output(observations, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Cannot open synthetic browser observations");
    }
    std::cerr << "Synthetic input observer ready\n";
    std::uint64_t previousSequence = 0;
    unsigned keys = 0;
    while (keys < 24) {
        const auto message = co_await transport.receiveInput();
        if (message.sequence() <= previousSequence) {
            throw std::runtime_error("Browser input sequence was not strictly increasing");
        }
        previousSequence = message.sequence();
        if (message.has_input_event() && message.input_event().kind() == wire::InputEvent::KEY && message.input_event().code() == 0x87) {
            if (message.generation() != 1 || message.input_event().pressed() != (keys % 2 == 0)) {
                throw std::runtime_error("Synthetic browser key transitions arrived out of order");
            }
            output << message.sequence() << ' ' << message.input_event().pressed() << std::endl;
            if (!output) {
                throw std::runtime_error("Cannot record synthetic browser observation");
            }
            ++keys;
        }
    }
    bool disconnected = false;
    try {
        co_await transport.watchVideo();
    } catch (const std::exception&) {
        disconnected = true;
    }
    if (!disconnected) {
        throw std::runtime_error("Synthetic browser did not release its video channel");
    }
    std::cout << "24 ordered browser F24 transitions reached the Agent native desktop transport\n";
}
asio::awaitable<void> exercise(agent::DesktopTransport& transport, std::string credential, agent::EncodedVideoFrame first, agent::EncodedVideoFrame refreshed) {
    co_await transport.connect(std::move(credential));
    wire::Envelope cursor;
    cursor.mutable_cursor_update()->set_visible(false);
    co_await transport.sendCursor(std::move(cursor));
    wire::Envelope config;
    config.set_generation(1);
    config.mutable_video_config()->set_codec(first.codec);
    config.mutable_video_config()->set_width(32);
    config.mutable_video_config()->set_height(32);
    co_await transport.sendVideo(std::move(config));
    for (unsigned i = 0; i < 2; ++i) {
        if (i) {
            auto input = co_await transport.receiveInput();
            if (!input.has_request_refresh()) {
                throw std::runtime_error("Missing refresh request");
            }
        }
        const auto& encoded = i ? refreshed : first;
        wire::Envelope frame;
        frame.set_generation(1);
        frame.set_frame_id(i + 1);
        frame.mutable_video_frame()->set_data(encoded.annexB.data(), encoded.annexB.size());
        frame.mutable_video_frame()->set_key_frame(encoded.keyFrame);
        frame.mutable_video_frame()->set_timestamp_microseconds(encoded.timestampMicroseconds);
        co_await transport.sendVideo(std::move(frame));
    }
    bool disconnected = false;
    try {
        co_await transport.watchVideo();
    } catch (const std::exception&) {
        disconnected = true;
    }
    if (!disconnected) {
        throw std::runtime_error("Desktop disconnect was not observed");
    }
    // Closing either channel makes the other unusable immediately.
    bool inputClosed = false;
    try {
        (void)co_await transport.receiveInput();
    } catch (const std::exception&) {
        inputClosed = true;
    }
    if (!inputClosed) {
        throw std::runtime_error("Desktop input survived video disconnect");
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3 && argc != 4) {
        return 2;
    }
    try {
        using namespace ultraterimal;
        if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
            throw std::runtime_error("Cannot read binary session grant");
        }
        const auto config = agent::loadConnectionConfig(argv[1]);
        const std::string mode = argc >= 3 ? argv[2] : "";
        const bool bridge = mode == "--bridge" && argc == 4;
        const bool browserFixture = mode == "--browser-fixture" && argc == 4;
        if (!mode.empty()) {
            if (mode != "--fixture" && mode != "--fixture-cancel" && !bridge && !browserFixture) {
                throw std::invalid_argument("Invalid fixture mode");
            }
            if (!browserFixture) {
                agent::saveCredential(config.credentialFile, std::string(64, 'a'));
            }
        }
        std::string input(protocol::kMaximumControlBytes + 1, '\0');
        std::cin.read(input.data(), static_cast<std::streamsize>(input.size()));
        input.resize(static_cast<std::size_t>(std::cin.gcount()));
        if (input.size() > protocol::kMaximumControlBytes) {
            throw std::runtime_error("Oversized session grant");
        }
        auto grant = protocol::decodeMessage(input);
        if (!grant) {
            throw std::runtime_error("Invalid session grant");
        }
        std::optional<agent::EncodedVideoFrame> first, refreshed;
        if (!bridge) {
            agent::SoftwareVideoEncoder encoder({32, 32});
            std::vector<std::uint8_t> pixels(32 * 32 * 4, 100);
            first = encoder.encode(pixels, 32 * 4, 0, true).value();
            refreshed = encoder.encode(pixels, 32 * 4, 33333, true).value();
            if (!first || !refreshed || !first->keyFrame || !refreshed->keyFrame) {
                throw std::runtime_error("Synthetic encoding failed");
            }
        }
        asio::io_context io;
        auto attachment = ruvia::attachEventLoop(io);
        std::unique_ptr<agent::DesktopSession> desktopSession;
        if (bridge) {
            DWORD windowsSession = 0;
            if (!ProcessIdToSessionId(GetCurrentProcessId(), &windowsSession) || !windowsSession) {
                throw std::runtime_error("Interactive test session required");
            }
            desktopSession = std::make_unique<agent::DesktopSession>(attachment.loop(), config, *grant, std::filesystem::path(argv[3]), windowsSession);
        }
        agent::DesktopTransport transport(attachment.loop(), config, std::move(*grant));
        asio::steady_timer deadline(io, mode == "--fixture-cancel" ? std::chrono::milliseconds(3000) : browserFixture ? std::chrono::milliseconds(30000)
                                                                                                                      : std::chrono::milliseconds(20000));
        bool timedOut = false;
        deadline.async_wait([&](std::error_code error) {
            if (!error) {
                timedOut = true;
                if (desktopSession) {
                    desktopSession->stop();
                }
                transport.cancel();
            }
        });
        std::exception_ptr failure;
        auto operation = bridge           ? exerciseBridge(*desktopSession, agent::loadCredential(config.credentialFile))
                         : browserFixture ? exerciseBrowser(transport, agent::loadCredential(config.credentialFile), std::move(*first), argv[3])
                                          : exercise(transport, agent::loadCredential(config.credentialFile), std::move(*first), std::move(*refreshed));
        asio::co_spawn(io, std::move(operation), [&](std::exception_ptr error) {
            failure = error;
            deadline.cancel();
            attachment.stop();
        });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (timedOut) {
            throw std::runtime_error("Desktop transport test timed out");
        }
        std::cout << "Agent desktop pairing, OpenH264 frames, refresh and paired cancellation passed\n";
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Agent desktop transport verification failed\n";
        return 1;
    }
}
