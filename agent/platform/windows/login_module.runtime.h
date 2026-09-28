#pragma once

#include <atomic>

namespace ultraterimal::agent {
// COM objects retain the module until all their members have been destroyed.
class WindowsLoginModuleReference {
  public:
    WindowsLoginModuleReference() noexcept { ++objects_; }
    ~WindowsLoginModuleReference() { --objects_; }
    WindowsLoginModuleReference(const WindowsLoginModuleReference&) = delete;
    WindowsLoginModuleReference& operator=(const WindowsLoginModuleReference&) = delete;
    static bool unused() noexcept { return objects_ == 0 && locks_ == 0; }
    static bool lock(bool acquire) noexcept {
        if (acquire) {
            ++locks_;
            return true;
        }
        auto current = locks_.load();
        while (current != 0) {
            if (locks_.compare_exchange_weak(current, current - 1)) {
                return true;
            }
        }
        return false;
    }

  private:
    inline static std::atomic<unsigned long> objects_{0};
    inline static std::atomic<unsigned long> locks_{0};
};
} // namespace ultraterimal::agent
