#pragma once
#include <cstdint>
#include <db_executor.h>
#include <optional>
#include <string>
#include <task.h>
#include <utility>
#include <vector>

#include "modules/identity/identity.service.h"
#include "modules/sessions/session.types.h"
#include "telegram.types.h"

namespace ultraterimal::server {
class TelegramService {
  public:
    explicit TelegramService(std::string key = {}) : key_(std::move(key)) {}
    ruvia::task<TelegramState> state(ruvia::db_executor db, std::string userId) const;
    ruvia::task<std::optional<TelegramConfig>> configuration(ruvia::db_executor db) const;
    ruvia::task<bool> configure(ruvia::db_executor db, UserIdentity actor, TelegramConfig config) const;
    ruvia::task<bool> disable(ruvia::db_executor db, UserIdentity actor) const;
    ruvia::task<std::optional<std::string>> beginBinding(ruvia::db_executor db, UserIdentity user) const;
    ruvia::task<std::optional<std::string>> bind(ruvia::db_executor db, std::string generation, std::string token, std::int64_t telegramId) const;
    ruvia::task<void> unbind(ruvia::db_executor db, UserIdentity user) const;
    ruvia::task<std::optional<ApprovalDispatch>> requestApproval(ruvia::db_executor db, UserIdentity user, std::string deviceId, bool writeControl) const;
    ruvia::task<void> deliveryFailed(ruvia::db_executor db, std::string id) const;
    ruvia::task<std::optional<std::string>> decide(ruvia::db_executor db, std::string generation, std::string id, std::int64_t telegramId, bool accept) const;
    ruvia::task<std::vector<ApprovalInfo>> approvals(ruvia::db_executor db, UserIdentity user) const;
    ruvia::task<void> cancel(ruvia::db_executor db, UserIdentity user, std::string id) const;
    static ruvia::task<bool> consumeApproval(ruvia::db_transaction& tx, const SessionRequest& request, std::int64_t revision);

  private:
    std::string key_;
};
} // namespace ultraterimal::server
