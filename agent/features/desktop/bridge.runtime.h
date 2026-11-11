#pragma once
#include <asio/awaitable.hpp>
#include <cstdint>
#include <string>

#include <ruvia/core/blocking_pool.h>
#include <ruvia/core/event_loop.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
class DesktopWorkerChannel;
class NamedPipeTransport;
// Both worker endpoints must already be connected and owned by this event loop.
// Pair native channels before launching the desktop process so pairing cannot exhaust its
// startup timeout. The caller retains the child process and joins it afterwards.
// This operation cancels only the worker channel and IPC on exit. The relay
// owns native I/O independently. No credentials or tickets cross the pipe.
// The process owns the Worker-bound blocking pool through session shutdown.
// Returns a local terminal message for the control-channel owner to report.
asio::awaitable<wire::Envelope> bridgeDesktop(ruvia::event_loop loop, DesktopWorkerChannel& remote,
                                              NamedPipeTransport& pipe, std::string sessionId, std::uint32_t windowsSession,
                                              std::uint32_t displayIndex, bool writeControl, std::uint32_t desktopInputContract);
} // namespace ultraterimal::agent
