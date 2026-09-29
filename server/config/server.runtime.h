#pragma once
#include <memory>
#include <unordered_map>

#include "features/control/control.runtime.h"
#include "features/desktop/desktop.runtime.h"
#include "features/status/status.runtime.h"
#include "modules/devices/device.service.h"
#include "modules/identity/identity.service.h"
#include "modules/sessions/session.service.h"
#include "modules/telegram/telegram.service.h"
#include "server.config.h"
namespace ultraterimal::server {
struct ServerRuntime {
    ServerConfig config;
    IdentityService identities;
    DeviceService devices;
    SessionService sessions;
    TelegramService telegram;
    bool telegramConfiguring = false;
    StatusHub status;
    // Reserve the device during handshake; only a validated HELLO makes it ready.
    std::unordered_map<std::string, std::shared_ptr<ControlConnection>> controlConnections;
    std::shared_ptr<DesktopBudget> desktopBudget = std::make_shared<DesktopBudget>(256 * 1024 * 1024);
    std::unordered_map<std::string, std::shared_ptr<DesktopSession>> desktopSessions;
    explicit ServerRuntime(ServerConfig value) : config(std::move(value)), identities(config.database, config.securityKey), devices(config.database), sessions(config.database, config.securityKey), telegram(config.database, config.securityKey) {}
};
} // namespace ultraterimal::server
