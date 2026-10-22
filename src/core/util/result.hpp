#pragma once

// Result<T, E> — lightweight error propagation without exceptions crossing
// module boundaries (PLAN §6). Use `Result<T>` for values, `Result<void, E>`
// for outcomes. Callers must check `has_value()` before `value()`.

#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ldns {

struct Error {
    int code{};
    std::string message;

    Error() = default;
    Error(int c, std::string msg) : code(c), message(std::move(msg)) {}
};

namespace detail {
struct VoidTag {};
}  // namespace detail

template <typename T, typename E = Error>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T> does not support reference types");

public:
    using value_type = T;
    using error_type = E;

    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(E error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        if (!has_value()) throw std::logic_error("Result::value() on error: " + error().message);
        return std::get<0>(storage_);
    }
    const T& value() const& {
        if (!has_value()) throw std::logic_error("Result::value() on error: " + error().message);
        return std::get<0>(storage_);
    }
    T&& value() && {
        if (!has_value()) throw std::logic_error("Result::value() on error: " + error().message);
        return std::get<0>(std::move(storage_));
    }

    E& error() & {
        if (has_value()) throw std::logic_error("Result::error() on value");
        return std::get<1>(storage_);
    }
    const E& error() const& {
        if (has_value()) throw std::logic_error("Result::error() on value");
        return std::get<1>(storage_);
    }

    T value_or(T fallback) const& {
        return has_value() ? std::get<0>(storage_) : std::move(fallback);
    }

private:
    std::variant<T, E> storage_;
};

template <typename E>
class [[nodiscard]] Result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    Result() : storage_(std::in_place_index<0>, detail::VoidTag{}) {}
    Result(E error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const {
        if (!has_value()) throw std::logic_error("Result::value() on error: " + error().message);
    }

    E& error() & {
        if (has_value()) throw std::logic_error("Result::error() on value");
        return std::get<1>(storage_);
    }
    const E& error() const& {
        if (has_value()) throw std::logic_error("Result::error() on value");
        return std::get<1>(storage_);
    }

private:
    std::variant<detail::VoidTag, E> storage_;
};

}  // namespace ldns
