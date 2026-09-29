#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace ultraterimal::agent {
class ConsoleSessionEvents;
using ServiceTask = std::function<void(std::stop_token, ConsoleSessionEvents&)>;
void dispatchService(ServiceTask task);
std::filesystem::path serviceConfigPath();
std::filesystem::path agentExecutablePath();
void installService();
void uninstallService();
void startService();
void stopService();
std::string serviceStatus();
} // namespace ultraterimal::agent
