#include <iostream>
#include <stdexcept>
#include <string>

#include "ultraterimal/protocol/native.protocol.h"

int main() {
    using namespace ultraterimal;
    try {
        const auto require = [](bool value) { if (!value) { throw std::runtime_error("Native authentication contract failed"); } };
        wire::NativeConnect request;
        request.set_protocol_version(protocol::kVersion);
        request.set_purpose(wire::NativeConnect::CONTROL);
        request.set_credential(std::string(64, 'a'));
        const auto valid = [&] { return protocol::decodeNativeConnect(request.SerializeAsString()).has_value(); };
        require(valid());
        request.set_purpose(wire::NativeConnect::DESKTOP_CHANNEL);
        require(valid());
        request.set_device_name("unexpected");
        require(!valid());
        request.clear_device_name();
        request.set_credential(std::string(64, 'g'));
        require(!valid());
        request.clear_credential();
        request.set_purpose(wire::NativeConnect::ENROLLMENT);
        request.set_enrollment_code(std::string(64, 'b'));
        request.set_device_name("device");
        require(valid());
        request.set_device_name(std::string(129, 'x'));
        require(!valid());
        request.set_device_name(std::string("bad\0name", 8));
        require(!valid());
        request.set_device_name("device");
        request.set_protocol_version(protocol::kVersion + 1);
        require(!valid());
        require(!protocol::decodeNativeConnect(std::string(4097, 'x')));
        require(!protocol::decodeNativeConnect("\xff"));
        wire::NativeAccepted response;
        response.set_protocol_version(protocol::kVersion);
        const auto accepted = [&](bool enrollment) { return protocol::decodeNativeAccepted(response.SerializeAsString(), enrollment).has_value(); };
        require(accepted(false) && accepted(true));
        response.set_device_id("secret-on-rejection");
        require(!accepted(false));
        response.set_accepted(true);
        require(accepted(false) && !accepted(true));
        response.set_credential(std::string(64, 'c'));
        require(accepted(true) && !accepted(false));
        response.set_device_id("bad\r\nidentity");
        require(!accepted(true));
        std::cout << "Native authentication bounds, purpose and identity isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
