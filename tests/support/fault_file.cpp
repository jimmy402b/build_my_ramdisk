#include "fault_file.h"

#include "src/common/scope_exit.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace myrd::persistence::test {
namespace {

[[nodiscard]] std::size_t phase_index(const FaultFilePhase phase) noexcept {
    return static_cast<std::size_t>(phase);
}

[[nodiscard]] std::uint32_t default_error(
    const FaultFileAction action) noexcept {
    switch (action) {
    case FaultFileAction::DiskFull:
        return ERROR_DISK_FULL;
    case FaultFileAction::FlushError:
        return ERROR_IO_DEVICE;
    case FaultFileAction::CloseError:
        return ERROR_WRITE_FAULT;
    case FaultFileAction::TornWrite:
        return ERROR_WRITE_FAULT;
    case FaultFileAction::NativeError:
        return ERROR_GEN_FAILURE;
    case FaultFileAction::ShortRead:
    case FaultFileAction::ShortWrite:
    case FaultFileAction::ZeroProgress:
    case FaultFileAction::ReadbackCorruption:
        return ERROR_SUCCESS;
    }
    return ERROR_GEN_FAILURE;
}

[[nodiscard]] Error native_error(
    const std::uint32_t code,
    const wchar_t* operation) {
    return {
        ErrorDomain::win32,
        code,
        operation,
        L"injected test file failure",
        code,
    };
}

[[nodiscard]] Result<std::size_t> failed_transfer(
    const std::uint32_t code,
    const wchar_t* operation) {
    return Result<std::size_t>::failure(native_error(code, operation));
}

[[nodiscard]] bool checked_end(
    const std::uint64_t offset,
    const std::size_t length,
    std::uint64_t& end) noexcept {
    if (length > (std::numeric_limits<std::uint64_t>::max)() - offset) {
        return false;
    }
    end = offset + static_cast<std::uint64_t>(length);
    return true;
}

} // namespace

class FaultFileBackend final : public IFileIoBackend {
public:
    [[nodiscard]] static std::shared_ptr<FaultFileState> create_state(
        const std::uint64_t logical_length,
        std::vector<FaultFileRule> rules,
        FaultFileObserver observer) {
        return std::shared_ptr<FaultFileState>(
            new FaultFileState(
                logical_length,
                std::move(rules),
                std::move(observer)));
    }

    explicit FaultFileBackend(std::shared_ptr<FaultFileState> state)
        : state_(std::move(state)) {}

    Result<std::size_t> read_some(
        const OperationContext&,
        const std::uint64_t offset,
        const std::span<std::byte> destination) override {
        std::unique_lock lock(state_->mutex_);
        const auto call = begin_locked(FaultFilePhase::Read);
        record_request_locked(
            FaultFilePhase::Read, call, offset, destination.size());
        ScopeExit notify_observer([&] {
            lock.unlock();
            observe(FaultFilePhase::Read, call);
        });
        if (state_->closed_) {
            return failed_transfer(ERROR_INVALID_HANDLE, L"file_io.read");
        }

        std::uint64_t requested_end = 0;
        if (!checked_end(offset, destination.size(), requested_end)) {
            return failed_transfer(
                ERROR_ARITHMETIC_OVERFLOW, L"file_io.read");
        }

        auto* rule = matching_rule(
            FaultFilePhase::Read, call, offset, requested_end);
        if (rule != nullptr &&
            (rule->action == FaultFileAction::NativeError)) {
            const auto code = rule_error(*rule);
            consume(rule);
            return failed_transfer(code, L"file_io.read");
        }
        if (rule != nullptr &&
            rule->action == FaultFileAction::ZeroProgress) {
            consume(rule);
            return Result<std::size_t>::success(0);
        }

        const auto available = offset < state_->logical_length_
            ? state_->logical_length_ - offset
            : 0;
        auto transfer = static_cast<std::size_t>((std::min)(
            static_cast<std::uint64_t>(destination.size()), available));
        if (rule != nullptr &&
            rule->action == FaultFileAction::ShortRead) {
            transfer = bytes_before_rule(*rule, offset, transfer);
        }

        std::memset(destination.data(), 0, transfer);
        copy_allocated_pages(offset, destination.first(transfer));

        bool corruption_applied = false;
        if (rule != nullptr &&
            rule->action == FaultFileAction::ReadbackCorruption &&
            rule->byte_offset.has_value() &&
            *rule->byte_offset >= offset &&
            *rule->byte_offset < offset + transfer) {
            const auto index = static_cast<std::size_t>(
                *rule->byte_offset - offset);
            destination[index] ^= std::byte{0x01};
            corruption_applied = true;
        }
        if (rule != nullptr &&
            (rule->action != FaultFileAction::ReadbackCorruption ||
             corruption_applied)) {
            consume(rule);
        }
        return Result<std::size_t>::success(transfer);
    }

    Result<std::size_t> write_some(
        const OperationContext&,
        const std::uint64_t offset,
        const std::span<const std::byte> source) override {
        std::unique_lock lock(state_->mutex_);
        const auto call = begin_locked(FaultFilePhase::Write);
        ScopeExit notify_observer([&] {
            lock.unlock();
            observe(FaultFilePhase::Write, call);
        });
        if (state_->closed_) {
            return failed_transfer(ERROR_INVALID_HANDLE, L"file_io.write");
        }

        std::uint64_t requested_end = 0;
        if (!checked_end(offset, source.size(), requested_end)) {
            return failed_transfer(
                ERROR_ARITHMETIC_OVERFLOW, L"file_io.write");
        }

        auto* rule = matching_rule(
            FaultFilePhase::Write, call, offset, requested_end);
        if (rule != nullptr &&
            (rule->action == FaultFileAction::NativeError ||
             (rule->action == FaultFileAction::DiskFull &&
              !rule->byte_offset.has_value()))) {
            const auto code = rule_error(*rule);
            consume(rule);
            return failed_transfer(code, L"file_io.write");
        }
        if (rule != nullptr &&
            rule->action == FaultFileAction::ZeroProgress) {
            consume(rule);
            return Result<std::size_t>::success(0);
        }

        auto transfer = source.size();
        if (rule != nullptr &&
            (rule->action == FaultFileAction::ShortWrite ||
             rule->action == FaultFileAction::DiskFull ||
             rule->action == FaultFileAction::TornWrite)) {
            transfer = bytes_before_rule(*rule, offset, source.size());
        }

        auto write_result = write_prefix(offset, source.first(transfer));
        if (!write_result.ok()) {
            return write_result;
        }
        if (rule != nullptr &&
            (rule->action == FaultFileAction::DiskFull ||
             rule->action == FaultFileAction::TornWrite)) {
            const auto code = rule_error(*rule);
            consume(rule);
            return failed_transfer(code, L"file_io.write");
        }
        if (rule != nullptr) {
            consume(rule);
        }
        return Result<std::size_t>::success(transfer);
    }

    Result<void> flush(const OperationContext&) override {
        return simple_phase(
            FaultFilePhase::Flush,
            FaultFileAction::FlushError,
            L"file_io.flush");
    }

    Result<std::uint64_t> length(const OperationContext&) const override {
        std::unique_lock lock(state_->mutex_);
        const auto call = begin_locked(FaultFilePhase::Length);
        ScopeExit notify_observer([&] {
            lock.unlock();
            observe(FaultFilePhase::Length, call);
        });
        if (state_->closed_) {
            return Result<std::uint64_t>::failure(
                native_error(ERROR_INVALID_HANDLE, L"file_io.length"));
        }
        auto* rule = matching_rule(
            FaultFilePhase::Length, call, 0, 0);
        if (rule != nullptr && rule->action == FaultFileAction::NativeError) {
            const auto code = rule_error(*rule);
            consume(rule);
            return Result<std::uint64_t>::failure(
                native_error(code, L"file_io.length"));
        }
        return Result<std::uint64_t>::success(state_->logical_length_);
    }

    Result<void> truncate(
        const OperationContext&,
        const std::uint64_t new_length) override {
        std::unique_lock lock(state_->mutex_);
        const auto call = begin_locked(FaultFilePhase::Truncate);
        ScopeExit notify_observer([&] {
            lock.unlock();
            observe(FaultFilePhase::Truncate, call);
        });
        if (state_->closed_) {
            return Result<void>::failure(
                native_error(ERROR_INVALID_HANDLE, L"file_io.truncate"));
        }
        auto* rule = matching_rule(
            FaultFilePhase::Truncate, call, new_length, new_length);
        if (rule != nullptr && rule->action == FaultFileAction::NativeError) {
            const auto code = rule_error(*rule);
            consume(rule);
            return Result<void>::failure(
                native_error(code, L"file_io.truncate"));
        }

        if (new_length < state_->logical_length_) {
            const auto first_removed =
                (new_length + FaultFileState::kPageBytes - 1) /
                FaultFileState::kPageBytes;
            state_->pages_.erase(
                state_->pages_.lower_bound(first_removed),
                state_->pages_.end());

            const auto tail = new_length % FaultFileState::kPageBytes;
            if (tail != 0) {
                const auto page_index =
                    new_length / FaultFileState::kPageBytes;
                const auto found = state_->pages_.find(page_index);
                if (found != state_->pages_.end()) {
                    std::fill(
                        found->second.begin() +
                            static_cast<std::ptrdiff_t>(tail),
                        found->second.end(),
                        std::byte{0});
                }
            }
        }
        state_->logical_length_ = new_length;
        return Result<void>::success();
    }

    Result<void> close() override {
        std::unique_lock lock(state_->mutex_);
        const auto call = begin_locked(FaultFilePhase::Close);
        ScopeExit notify_observer([&] {
            lock.unlock();
            observe(FaultFilePhase::Close, call);
        });
        if (state_->closed_) {
            return Result<void>::success();
        }
        auto* rule = matching_rule(FaultFilePhase::Close, call, 0, 0);
        if (rule != nullptr && rule->action == FaultFileAction::CloseError) {
            const auto code = rule_error(*rule);
            consume(rule);
            return Result<void>::failure(
                native_error(code, L"file_io.close"));
        }
        state_->closed_ = true;
        return Result<void>::success();
    }

private:
    std::shared_ptr<FaultFileState> state_;

    // The call number is linearized by state_->mutex_: no rule can observe a
    // duplicate occurrence and getters see the increment atomically with the
    // operation's state transition.
    [[nodiscard]] std::uint64_t begin_locked(
        const FaultFilePhase phase) const {
        return ++state_->calls_[phase_index(phase)];
    }

    void record_request_locked(
        const FaultFilePhase phase,
        const std::uint64_t call,
        const std::uint64_t offset,
        const std::size_t length) const noexcept {
        if (state_->request_count_ >= state_->requests_.size()) {
            state_->request_trace_overflowed_ = true;
            return;
        }
        state_->requests_[state_->request_count_++] = {
            phase, call, offset, static_cast<std::uint64_t>(length)};
    }

    void observe(
        const FaultFilePhase phase,
        const std::uint64_t call) const {
        if (state_->observer_) {
            state_->observer_(phase, call);
        }
    }

    [[nodiscard]] FaultFileRule* matching_rule(
        const FaultFilePhase phase,
        const std::uint64_t call,
        const std::uint64_t begin_offset,
        const std::uint64_t end_offset) const {
        for (std::size_t i = 0; i < state_->rules_.size(); ++i) {
            if (state_->consumed_rules_[i]) {
                continue;
            }
            auto& rule = state_->rules_[i];
            if (rule.phase != phase) {
                continue;
            }
            if (rule.byte_offset.has_value()) {
                // A byte-addressed rule becomes armed on its requested call
                // and remains pending until a later half-open request range
                // actually contains that byte. Merely touching the right
                // endpoint must neither match nor consume the rule.
                if (rule.occurrence > call ||
                    *rule.byte_offset < begin_offset ||
                    *rule.byte_offset >= end_offset) {
                    continue;
                }
            } else if (rule.occurrence != call) {
                continue;
            }
            return &rule;
        }
        return nullptr;
    }

    void consume(const FaultFileRule* rule) const noexcept {
        const auto index = static_cast<std::size_t>(
            rule - state_->rules_.data());
        state_->consumed_rules_[index] = true;
    }

    [[nodiscard]] static std::uint32_t rule_error(
        const FaultFileRule& rule) noexcept {
        return rule.native_code != ERROR_SUCCESS
            ? rule.native_code
            : default_error(rule.action);
    }

    [[nodiscard]] static std::size_t bytes_before_rule(
        const FaultFileRule& rule,
        const std::uint64_t offset,
        const std::size_t maximum) noexcept {
        if (!rule.byte_offset.has_value() || *rule.byte_offset <= offset) {
            return 0;
        }
        const auto distance = *rule.byte_offset - offset;
        return static_cast<std::size_t>((std::min)(
            distance, static_cast<std::uint64_t>(maximum)));
    }

    [[nodiscard]] Result<std::size_t> write_prefix(
        const std::uint64_t offset,
        const std::span<const std::byte> source) {
        if (source.empty()) {
            return Result<std::size_t>::success(0);
        }
        try {
            std::uint64_t position = offset;
            std::size_t copied = 0;
            while (copied < source.size()) {
                const auto page_index =
                    position / FaultFileState::kPageBytes;
                const auto page_offset = static_cast<std::size_t>(
                    position % FaultFileState::kPageBytes);
                const auto amount = (std::min)(
                    source.size() - copied,
                    static_cast<std::size_t>(
                        FaultFileState::kPageBytes - page_offset));
                auto& page = state_->pages_[page_index];
                std::copy_n(
                    source.data() + copied,
                    amount,
                    page.data() + page_offset);
                copied += amount;
                position += amount;
            }
            state_->logical_length_ = (std::max)(
                state_->logical_length_, position);
            return Result<std::size_t>::success(source.size());
        } catch (const std::bad_alloc&) {
            return failed_transfer(
                ERROR_NOT_ENOUGH_MEMORY, L"file_io.write");
        }
    }

    void copy_allocated_pages(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const {
        if (destination.empty()) {
            return;
        }
        const auto end = offset + destination.size();
        const auto first_page = offset / FaultFileState::kPageBytes;
        const auto last_page =
            (end - 1) / FaultFileState::kPageBytes;
        auto found = state_->pages_.lower_bound(first_page);
        while (found != state_->pages_.end() && found->first <= last_page) {
            const auto page_begin =
                found->first * FaultFileState::kPageBytes;
            const auto copy_begin = (std::max)(offset, page_begin);
            const auto copy_end = (std::min)(
                end, page_begin + FaultFileState::kPageBytes);
            const auto destination_offset = static_cast<std::size_t>(
                copy_begin - offset);
            const auto source_offset = static_cast<std::size_t>(
                copy_begin - page_begin);
            const auto amount = static_cast<std::size_t>(
                copy_end - copy_begin);
            std::copy_n(
                found->second.data() + source_offset,
                amount,
                destination.data() + destination_offset);
            ++found;
        }
    }

    Result<void> simple_phase(
        const FaultFilePhase phase,
        const FaultFileAction expected_action,
        const wchar_t* operation) {
        std::unique_lock lock(state_->mutex_);
        const auto call = begin_locked(phase);
        ScopeExit notify_observer([&] {
            lock.unlock();
            observe(phase, call);
        });
        if (state_->closed_) {
            return Result<void>::failure(
                native_error(ERROR_INVALID_HANDLE, operation));
        }
        auto* rule = matching_rule(phase, call, 0, 0);
        if (rule != nullptr &&
            (rule->action == expected_action ||
             rule->action == FaultFileAction::NativeError)) {
            const auto code = rule_error(*rule);
            consume(rule);
            return Result<void>::failure(native_error(code, operation));
        }
        return Result<void>::success();
    }
};

FaultFileState::FaultFileState(
    const std::uint64_t logical_length,
    std::vector<FaultFileRule> rules,
    FaultFileObserver observer)
    : logical_length_(logical_length),
      rules_(std::move(rules)),
      consumed_rules_(rules_.size(), false),
      observer_(std::move(observer)) {}

std::uint64_t FaultFileState::logical_length() const {
    std::lock_guard lock(mutex_);
    return logical_length_;
}

std::uint64_t FaultFileState::physical_bytes() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::uint64_t>(pages_.size()) * kPageBytes;
}

std::size_t FaultFileState::allocated_pages() const {
    std::lock_guard lock(mutex_);
    return pages_.size();
}

std::uint64_t FaultFileState::calls(
    const FaultFilePhase phase) const {
    std::lock_guard lock(mutex_);
    return calls_[phase_index(phase)];
}

bool FaultFileState::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

void FaultFileState::force_length_for_test(
    const std::uint64_t logical_length) {
    std::lock_guard lock(mutex_);
    logical_length_ = logical_length;
}

void FaultFileState::clear_requests_for_test() {
    std::lock_guard lock(mutex_);
    request_count_ = 0;
    request_trace_overflowed_ = false;
}

std::vector<FaultFileRequest> FaultFileState::requests() const {
    std::lock_guard lock(mutex_);
    return {requests_.begin(), requests_.begin() +
        static_cast<std::ptrdiff_t>(request_count_)};
}

bool FaultFileState::request_trace_overflowed() const {
    std::lock_guard lock(mutex_);
    return request_trace_overflowed_;
}

FaultFile make_fault_file(
    const std::uint64_t logical_length,
    std::vector<FaultFileRule> rules,
    FaultFileObserver observer) {
    auto state = FaultFileBackend::create_state(
        logical_length,
        std::move(rules),
        std::move(observer));
    auto file = make_exact_persistence_file(
        std::make_unique<FaultFileBackend>(state));
    return {std::move(file), std::move(state)};
}

} // namespace myrd::persistence::test
