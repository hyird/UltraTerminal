#pragma once
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ultraterimal::server {
class DatabaseError : public std::runtime_error {
  public:
    explicit DatabaseError(unsigned code) : std::runtime_error("Database operation failed (" + std::to_string(code) + ")"), code_(code) {}
    unsigned code() const noexcept { return code_; }

  private:
    unsigned code_;
};
struct DatabaseParameter {
    std::optional<std::string> value;
    bool binary = false;
    DatabaseParameter(std::nullptr_t) {}
    DatabaseParameter(std::nullopt_t) {}
    DatabaseParameter(std::string_view text) : value(text) {}
    DatabaseParameter(const std::string& text) : value(text) {}
    DatabaseParameter(const char* text) : value(text) {}
    DatabaseParameter(bool number) : value(number ? "1" : "0") {}
    template <std::integral T>
    DatabaseParameter(T number) : value(std::to_string(number)) {}
    template <typename T>
    DatabaseParameter(const std::optional<T>& item) : DatabaseParameter(nullptr) {
        if (item) {
            *this = DatabaseParameter(*item);
        }
    }
    DatabaseParameter(std::span<const std::byte> bytes) : value(std::in_place, reinterpret_cast<const char*>(bytes.data()), bytes.size()), binary(true) {}
};
template <typename T>
struct IsDatabaseOptional : std::false_type {};
template <typename T>
struct IsDatabaseOptional<std::optional<T>> : std::true_type {};
struct DatabaseField {
    std::optional<std::string> value;
    template <typename T>
    T as() const {
        if constexpr (IsDatabaseOptional<T>::value) {
            return value ? T{as<typename T::value_type>()} : std::nullopt;
        } else {
            if (!value) {
                throw std::runtime_error("Unexpected database NULL");
            }
            if constexpr (std::is_same_v<T, std::string>) {
                return *value;
            } else if constexpr (std::is_same_v<T, bool>) {
                if (*value == "1") {
                    return true;
                }
                if (*value == "0") {
                    return false;
                }
                throw std::runtime_error("Invalid database boolean");
            } else {
                T result{};
                const auto parsed = std::from_chars(value->data(), value->data() + value->size(), result);
                if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size()) {
                    throw std::runtime_error("Invalid database number");
                }
                return result;
            }
        }
    }
    template <typename T>
    T as(T fallback) const { return value ? as<T>() : std::move(fallback); }
};
struct DatabaseRow {
    std::vector<std::pair<std::string, DatabaseField>> fields;
    const DatabaseField& operator[](std::size_t index) const { return fields.at(index).second; }
    const DatabaseField& operator[](std::string_view name) const {
        for (const auto& [key, field] : fields) {
            if (key == name) {
                return field;
            }
        }
        throw std::runtime_error("Unknown database column");
    }
};
struct DatabaseResult {
    std::vector<DatabaseRow> rows;
    std::uint64_t affectedRows = 0;
    bool empty() const { return rows.empty(); }
    std::size_t size() const { return rows.size(); }
    const DatabaseRow& operator[](std::size_t index) const { return rows.at(index); }
    auto begin() const { return rows.begin(); }
    auto end() const { return rows.end(); }
};
} // namespace ultraterimal::server
