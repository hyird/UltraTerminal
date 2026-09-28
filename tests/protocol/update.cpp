#include <stdexcept>

#include "ultraterimal/protocol/native.protocol.h"
#include "ultraterimal/protocol/update.protocol.h"
int main() {
    using namespace ultraterimal;
    const auto require = [](bool condition) { if (!condition) { throw std::runtime_error("Update contract failed"); } };
    wire::AgentUpdate update;
    update.set_version("1.0.4");
    update.set_sha256(std::string(64, 'a'));
    update.set_size(100000);
    require(protocol::newerUpdate(update, "1.0.3"));
    require(!protocol::newerUpdate(update, "1.0.4"));
    require(!protocol::newerUpdate(update, "2.0.0"));
    for (auto version : {"", "1.2", "1.2.3.4", "-1.2.3", "01.2.3", "1.2.65536", "1.x.3"}) {
        require(!protocol::updateVersion(version));
    }
    update.set_size(protocol::kMaximumUpdateBytes + 1);
    require(!protocol::validUpdate(update));
    update.set_size(100000);
    update.set_sha256(std::string(64, 'g'));
    require(!protocol::validUpdate(update));
    update.set_sha256(std::string(64, 'a'));
    wire::AgentUpdateChunk chunk;
    chunk.set_offset(20);
    chunk.set_data("abc");
    require(protocol::validUpdateChunk(chunk, 20, 23));
    require(!protocol::validUpdateChunk(chunk, 19, 23));
    require(!protocol::validUpdateChunk(chunk, 20, 22));
    require(!protocol::validUpdateChunk(chunk, 20, 19));
    chunk.set_data(std::string(protocol::kUpdateChunkBytes + 1, 'a'));
    require(!protocol::validUpdateChunk(chunk, 20, 100000));
    wire::Envelope command;
    command.set_protocol_version(protocol::kVersion);
    command.set_sequence(1);
    *command.mutable_agent_update() = update;
    require(protocol::encodeMessage(command).has_value());
    command.set_session_id("desktop");
    require(!protocol::encodeMessage(command));
    wire::NativeConnect authentication;
    authentication.set_protocol_version(protocol::kVersion);
    authentication.set_purpose(wire::NativeConnect::AGENT_UPDATE);
    authentication.set_credential(std::string(64, 'a'));
    require(protocol::decodeNativeConnect(authentication.SerializeAsString()).has_value());
    authentication.clear_credential();
    require(!protocol::decodeNativeConnect(authentication.SerializeAsString()));
}
