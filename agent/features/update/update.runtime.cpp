#include "update.runtime.h"

#include <array>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_future.hpp>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <openssl/evp.h>
#include <ruvia/core/asio_task.h>
#include <ruvia/core/execution_domain.h>

#include "features/connection/connection.transport.h"
#include "platform/windows/credential.transport.h"
#include "platform/windows/update.transport.h"
namespace ultraterimal::agent {
namespace {
asio::awaitable<void> download(ruvia::execution_handle loop, const std::filesystem::path& root, wire::AgentUpdate update) {
    auto config = loadConnectionConfig(root / "agent.json");
    auto transport = std::make_shared<DeviceTransport>(loop, config, protocol::kMaximumControlBytes, wire::NativeConnect::AGENT_UPDATE);
    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(std::chrono::minutes(15));
    deadline.async_wait([transport](std::error_code error) { if (!error) { transport->cancel(); } });
    try {
        co_await transport->connect(loadCredential(config.credentialFile));
        co_await transport->send(update.SerializeAsString());
        std::ofstream file(root / ".update/package.msi", std::ios::binary | std::ios::trunc);
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!file || !digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("Cannot stage package");
        }
        std::uint64_t offset = 0;
        while (offset < update.size()) {
            const auto bytes = co_await transport->receive();
            wire::AgentUpdateChunk chunk;
            if (bytes.size() > protocol::kUpdateChunkBytes + 64 || !chunk.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())) || !protocol::validUpdateChunk(chunk, offset, update.size())) {
                throw std::runtime_error("Invalid package chunk");
            }
            // This dedicated updater process performs no desktop or service work.
            file.write(chunk.data().data(), static_cast<std::streamsize>(chunk.data().size()));
            if (!file || EVP_DigestUpdate(digest.get(), chunk.data().data(), chunk.data().size()) != 1) {
                throw std::runtime_error("Cannot stage package chunk");
            }
            offset += chunk.data().size();
        }
        file.close();
        std::array<unsigned char, 32> hash{};
        unsigned count = 0;
        if (!file || EVP_DigestFinal_ex(digest.get(), hash.data(), &count) != 1 || count != hash.size()) {
            throw std::runtime_error("Package digest failed");
        }
        std::string hex;
        for (auto byte : hash) {
            hex += "0123456789abcdef"[byte >> 4];
            hex += "0123456789abcdef"[byte & 15];
        }
        if (hex != update.sha256()) {
            throw std::runtime_error("Package digest mismatch");
        }
        co_await transport->send("verified");
        deadline.cancel();
        co_await transport->close();
    } catch (...) {
        deadline.cancel();
        transport->cancel();
        throw;
    }
}

} // namespace
int runAgentUpdate() {
    WindowsAgentUpdate update;
    try {
        asio::io_context io;
        ruvia::execution_domain attachment(io);
        std::exception_ptr failure;
        asio::co_spawn(attachment.executor(), download(attachment.handle(), update.root(), update.request()), [&](std::exception_ptr error) { failure = error; attachment.request_stop(); });
        io.run();
        io.restart();
        auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
        io.run();
        retired.get();
        if (failure) {
            std::rethrow_exception(failure);
        }
        return update.install();
    } catch (...) {
        update.failed(13); // ERROR_INVALID_DATA: download, chunk or digest validation failed.
        return 1;
    }
}
} // namespace ultraterimal::agent
