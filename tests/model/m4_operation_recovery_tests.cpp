#include "tests/support/test_framework.h"

#include "src/management/operation_store.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

std::wstring operation_leaf(const myrd::product::Uuid& value) {
    const auto text = value.to_string();
    return std::wstring(text.begin(), text.end()) + L".myop";
}

std::wstring widen(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

std::array<std::byte, 32> digest() {
    std::array<std::byte, 32> value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::byte>(index + 1U);
    }
    return value;
}

myrd::management::OperationRecord queued_record() {
    return myrd::management::OperationRecord{
        1,
        uuid("10000000-0000-4000-8000-000000000001"),
        uuid("20000000-0000-4000-8000-000000000002"),
        uuid("30000000-0000-4000-8000-000000000003"),
        L"S-1-5-21-100-200-300-1001",
        "disk.checkpoint",
        digest(),
        myrd::management::OperationState::queued,
        "queued",
        1700000000000ULL,
        1700000000000ULL,
        std::nullopt,
        true,
        false,
        {}};
}

myrd::management::OperationRecord running_record() {
    auto value = queued_record();
    value.revision = 2;
    value.state = myrd::management::OperationState::running;
    value.phase = "dispatch";
    value.updated_unix_ms += 1;
    return value;
}

myrd::management::OperationRecord terminal_record() {
    auto value = running_record();
    value.revision = 3;
    value.state = myrd::management::OperationState::succeeded;
    value.phase = "terminal";
    value.updated_unix_ms += 1;
    value.terminal_unix_ms = value.updated_unix_ms;
    value.cancellable = false;
    value.terminal_result = {
        std::byte{'p'}, std::byte{'r'}, std::byte{'o'}, std::byte{'v'},
        std::byte{'e'}, std::byte{'n'}};
    return value;
}

std::vector<std::byte> encode(
    const myrd::management::OperationRecord& value) {
    auto encoded = myrd::management::encode_operation_record(value);
    REQUIRE(encoded.ok());
    return encoded.move_value();
}

myrd::Error failure(std::wstring operation) {
    return myrd::Error{
        myrd::ErrorDomain::win32,
        static_cast<std::uint32_t>(
            myrd::profile::ProfileStoreError::io_failure),
        std::move(operation), L"Injected crash", ERROR_WRITE_FAULT};
}

struct FaultState final {
    std::map<std::wstring, std::vector<std::byte>, std::less<>> files;
    std::map<std::uint64_t, std::vector<std::byte>> temps;
    std::uint64_t next_token{1};
    std::optional<std::size_t> fail_boundary;
    std::size_t boundary{};
    bool fail_writes_after_first_current_publish{};
    std::size_t current_publishes{};
    bool removal_attempted{};
    bool throw_list_after_current_publish{};
    bool throw_after_remove{};

    void reset() {
        fail_boundary.reset();
        boundary = 0;
        removal_attempted = false;
    }
    void fail_at(std::size_t value) {
        fail_boundary = value;
        boundary = 0;
    }
    bool step() {
        const auto current = boundary++;
        return fail_boundary.has_value() && current == fail_boundary.value();
    }
};

class FaultDirectory final : public myrd::profile::IAtomicFileDirectory {
public:
    explicit FaultDirectory(std::shared_ptr<FaultState> state)
        : state_(std::move(state)) {}

    myrd::Result<std::vector<std::byte>> read_file(
        std::wstring_view name) override {
        if (state_->step()) return failed_value<std::vector<std::byte>>(L"read.before");
        const auto found = state_->files.find(std::wstring(name));
        if (found == state_->files.end()) {
            return myrd::Result<std::vector<std::byte>>::failure(myrd::Error{
                myrd::ErrorDomain::protocol,
                static_cast<std::uint32_t>(
                    myrd::profile::ProfileStoreError::path_not_found),
                L"fault.read", L"Not found"});
        }
        auto value = found->second;
        if (state_->step()) return failed_value<std::vector<std::byte>>(L"read.after");
        return myrd::Result<std::vector<std::byte>>::success(std::move(value));
    }

    myrd::Result<myrd::profile::AtomicTempFile> create_unique_temp(
        std::wstring_view) override {
        if (state_->step()) {
            return failed_value<myrd::profile::AtomicTempFile>(L"create.before");
        }
        const myrd::profile::AtomicTempFile token{state_->next_token++};
        state_->temps[token.value] = {};
        if (state_->step()) {
            state_->temps.erase(token.value);
            return failed_value<myrd::profile::AtomicTempFile>(L"create.after");
        }
        return myrd::Result<myrd::profile::AtomicTempFile>::success(token);
    }

    myrd::Result<void> write_temp(
        myrd::profile::AtomicTempFile token,
        std::span<const std::byte> bytes) override {
        if (state_->fail_writes_after_first_current_publish
            && state_->current_publishes >= 1) {
            return failed(L"write.after-first-operation");
        }
        if (state_->step()) return failed(L"write.before");
        state_->temps[token.value] = {bytes.begin(), bytes.end()};
        if (state_->step()) return failed(L"write.after");
        return myrd::Result<void>::success();
    }

    myrd::Result<void> flush_temp(
        myrd::profile::AtomicTempFile) override {
        if (state_->step()) return failed(L"flush.before");
        if (state_->step()) return failed(L"flush.after");
        return myrd::Result<void>::success();
    }

    myrd::Result<std::vector<std::byte>> read_temp(
        myrd::profile::AtomicTempFile token) override {
        if (state_->step()) return failed_value<std::vector<std::byte>>(L"verify.before");
        auto value = state_->temps[token.value];
        if (state_->step()) return failed_value<std::vector<std::byte>>(L"verify.after");
        return myrd::Result<std::vector<std::byte>>::success(std::move(value));
    }

    myrd::Result<void> publish_temp(
        myrd::profile::AtomicTempFile token,
        std::wstring_view destination,
        bool) override {
        if (state_->step()) return failed(L"publish.before");
        state_->files[std::wstring(destination)] = state_->temps[token.value];
        state_->temps.erase(token.value);
        if (destination.ends_with(L".myop")) {
            ++state_->current_publishes;
        }
        if (state_->step()) return failed(L"publish.after");
        return myrd::Result<void>::success();
    }

    myrd::Result<void> remove_file(std::wstring_view name) override {
        state_->removal_attempted = true;
        if (state_->step()) return failed(L"remove.before");
        state_->files.erase(std::wstring(name));
        if (state_->throw_after_remove) {
            throw std::runtime_error("remove.after");
        }
        if (state_->step()) return failed(L"remove.after");
        return myrd::Result<void>::success();
    }
    myrd::Result<std::vector<std::wstring>> list_files() override {
        if (state_->throw_list_after_current_publish
            && state_->current_publishes != 0) {
            throw std::runtime_error("list.after-publish");
        }
        std::vector<std::wstring> names;
        for (const auto& [name, unused] : state_->files) {
            static_cast<void>(unused);
            names.push_back(name);
        }
        return myrd::Result<std::vector<std::wstring>>::success(
            std::move(names));
    }
    myrd::Result<void> flush_directory() override {
        if (state_->step()) return failed(L"directory.before");
        if (state_->step()) return failed(L"directory.after");
        return myrd::Result<void>::success();
    }
    void discard_temp(myrd::profile::AtomicTempFile token) noexcept override {
        state_->temps.erase(token.value);
    }

private:
    myrd::Result<void> failed(std::wstring operation) {
        return myrd::Result<void>::failure(failure(std::move(operation)));
    }
    template <class T>
    myrd::Result<T> failed_value(std::wstring operation) {
        return myrd::Result<T>::failure(failure(std::move(operation)));
    }

    std::shared_ptr<FaultState> state_;
};

myrd::Result<myrd::management::OperationStore> open_store(
    const std::shared_ptr<FaultState>& state, std::wstring gate_key) {
    return myrd::management::OperationStore::from_directory_capability(
        std::make_unique<FaultDirectory>(state), {}, std::move(gate_key));
}

std::shared_ptr<FaultState> seeded(
    const myrd::management::OperationRecord& record) {
    auto state = std::make_shared<FaultState>();
    state->files[operation_leaf(record.operation_id)] = encode(record);
    return state;
}

} // namespace

TEST_CASE(operation_crash_model_queued_is_absent_or_complete) {
    auto baseline_state = std::make_shared<FaultState>();
    auto baseline = open_store(baseline_state, L"queued-baseline");
    REQUIRE(baseline.ok());
    baseline_state->reset();
    const myrd::management::OperationRequest request{
        queued_record().request_id,
        queued_record().profile_id,
        queued_record().caller_sid,
        queued_record().command_scope,
        digest(),
        1700000000000ULL};
    REQUIRE(baseline.value().admit(request).ok());
    const auto boundaries = baseline_state->boundary;
    REQUIRE(boundaries >= 10);

    for (std::size_t boundary = 0; boundary < boundaries; ++boundary) {
        auto state = std::make_shared<FaultState>();
        {
            auto store = open_store(state, L"queued-" + std::to_wstring(boundary));
            REQUIRE(store.ok());
            state->fail_at(boundary);
            REQUIRE(!store.value().admit(request).ok());
        }
        REQUIRE(state->temps.empty());
        state->reset();
        auto reopened = open_store(
            state, L"queued-reopen-" + std::to_wstring(boundary));
        REQUIRE(reopened.ok());
        auto records = reopened.value().list_records();
        REQUIRE(records.ok());
        REQUIRE(records.value().size() <= 1);
        if (!records.value().empty()) {
            REQUIRE_EQ(records.value()[0].state,
                       myrd::management::OperationState::queued);
        }
    }
}

TEST_CASE(operation_crash_model_running_and_terminal_are_never_mixed) {
    const auto queued = queued_record();
    const auto running = running_record();
    const auto terminal = terminal_record();
    for (const auto* proposed : {&running, &terminal}) {
        const auto& initial = proposed->state == myrd::management::OperationState::running
            ? queued : running;
        auto baseline_state = seeded(initial);
        auto baseline = open_store(
            baseline_state, L"transition-baseline-" + widen(proposed->phase));
        REQUIRE(baseline.ok());
        baseline_state->reset();
        REQUIRE(baseline.value().write_record(initial.revision, *proposed).ok());
        const auto boundaries = baseline_state->boundary;
        for (std::size_t boundary = 0; boundary < boundaries; ++boundary) {
            auto state = seeded(initial);
            {
                auto store = open_store(
                    state, L"transition-" + std::to_wstring(boundary)
                        + widen(proposed->phase));
                REQUIRE(store.ok());
                state->fail_at(boundary);
                REQUIRE(!store.value().write_record(
                    initial.revision, *proposed).ok());
            }
            REQUIRE(state->temps.empty());
            state->reset();
            auto reopened = open_store(
                state, L"transition-reopen-" + std::to_wstring(boundary)
                    + widen(proposed->phase));
            REQUIRE(reopened.ok());
            auto loaded = reopened.value().load_record(initial.operation_id);
            REQUIRE(loaded.ok());
            REQUIRE(loaded.value() == initial || loaded.value() == *proposed);
            if (loaded.value().state
                == myrd::management::OperationState::succeeded) {
                REQUIRE_EQ(loaded.value().terminal_result,
                           terminal.terminal_result);
            }
        }
    }
}

TEST_CASE(operation_restart_reconciliation_never_invents_success) {
    const auto running = running_record();
    auto state = seeded(running);
    auto store = open_store(state, L"reconcile");
    REQUIRE(store.ok());
    auto recovered = store.value().reconcile_after_restart(1700000000100ULL);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.value().size(), 1U);
    REQUIRE_EQ(recovered.value()[0].state,
               myrd::management::OperationState::reconciling);
    REQUIRE_EQ(recovered.value()[0].phase, running.phase);
    REQUIRE(!recovered.value()[0].terminal_unix_ms.has_value());
    REQUIRE(recovered.value()[0].terminal_result.empty());

    auto terminal_state = seeded(terminal_record());
    auto terminal_store = open_store(terminal_state, L"reconcile-terminal");
    REQUIRE(terminal_store.ok());
    auto terminal_recovery = terminal_store.value().reconcile_after_restart(
        1700000000100ULL);
    REQUIRE(terminal_recovery.ok());
    REQUIRE_EQ(terminal_recovery.value()[0], terminal_record());
}

TEST_CASE(operation_restart_reconciliation_is_crash_idempotent) {
    auto baseline_state = seeded(running_record());
    auto baseline = open_store(baseline_state, L"reconcile-crash-baseline");
    REQUIRE(baseline.ok());
    baseline_state->reset();
    REQUIRE(baseline.value().reconcile_after_restart(
        1700000000100ULL).ok());
    const auto boundaries = baseline_state->boundary;
    REQUIRE(boundaries >= 10);

    for (std::size_t boundary = 0; boundary < boundaries; ++boundary) {
        auto state = seeded(running_record());
        {
            auto store = open_store(
                state, L"reconcile-crash-" + std::to_wstring(boundary));
            REQUIRE(store.ok());
            state->fail_at(boundary);
            REQUIRE(!store.value().reconcile_after_restart(
                1700000000100ULL).ok());
        }
        REQUIRE(state->temps.empty());
        state->reset();
        auto reopened = open_store(
            state, L"reconcile-crash-reopen-" + std::to_wstring(boundary));
        REQUIRE(reopened.ok());
        auto records = reopened.value().list_records();
        REQUIRE(records.ok());
        REQUIRE_EQ(records.value().size(), 1U);
        REQUIRE(records.value()[0].state
                    == myrd::management::OperationState::running
                || records.value()[0].state
                    == myrd::management::OperationState::reconciling);
        REQUIRE(!records.value()[0].terminal_unix_ms.has_value());
        REQUIRE(records.value()[0].terminal_result.empty());
    }
}

TEST_CASE(operation_restart_reconciliation_reports_partial_commit) {
    auto first = running_record();
    auto second = running_record();
    second.operation_id = uuid("20000000-0000-4000-8000-000000000004");
    second.request_id = uuid("10000000-0000-4000-8000-000000000004");
    second.created_unix_ms += 10;
    second.updated_unix_ms += 10;
    auto state = seeded(first);
    state->files[operation_leaf(second.operation_id)] = encode(second);
    auto store = open_store(state, L"reconcile-partial");
    REQUIRE(store.ok());
    state->reset();
    state->fail_writes_after_first_current_publish = true;
    auto result = store.value().reconcile_after_restart(1700000000100ULL);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   myrd::management::OperationStoreError::commit_indeterminate));
    REQUIRE(state->temps.empty());
    state->fail_writes_after_first_current_publish = false;
    state->reset();
    auto reopened = open_store(state, L"reconcile-partial-reopen");
    REQUIRE(reopened.ok());
    auto records = reopened.value().list_records();
    REQUIRE(records.ok());
    REQUIRE_EQ(records.value().size(), 2U);
    const auto reconciling = std::count_if(
        records.value().begin(), records.value().end(), [](const auto& record) {
            return record.state
                == myrd::management::OperationState::reconciling;
        });
    const auto running = std::count_if(
        records.value().begin(), records.value().end(), [](const auto& record) {
            return record.state == myrd::management::OperationState::running;
        });
    REQUIRE_EQ(reconciling, 1);
    REQUIRE_EQ(running, 1);
}

TEST_CASE(operation_restart_reconciliation_reports_post_commit_exception) {
    auto state = seeded(running_record());
    auto store = open_store(state, L"reconcile-post-commit-exception");
    REQUIRE(store.ok());
    state->reset();
    state->throw_list_after_current_publish = true;
    auto result = store.value().reconcile_after_restart(1700000000100ULL);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
        myrd::management::OperationStoreError::commit_indeterminate));
}

TEST_CASE(operation_cancellation_request_is_crash_durable) {
    auto baseline_state = seeded(queued_record());
    auto baseline = open_store(baseline_state, L"cancel-baseline");
    REQUIRE(baseline.ok());
    baseline_state->reset();
    REQUIRE(baseline.value().request_cancel(
        queued_record().operation_id, 1700000000100ULL).ok());
    const auto boundaries = baseline_state->boundary;
    REQUIRE(boundaries >= 10);

    for (std::size_t boundary = 0; boundary < boundaries; ++boundary) {
        auto state = seeded(queued_record());
        {
            auto store = open_store(
                state, L"cancel-" + std::to_wstring(boundary));
            REQUIRE(store.ok());
            state->fail_at(boundary);
            REQUIRE(!store.value().request_cancel(
                queued_record().operation_id, 1700000000100ULL).ok());
        }
        REQUIRE(state->temps.empty());
        state->reset();
        auto reopened = open_store(
            state, L"cancel-reopen-" + std::to_wstring(boundary));
        REQUIRE(reopened.ok());
        auto loaded = reopened.value().load_record(
            queued_record().operation_id);
        REQUIRE(loaded.ok());
        REQUIRE_EQ(loaded.value().state,
                   myrd::management::OperationState::queued);
        REQUIRE(loaded.value().revision == 1
                || loaded.value().revision == 2);
        REQUIRE_EQ(loaded.value().cancel_requested,
                   loaded.value().revision == 2);
        REQUIRE(!loaded.value().terminal_unix_ms.has_value());
    }
}

TEST_CASE(operation_cleanup_crash_leaves_only_operation_metadata) {
    const auto terminal = terminal_record();
    auto baseline_state = seeded(terminal);
    baseline_state->files[
        std::wstring(operation_leaf(terminal.operation_id)).replace(36, 5, L".prev")]
        = encode(running_record());
    const auto now = terminal.terminal_unix_ms.value()
        + myrd::management::kOperationRetentionMilliseconds;
    auto baseline = open_store(baseline_state, L"cleanup-baseline");
    REQUIRE(baseline.ok());
    baseline_state->reset();
    REQUIRE(baseline.value().cleanup_terminal_records(now).ok());
    const auto boundaries = baseline_state->boundary;
    REQUIRE(boundaries >= 10);

    for (std::size_t boundary = 0; boundary < boundaries; ++boundary) {
        auto state = seeded(terminal);
        auto previous_name = operation_leaf(terminal.operation_id);
        previous_name.replace(36, 5, L".prev");
        state->files[previous_name] = encode(running_record());
        {
            auto store = open_store(
                state, L"cleanup-" + std::to_wstring(boundary));
            REQUIRE(store.ok());
            state->fail_at(boundary);
            auto cleaned = store.value().cleanup_terminal_records(now);
            REQUIRE(!cleaned.ok());
            if (state->removal_attempted) {
                REQUIRE_EQ(cleaned.error().code, static_cast<std::uint32_t>(
                    myrd::management::OperationStoreError::commit_indeterminate));
            }
        }
        state->reset();
        auto reopened = open_store(
            state, L"cleanup-reopen-" + std::to_wstring(boundary));
        REQUIRE(reopened.ok());
        auto records = reopened.value().list_records();
        REQUIRE(records.ok());
        REQUIRE(records.value().size() <= 1);
        if (!records.value().empty()) {
            REQUIRE_EQ(records.value()[0], terminal);
        }
        for (const auto& [name, unused] : state->files) {
            static_cast<void>(unused);
            REQUIRE(name == operation_leaf(terminal.operation_id)
                    || name == previous_name);
        }
    }
}

TEST_CASE(operation_cleanup_reports_exception_after_visible_deletion) {
    const auto terminal = terminal_record();
    auto state = seeded(terminal);
    auto store = open_store(state, L"cleanup-post-delete-exception");
    REQUIRE(store.ok());
    state->throw_after_remove = true;
    auto result = store.value().cleanup_terminal_records(
        terminal.terminal_unix_ms.value()
        + myrd::management::kOperationRetentionMilliseconds);
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
        myrd::management::OperationStoreError::commit_indeterminate));
}

TEST_CASE(operation_open_reports_exception_after_visible_temp_deletion) {
    auto state = std::make_shared<FaultState>();
    state->files[
        L".next.10000000-0000-4000-8000-000000000001.tmp"] = {
            std::byte{'x'}};
    state->throw_after_remove = true;
    auto store = open_store(state, L"open-post-temp-delete-exception");
    REQUIRE(!store.ok());
    REQUIRE_EQ(store.error().code, static_cast<std::uint32_t>(
        myrd::management::OperationStoreError::commit_indeterminate));
}
