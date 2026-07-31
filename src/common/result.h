#pragma once

#include "error.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace myrd {

template <class T>
class Result {
public:
    static Result<T> success(T value) {
        Result<T> r;
        r.value_ = std::move(value);
        return r;
    }

    static Result<T> failure(Error error) {
        Result<T> r;
        r.error_ = std::move(error);
        return r;
    }

    bool ok() const noexcept { return value_.has_value(); }
    bool has_error() const noexcept { return error_.has_value(); }

    T& value() { return value_.value(); }
    const T& value() const { return value_.value(); }

    T&& move_value() { return std::move(value_.value()); }

    Error& error() { return error_.value(); }
    const Error& error() const { return error_.value(); }

    explicit operator bool() const noexcept { return ok(); }

private:
    std::optional<T> value_;
    std::optional<Error> error_;

    Result() = default;
};

template <>
class Result<void> {
public:
    static Result<void> success() {
        Result<void> r;
        r.success_ = true;
        return r;
    }

    static Result<void> failure(Error error) {
        Result<void> r;
        r.error_ = std::move(error);
        return r;
    }

    bool ok() const noexcept { return success_; }
    bool has_error() const noexcept { return error_.has_value(); }

    Error& error() { return error_.value(); }
    const Error& error() const { return error_.value(); }

    explicit operator bool() const noexcept { return ok(); }

private:
    bool success_ = false;
    std::optional<Error> error_;

    Result() = default;
};

} // namespace myrd
