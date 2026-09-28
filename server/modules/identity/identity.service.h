#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace ultraterimal::server {
struct UserIdentity {
    std::string id;
    std::string username;
};
struct LoginSession {
    UserIdentity user;
    std::string token;
};
std::string randomToken();
std::string tokenHash(std::string_view token);
bool validToken(std::string_view token);
class IdentityService {
  public:
    explicit IdentityService(std::string database) : database_(std::move(database)) {}
    void bootstrap(std::string_view username, std::string_view password) const;
    std::optional<LoginSession> login(std::string_view username, std::string_view password) const;
    bool verifyPassword(std::string_view userId, std::string_view password) const;
    std::optional<UserIdentity> authenticate(std::string_view token) const;
    void logout(std::string_view token) const;

  private:
    std::string database_;
};
} // namespace ultraterimal::server
