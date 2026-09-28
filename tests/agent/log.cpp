#include <iostream>
#include <stdexcept>

#include "features/logging/log.runtime.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() {
    try {
        using namespace ultraterimal;
        agent::AgentLogBuffer logs("runtime-1");
        require(!logs.next(), "Empty queue produced a batch");
        logs.append(wire::AgentLog::STARTED, 1);
        const auto original = logs.next()->SerializeAsString();
        require(!logs.acknowledge(2), "Accepted an acknowledgment for an unsent event");
        for (std::size_t i = 0; i < agent::AgentLogBuffer::kCapacity + 9; ++i) {
            logs.append(wire::AgentLog::CONNECT_FAILED, 2 + i);
        }
        require(logs.next()->SerializeAsString() == original, "Reconnect/overflow changed the unacknowledged batch");
        require(logs.acknowledge(1), "Failed to acknowledge first batch");
        const auto* batch = logs.next();
        require(batch->entries_size() == 16 && batch->dropped() == 9, "Queue overflow accounting failed");
        require(batch->entries(0).sequence() == 11, "Oldest pending events were not evicted");
        std::uint64_t previous = 1;
        while ((batch = logs.next())) {
            wire::Envelope message;
            message.set_protocol_version(protocol::kVersion);
            message.set_sequence(1);
            *message.mutable_agent_logs() = *batch;
            require(protocol::encodeMessage(message).has_value(), "Produced invalid log message");
            require(batch->entries(0).sequence() > previous, "Event order regressed");
            previous = batch->entries().rbegin()->sequence();
            require(logs.acknowledge(previous), "Batch acknowledgment failed");
        }
        require(!logs.acknowledge(previous), "Accepted duplicate acknowledgment");
        std::cout << "Agent log buffering passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
