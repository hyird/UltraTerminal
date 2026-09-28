#pragma once
#include <utility>

#include <windows.h>

namespace ultraterimal::agent {
class UniqueHandle {
  public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

  private:
    HANDLE handle_ = nullptr;
};
} // namespace ultraterimal::agent
