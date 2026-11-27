#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
inline constexpr std::size_t kMaximumTelemetryDisks = 16;
inline constexpr std::size_t kMaximumTelemetryTextBytes = 128;
inline constexpr std::size_t kMaximumTelemetryFilesystemBytes = 32;
inline constexpr std::uint32_t kMaximumTelemetryCpuCores = 4096;
inline constexpr std::uint32_t kMaximumTelemetryCpuMilli = 100000;

inline bool validPlatform(std::string_view value) noexcept {
    return value == "windows-x64" || value == "linux-x64";
}

inline std::string_view normalizePlatform(std::string_view value) noexcept {
    return value.empty() ? std::string_view("windows-x64") : value;
}

inline bool validTelemetryText(std::string_view value, std::size_t maximum, bool allowEmpty) noexcept {
    if (value.empty()) {
        return allowEmpty;
    }
    return value.size() <= maximum && value.find('\0') == std::string_view::npos;
}

inline bool validHostInfo(const wire::HostInfo& host) noexcept {
    return validTelemetryText(host.os_name(), kMaximumTelemetryTextBytes, true) &&
           validTelemetryText(host.kernel_version(), kMaximumTelemetryTextBytes, true) &&
           validTelemetryText(host.architecture(), kMaximumTelemetryTextBytes, true) &&
           validTelemetryText(host.cpu_model(), kMaximumTelemetryTextBytes, true) &&
           host.cpu_cores() <= kMaximumTelemetryCpuCores &&
           validTelemetryText(host.virtualization(), kMaximumTelemetryTextBytes, true);
}

inline bool validTelemetrySample(const wire::TelemetrySample& sample) noexcept {
    if (!sample.sampled_at_unix_milliseconds() || sample.cpu_usage_milli() > kMaximumTelemetryCpuMilli) {
        return false;
    }
    if (sample.memory_total_bytes() > 0 && sample.memory_used_bytes() > sample.memory_total_bytes()) {
        return false;
    }
    if (sample.swap_total_bytes() > 0 && sample.swap_used_bytes() > sample.swap_total_bytes()) {
        return false;
    }
    if (sample.disk_total_bytes() > 0 && sample.disk_used_bytes() > sample.disk_total_bytes()) {
        return false;
    }
    if (static_cast<std::size_t>(sample.disks_size()) > kMaximumTelemetryDisks) {
        return false;
    }
    for (const auto& disk : sample.disks()) {
        if (!validTelemetryText(disk.mount_point(), kMaximumTelemetryTextBytes, false) ||
            !validTelemetryText(disk.filesystem(), kMaximumTelemetryFilesystemBytes, true) ||
            (disk.total_bytes() > 0 && disk.used_bytes() > disk.total_bytes())) {
            return false;
        }
    }
    if (sample.has_host() && !validHostInfo(sample.host())) {
        return false;
    }
    return true;
}
} // namespace ultraterimal::protocol
