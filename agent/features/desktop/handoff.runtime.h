#pragma once

namespace ultraterimal::agent {
// A flapping console cannot keep an authorized connection in endless recovery.
// Successful frames and elapsed time do not replenish this connection's budget.
class DesktopHandoffBudget {
  public:
    bool admit() {
        if (attempts_ == 3) {
            return false;
        }
        ++attempts_;
        return true;
    }

  private:
    unsigned attempts_ = 0;
};
} // namespace ultraterimal::agent
