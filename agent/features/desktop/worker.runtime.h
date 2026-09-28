#pragma once

#include <asio/awaitable.hpp>
#include <cstdint>

#include <ruvia/core/EventLoop.h>

namespace ultraterimal::agent {
class NamedPipeTransport;
// The launcher must authenticate the pipe peer and the current Windows session
// before calling this entry point. The pipe and loop outlive the awaited task.
asio::awaitable<void> runDesktopWorker(ruvia::EventLoop loop, NamedPipeTransport& pipe, std::uint32_t windowsSession);
} // namespace ultraterimal::agent
