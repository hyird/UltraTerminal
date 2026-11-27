#pragma once
#include <cstdint>
#include <db_executor.h>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <task.h>
#include <utility>
#include <vector>

#include "identity.types.h"

namespace ultraterimal::server {
std::string randomToken();
std::string tokenHash(std::string_view token);
bool validToken(std::string_view token);

class IdentityService {
  public:
    explicit IdentityService(std::string key = {}) : key_(std::move(key)) {}
    ruvia::task<void> bootstrap(ruvia::db_executor db, std::string username, std::string password) const;
    ruvia::task<std::expected<LoginSession, LoginError>> login(ruvia::db_executor db, std::string username, std::string password, std::string code = {}) const;
    ruvia::task<std::optional<UserIdentity>> authenticate(ruvia::db_executor db, std::string token) const;
    ruvia::task<void> logout(ruvia::db_executor db, std::string token) const;
    ruvia::task<SecurityInfo> security(ruvia::db_executor db, std::string userId) const;
    ruvia::task<std::optional<SecurityChange>> changeSecurity(ruvia::db_executor db, UserIdentity user, std::string action, std::string password, std::string code, std::string newPassword = {}, std::string telegramProof = {}) const;
    ruvia::task<std::optional<TelegramVerification>> requestTelegramVerification(ruvia::db_executor db, UserIdentity user, std::string generation) const;
    ruvia::task<std::optional<std::string>> decideTelegramVerification(ruvia::db_executor db, std::string generation, std::string id, std::int64_t telegramId, bool accept) const;
    ruvia::task<void> failTelegramVerification(ruvia::db_executor db, std::string id) const;
    ruvia::task<bool> verifyConnectionFactor(ruvia::db_transaction& tx, UserIdentity user, std::string code) const;
    ruvia::task<bool> reauthenticate(ruvia::db_executor db, UserIdentity user, std::string password, std::string code, std::string telegramProof = {}) const;
    ruvia::task<std::optional<std::vector<AccountInfo>>> accounts(ruvia::db_executor db, UserIdentity actor) const;
    ruvia::task<bool> createAccount(ruvia::db_executor db, UserIdentity actor, std::string username, std::string password, std::string role) const;
    ruvia::task<bool> updateAccount(ruvia::db_executor db, UserIdentity actor, std::string id, std::string role, bool disabled, std::string password) const;

  private:
    std::string key_;
};
} // namespace ultraterimal::server
