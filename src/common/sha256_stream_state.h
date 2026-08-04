#pragma once

// Internal, platform-independent state machine for a streaming digest. The
// public SHA stream supplies BCrypt callbacks; tests use deterministic fakes
// without adding fault controls to the production object or ABI.

#include "result.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace myrd::detail {

inline constexpr std::uint32_t kSha256AlreadyFinalizedCode = 0x2201U;

static_assert(std::is_nothrow_move_constructible_v<Error>);
static_assert(std::is_nothrow_move_assignable_v<Error>);

class Sha256StreamState final {
public:
    void fail(Error error) {
        if (!terminal_error_.has_value() && !finalized_) {
            terminal_error_ = std::move(error);
        }
    }

    template <class UpdateFn>
    Result<void> update(
        std::span<const std::byte> data,
        std::size_t test_chunk_limit,
        UpdateFn&& update_chunk) {
        if (terminal_error_.has_value()) {
            return Result<void>::failure(*terminal_error_);
        }
        if (finalized_) {
            return Result<void>::failure(already_finalized_error());
        }

        const auto native_limit = static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)());
        auto chunk_limit = native_limit;
        if (test_chunk_limit != 0) {
            chunk_limit = (std::min)(chunk_limit, test_chunk_limit);
        }

        const auto* cursor = data.data();
        auto remaining = data.size();
        while (remaining != 0) {
            const auto chunk_size = (std::min)(remaining, chunk_limit);
            auto result = update_chunk(
                std::span<const std::byte>(cursor, chunk_size));
            ++chunk_calls_;
            if (!result.ok()) {
                terminal_error_.emplace(std::move(result.error()));
                return Result<void>::failure(*terminal_error_);
            }
            cursor += chunk_size;
            remaining -= chunk_size;
        }
        return Result<void>::success();
    }

    template <class FinishFn>
    Result<std::string> finalize(FinishFn&& finish) {
        if (terminal_error_.has_value()) {
            return Result<std::string>::failure(*terminal_error_);
        }
        if (finalized_) {
            return Result<std::string>::failure(already_finalized_error());
        }
        auto result = finish();
        if (!result.ok()) {
            terminal_error_.emplace(std::move(result.error()));
            return Result<std::string>::failure(*terminal_error_);
        }
        finalized_ = true;
        return result;
    }

    std::size_t chunk_calls() const noexcept { return chunk_calls_; }

private:
    static Error already_finalized_error() {
        return {ErrorDomain::internal, kSha256AlreadyFinalizedCode,
                L"Sha256Stream",
                L"stream already finalized"};
    }

    std::optional<Error> terminal_error_;
    bool finalized_{};
    std::size_t chunk_calls_{};
};

}  // namespace myrd::detail
