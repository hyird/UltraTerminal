#pragma once
#include <asio/awaitable.hpp>
#include <cstdint>
#include <string>

#include <ruvia/core/BlockingPool.h>
#include <ruvia/core/EventLoop.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
class DesktopTransport;
class NamedPipeTransport;
// Both transports must already be connected and owned by this event loop.
// Pair native channels before launching the desktop process so pairing cannot exhaust its
// startup timeout. The caller retains the child process and joins it afterwards.
// This operation owns all readers/writers until completion and cancels both
// transports on every exit. No credentials or channel tickets cross the pipe.
// The session owns systemPool; await this operation before destroying the pool.
// Returns a local terminal message for the control-channel owner to report.
asio::awaitable<wire::Envelope> bridgeDesktop(ruvia::EventLoop loop, ruvia::BlockingPool& systemPool, DesktopTransport& remote,
                                              NamedPipeTransport& pipe, std::string sessionId, std::uint32_t windowsSession,
                                              std::uint32_t displayIndex, bool writeControl, std::uint32_t desktopInputContract);
} // namespace ultraterimal::agent
