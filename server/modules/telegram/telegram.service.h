#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modules/database/database.config.h"
#include "modules/database/database.service.h"
#include "modules/identity/identity.service.h"
#include "modules/sessions/session.types.h"

namespace ultraterimal::server {
struct TelegramState {
    std::string botUsername;
    std::string telegramId;
};
struct TelegramConfig {
    std::string token, username, webhookSecret, generation;
};
struct ApprovalInfo {
    std::string id, deviceId, state;
    bool writeControl = false;
    std::int64_t expiresAt = 0;
};
struct ApprovalDispatch {
    ApprovalInfo approval;
    std::string botToken, username, deviceName;
    std::int64_t ownerTelegramId = 0;
};
class TelegramService {
  public:
    TelegramService(DatabaseConfig database, std::string key) : database_(std::move(database)), key_(std::move(key)) {}
    TelegramState state(std::string_view userId) const;
    std::optional<TelegramConfig> configuration() const;
    bool configure(const UserIdentity& actor, const TelegramConfig& config) const;
    bool disable(const UserIdentity& actor) const;
    std::optional<std::string> beginBinding(const UserIdentity& user) const;
    std::optional<std::string> bind(std::string_view generation, std::string_view token, std::int64_t telegramId) const;
    void unbind(const UserIdentity& user) const;
    std::optional<ApprovalDispatch> requestApproval(const UserIdentity& user, std::string_view deviceId, bool writeControl) const;
    void deliveryFailed(std::string_view id) const;
    std::optional<std::string> decide(std::string_view generation, std::string_view id, std::int64_t telegramId, bool accept) const;
    std::vector<ApprovalInfo> approvals(const UserIdentity& user) const;
    void cancel(const UserIdentity& user, std::string_view id) const;
    static bool consumeApproval(DatabaseTransaction& tx, const SessionRequest& request, std::int64_t revision);

  private:
    DatabaseConfig database_;
    std::string key_;
};
} // namespace ultraterimal::server
