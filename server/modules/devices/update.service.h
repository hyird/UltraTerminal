#pragma once
#include <filesystem>
#include <fstream>
#include <optional>

#include <boost/json.hpp>

#include "ultraterimal/protocol/update.protocol.h"

namespace ultraterimal::server {
struct AgentRelease {
    wire::AgentUpdate update;
    std::filesystem::path file;
};
inline std::optional<AgentRelease> agentRelease(const std::filesystem::path& releaseDir) {
    if (releaseDir.empty()) {
        return {};
    }
    try {
        std::ifstream stream(releaseDir / "windows-agent.json", std::ios::binary);
        std::string bytes(4097, '\0');
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(stream.gcount()));
        if (bytes.empty() || bytes.size() > 4096) {
            return {};
        }
        const auto object = boost::json::parse(bytes).as_object();
        std::string name(object.at("file").as_string());
        if (name.starts_with("/assets/")) {
            name = name.substr(8);
        }
        if (!name.ends_with(".msi") || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos) {
            return {};
        }
        const auto canonicalRoot = std::filesystem::canonical(releaseDir);
        AgentRelease release;
        const auto direct = releaseDir / name;
        const auto nested = releaseDir / "assets" / name;
        if (std::filesystem::exists(direct)) {
            release.file = std::filesystem::canonical(direct);
            if (release.file.parent_path() != canonicalRoot) {
                return {};
            }
        } else {
            release.file = std::filesystem::canonical(nested);
            if (release.file.parent_path() != std::filesystem::canonical(releaseDir / "assets")) {
                return {};
            }
        }
        release.update.set_version(std::string(object.at("version").as_string()));
        release.update.set_sha256(std::string(object.at("sha256").as_string()));
        release.update.set_size(std::filesystem::file_size(release.file));
        if (!protocol::validUpdate(release.update)) {
            return {};
        }
        return release;
    } catch (const std::exception&) {
        return {};
    }
}
} // namespace ultraterimal::server
