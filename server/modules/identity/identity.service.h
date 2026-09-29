#pragma once
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modules/database/database.config.h"
namespace ultraterimal::server {
struct UserIdentity {
    std::string id;
    std::string username;
    std::string role = "user";
    std::string loginHash;
};
enum class LoginError { kInvalidCredentials,
                        kFactorRequired };
struct LoginSession {
    UserIdentity user;
    std::string token;
};
std::string randomToken();
std::string tokenHash(std::string_view token);
bool validToken(std::string_view token);
struct AccountInfo {
    std::string id, username, role;
    bool disabled = false;
    bool totp = false;
};
struct SecurityInfo {
    bool totp = false;
    int recoveryCodes = 0;
};
struct SecurityChange {
    std::string secret;
    std::vector<std::string> recoveryCodes;
};
class IdentityService {
  public:
    explicit IdentityService(DatabaseConfig database, std::string key = {}) : database_(std::move(database)), key_(std::move(key)) {}
    void bootstrap(std::string_view username, std::string_view password) const;
    std::expected<LoginSession, LoginError> login(std::string_view username, std::string_view password, std::string_view code = {}) const;
    std::optional<UserIdentity> authenticate(std::string_view token) const;
    void logout(std::string_view token) const;
    SecurityInfo security(std::string_view userId) const;
    std::optional<SecurityChange> changeSecurity(const UserIdentity& user, std::string_view action, std::string_view password, std::string_view code, std::string_view newPassword = {}) const;
    bool reauthenticate(const UserIdentity& user, std::string_view password, std::string_view code) const;
    std::optional<std::vector<AccountInfo>> accounts(const UserIdentity& actor) const;
    bool createAccount(const UserIdentity& actor, std::string_view username, std::string_view password, std::string_view role) const;
    bool updateAccount(const UserIdentity& actor, std::string_view id, std::string_view role, bool disabled, std::string_view password) const;

  private:
    DatabaseConfig database_;
    std::string key_;
};
} // namespace ultraterimal::server
