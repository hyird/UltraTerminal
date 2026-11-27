#pragma once
#include <filesystem>
#include <fstream>
#include <model.h>
#include <optional>
#include <string>
#include <string_view>

#include "ultraterimal/protocol/update.protocol.h"

namespace ultraterimal::server {
RUVIA_MODEL(AgentReleaseManifest,
            RUVIA_REQUIRED_FIELD(file, ruvia::string),
            RUVIA_REQUIRED_FIELD(version, ruvia::string),
            RUVIA_REQUIRED_FIELD(sha256, ruvia::string));
struct AgentRelease {
    wire::AgentUpdate update;
    std::filesystem::path file;
};
inline std::optional<AgentRelease> agentRelease(const std::filesystem::path& releaseDir, std::string_view platform = "windows-x64") {
    if (releaseDir.empty()) {
        return {};
    }
    const bool isLinux = platform == "linux-x64";
    if (!isLinux && platform != "windows-x64") {
        return {};
    }
    try {
        std::ifstream stream(releaseDir / (isLinux ? "linux-agent.json" : "windows-agent.json"), std::ios::binary);
        std::string bytes(4097, '\0');
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(stream.gcount()));
        if (bytes.empty() || bytes.size() > 4096) {
            return {};
        }
        auto parsed = ruvia::parse_json<AgentReleaseManifest>(bytes);
        if (!parsed.has_value()) {
            return {};
        }
        const auto manifest = std::move(parsed).take_value();
        std::string name(manifest.get<"file">().view());
        if (name.starts_with("/assets/")) {
            name = name.substr(8);
        }
        const bool validName = isLinux ? (name.starts_with("ultraterimal-agent-linux-x64-") && !name.ends_with(".msi")) : name.ends_with(".msi");
        if (!validName || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos) {
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
        release.update.set_version(std::string(manifest.get<"version">().view()));
        release.update.set_sha256(std::string(manifest.get<"sha256">().view()));
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
