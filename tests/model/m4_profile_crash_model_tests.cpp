#include "tests/support/test_framework.h"

#include "src/profile/profile_codec.h"
#include "src/profile/profile_store.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
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

myrd::profile::ProfileRecord record(std::uint64_t revision,
                                    std::wstring display_name) {
    return myrd::profile::ProfileRecord{
        revision,
        myrd::profile::Profile{
            uuid("00112233-4455-4677-8899-aabbccddeeff"),
            L"S-1-5-21-100-200-300-1001",
            std::move(display_name),
            1700000000000ULL,
            myrd::profile::ProfileLifecycle::provisioning,
            L"D:\\Images\\disk.bundle",
            "disk.rdm",
            uuid("10213243-5465-7687-98a9-bacbdcedfe0f"),
            8ULL * 1024ULL * 1024ULL * 1024ULL,
            512,
            4096,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            myrd::profile::MountKind::drive_letter,
            L"R:",
            true,
            myrd::profile::MemoryPolicy{
                myrd::profile::MemoryMode::automatic,
                2ULL * 1024ULL * 1024ULL * 1024ULL,
                1536ULL * 1024ULL * 1024ULL,
                1ULL * 1024ULL * 1024ULL * 1024ULL,
                256ULL * 1024ULL * 1024ULL},
            {},
            {}}};
}

std::vector<std::byte> encoded(const myrd::profile::ProfileRecord& value) {
    auto result = myrd::profile::encode_profile(value);
    REQUIRE(result.ok());
    return result.move_value();
}

myrd::Result<void> injected_failure(std::wstring_view operation) {
    return myrd::Result<void>::failure(myrd::Error{
        myrd::ErrorDomain::win32,
        static_cast<std::uint32_t>(myrd::profile::ProfileStoreError::io_failure),
        std::wstring(operation), L"Injected crash boundary", ERROR_WRITE_FAULT});
}

template <class T>
myrd::Result<T> injected_failure_value(std::wstring_view operation) {
    return myrd::Result<T>::failure(myrd::Error{
        myrd::ErrorDomain::win32,
        static_cast<std::uint32_t>(myrd::profile::ProfileStoreError::io_failure),
        std::wstring(operation), L"Injected crash boundary", ERROR_WRITE_FAULT});
}

class CrashDirectory final : public myrd::profile::IAtomicFileDirectory {
public:
    enum class Corruption {
        none,
        change_byte,
        truncate,
        append,
    };

    CrashDirectory() = default;
    explicit CrashDirectory(std::vector<std::byte> current)
        : files_{{L"profile.myrd", std::move(current)}} {}

    void fail_at(std::size_t boundary) { failure_boundary_ = boundary; }
    void throw_at(std::size_t boundary) { throw_boundary_ = boundary; }
    void corrupt_verification(Corruption value) { corruption_ = value; }
    void seed_file(std::wstring name, std::vector<std::byte> value) {
        files_[std::move(name)] = std::move(value);
    }
    std::size_t boundary_count() const noexcept { return boundary_; }
    const std::vector<std::wstring>& trace() const noexcept { return trace_; }

    const std::vector<std::byte>* file(std::wstring_view name) const {
        const auto found = files_.find(std::wstring(name));
        return found == files_.end() ? nullptr : &found->second;
    }

    bool no_temps() const noexcept { return temps_.empty(); }
    bool directory_flushed() const noexcept { return directory_flushed_; }
    std::optional<bool> replace_flag(std::wstring_view destination) const {
        const auto found = replace_flags_.find(std::wstring(destination));
        return found == replace_flags_.end()
            ? std::nullopt : std::optional<bool>(found->second);
    }

    myrd::Result<std::vector<std::byte>> read_file(
        std::wstring_view name) override {
        if (before(L"read." + std::wstring(name))) {
            return injected_failure_value<std::vector<std::byte>>(
                L"crash.read.before");
        }
        const auto found = files_.find(std::wstring(name));
        if (found == files_.end()) {
            return myrd::Result<std::vector<std::byte>>::failure(myrd::Error{
                myrd::ErrorDomain::protocol,
                static_cast<std::uint32_t>(
                    myrd::profile::ProfileStoreError::path_not_found),
                L"crash.read", L"File not found"});
        }
        auto value = found->second;
        if (after(L"read." + std::wstring(name))) {
            return injected_failure_value<std::vector<std::byte>>(
                L"crash.read.after");
        }
        return myrd::Result<std::vector<std::byte>>::success(std::move(value));
    }

    myrd::Result<myrd::profile::AtomicTempFile> create_unique_temp(
        std::wstring_view purpose) override {
        if (before(L"create." + std::wstring(purpose))) {
            return injected_failure_value<myrd::profile::AtomicTempFile>(
                L"crash.create.before");
        }
        const myrd::profile::AtomicTempFile token{next_token_++};
        temps_[token.value] = {};
        if (after(L"create." + std::wstring(purpose))) {
            return injected_failure_value<myrd::profile::AtomicTempFile>(
                L"crash.create.after");
        }
        return myrd::Result<myrd::profile::AtomicTempFile>::success(token);
    }

    myrd::Result<void> write_temp(
        myrd::profile::AtomicTempFile token,
        std::span<const std::byte> bytes) override {
        if (before(L"write.temp")) return injected_failure(L"crash.write.before");
        auto found = temps_.find(token.value);
        if (found == temps_.end()) return injected_failure(L"crash.write.token");
        found->second.assign(bytes.begin(), bytes.end());
        if (after(L"write.temp")) return injected_failure(L"crash.write.after");
        return myrd::Result<void>::success();
    }

    myrd::Result<void> flush_temp(
        myrd::profile::AtomicTempFile token) override {
        if (before(L"flush.temp")) return injected_failure(L"crash.flush.before");
        if (!temps_.contains(token.value)) {
            return injected_failure(L"crash.flush.token");
        }
        if (after(L"flush.temp")) return injected_failure(L"crash.flush.after");
        return myrd::Result<void>::success();
    }

    myrd::Result<std::vector<std::byte>> read_temp(
        myrd::profile::AtomicTempFile token) override {
        if (before(L"verify.temp")) {
            return injected_failure_value<std::vector<std::byte>>(
                L"crash.verify.before");
        }
        const auto found = temps_.find(token.value);
        if (found == temps_.end()) {
            return injected_failure_value<std::vector<std::byte>>(
                L"crash.verify.token");
        }
        auto value = found->second;
        if (corruption_ == Corruption::change_byte && !value.empty()) {
            value.front() ^= std::byte{0xff};
        } else if (corruption_ == Corruption::truncate && !value.empty()) {
            value.pop_back();
        } else if (corruption_ == Corruption::append) {
            value.push_back(std::byte{0x5a});
        }
        if (after(L"verify.temp")) {
            return injected_failure_value<std::vector<std::byte>>(
                L"crash.verify.after");
        }
        return myrd::Result<std::vector<std::byte>>::success(std::move(value));
    }

    myrd::Result<void> publish_temp(
        myrd::profile::AtomicTempFile token,
        std::wstring_view destination,
        bool replace_existing) override {
        const auto label = L"publish." + std::wstring(destination);
        if (before(label)) return injected_failure(L"crash.publish.before");
        const auto found = temps_.find(token.value);
        if (found == temps_.end()) return injected_failure(L"crash.publish.token");
        if (!replace_existing && files_.contains(std::wstring(destination))) {
            return injected_failure(L"crash.publish.exists");
        }
        replace_flags_[std::wstring(destination)] = replace_existing;
        files_[std::wstring(destination)] = found->second;
        temps_.erase(found);
        if (after(label)) return injected_failure(L"crash.publish.after");
        return myrd::Result<void>::success();
    }

    myrd::Result<void> flush_directory() override {
        if (before(L"flush.directory")) {
            return injected_failure(L"crash.directory_flush.before");
        }
        directory_flushed_ = true;
        if (after(L"flush.directory")) {
            return injected_failure(L"crash.directory_flush.after");
        }
        return myrd::Result<void>::success();
    }

    myrd::Result<void> remove_file(std::wstring_view name) override {
        files_.erase(std::wstring(name));
        return myrd::Result<void>::success();
    }
    myrd::Result<std::vector<std::wstring>> list_files() override {
        std::vector<std::wstring> names;
        names.reserve(files_.size());
        for (const auto& [name, unused] : files_) {
            static_cast<void>(unused);
            names.push_back(name);
        }
        return myrd::Result<std::vector<std::wstring>>::success(
            std::move(names));
    }

    void discard_temp(myrd::profile::AtomicTempFile token) noexcept override {
        temps_.erase(token.value);
    }

private:
    bool step(std::wstring label) {
        trace_.push_back(std::move(label));
        const auto current = boundary_++;
        if (throw_boundary_.has_value()
            && current == throw_boundary_.value()) {
            throw std::runtime_error("injected backend exception");
        }
        return failure_boundary_.has_value()
            && current == failure_boundary_.value();
    }

    bool before(std::wstring label) { return step(std::move(label) + L".before"); }
    bool after(std::wstring label) { return step(std::move(label) + L".after"); }

    std::map<std::wstring, std::vector<std::byte>, std::less<>> files_;
    std::map<std::uint64_t, std::vector<std::byte>> temps_;
    std::map<std::wstring, bool, std::less<>> replace_flags_;
    std::uint64_t next_token_{1};
    std::optional<std::size_t> failure_boundary_;
    std::optional<std::size_t> throw_boundary_;
    std::size_t boundary_{};
    std::vector<std::wstring> trace_;
    bool directory_flushed_{};
    Corruption corruption_{Corruption::none};
};

bool contains(const std::vector<std::wstring>& trace, std::wstring_view item) {
    return std::find(trace.begin(), trace.end(), item) != trace.end();
}

} // namespace

TEST_CASE(profile_atomic_publication_runs_every_durable_boundary) {
    const auto old_bytes = encoded(record(7, L"old"));
    const auto new_bytes = encoded(record(8, L"new"));
    CrashDirectory directory(old_bytes);

    auto result = myrd::profile::publish_atomically_with_previous(
        directory, L"profile.myrd", L"profile.prev", new_bytes);
    REQUIRE(result.ok());
    REQUIRE_EQ(*directory.file(L"profile.myrd"), new_bytes);
    REQUIRE_EQ(*directory.file(L"profile.prev"), old_bytes);
    REQUIRE(directory.no_temps());
    REQUIRE(directory.directory_flushed());
    for (const auto required : {
             L"create.next.before", L"write.temp.before",
             L"flush.temp.before", L"verify.temp.before",
             L"create.previous.before", L"publish.profile.prev.before",
             L"publish.profile.myrd.before", L"flush.directory.before"}) {
        REQUIRE(contains(directory.trace(), required));
    }
}

TEST_CASE(profile_atomic_publication_rejects_unsafe_names_and_oversized_data) {
    const auto old_bytes = encoded(record(7, L"old"));
    CrashDirectory directory(old_bytes);
    const std::vector<std::byte> oversized(
        myrd::profile::kProfileMaximumBytes + 1U, std::byte{0x5a});
    REQUIRE(!myrd::profile::publish_atomically_with_previous(
        directory, L"..\\profile.myrd", L"profile.prev", old_bytes).ok());
    REQUIRE(!myrd::profile::publish_atomically_with_previous(
        directory, L"profile.myrd", L"profile.prev", oversized).ok());
    REQUIRE_EQ(directory.boundary_count(), 0U);
    REQUIRE_EQ(*directory.file(L"profile.myrd"), old_bytes);
}

TEST_CASE(profile_atomic_crash_model_never_exposes_mixed_current_bytes) {
    const auto old_bytes = encoded(record(7, L"old"));
    const auto new_bytes = encoded(record(8, L"new"));
    CrashDirectory baseline(old_bytes);
    REQUIRE(myrd::profile::publish_atomically_with_previous(
        baseline, L"profile.myrd", L"profile.prev", new_bytes).ok());
    const auto boundary_count = baseline.boundary_count();
    REQUIRE(boundary_count >= 20);

    for (std::size_t boundary = 0; boundary < boundary_count; ++boundary) {
        CrashDirectory crashed(old_bytes);
        crashed.fail_at(boundary);
        REQUIRE(!myrd::profile::publish_atomically_with_previous(
            crashed, L"profile.myrd", L"profile.prev", new_bytes).ok());
        const auto* current = crashed.file(L"profile.myrd");
        REQUIRE(current != nullptr);
        REQUIRE(*current == old_bytes || *current == new_bytes);
        auto decoded = myrd::profile::decode_profile(*current);
        REQUIRE(decoded.ok());
        if (*current == new_bytes) {
            const auto* previous = crashed.file(L"profile.prev");
            REQUIRE(previous != nullptr);
            REQUIRE_EQ(*previous, old_bytes);
        }
        const auto* previous = crashed.file(L"profile.prev");
        REQUIRE(previous == nullptr || *previous == old_bytes);
    }
}

TEST_CASE(profile_atomic_initial_publication_has_a_complete_crash_matrix) {
    const auto new_bytes = encoded(record(1, L"initial"));
    CrashDirectory baseline;
    auto baseline_result = myrd::profile::publish_atomically_with_previous(
        baseline, L"profile.myrd", L"profile.prev", new_bytes);
    REQUIRE(baseline_result.ok());
    REQUIRE_EQ(*baseline.file(L"profile.myrd"), new_bytes);
    REQUIRE(baseline.file(L"profile.prev") == nullptr);
    REQUIRE_EQ(baseline.replace_flag(L"profile.myrd"),
               std::optional<bool>(false));
    REQUIRE(baseline.directory_flushed());

    for (std::size_t boundary = 0;
         boundary < baseline.boundary_count(); ++boundary) {
        CrashDirectory crashed;
        crashed.fail_at(boundary);
        auto result = myrd::profile::publish_atomically_with_previous(
            crashed, L"profile.myrd", L"profile.prev", new_bytes);
        REQUIRE(!result.ok());
        const auto* current = crashed.file(L"profile.myrd");
        REQUIRE(current == nullptr || *current == new_bytes);
        REQUIRE(crashed.file(L"profile.prev") == nullptr);
        if (current != nullptr) {
            REQUIRE_EQ(result.error().code,
                       static_cast<std::uint32_t>(
                           myrd::profile::ProfileStoreError::commit_indeterminate));
        }
    }
}

TEST_CASE(profile_atomic_publication_reports_indeterminate_commit_boundaries) {
    const auto old_bytes = encoded(record(7, L"old"));
    const auto new_bytes = encoded(record(8, L"new"));
    CrashDirectory baseline(old_bytes);
    REQUIRE(myrd::profile::publish_atomically_with_previous(
        baseline, L"profile.myrd", L"profile.prev", new_bytes).ok());

    for (std::size_t boundary = 0;
         boundary < baseline.trace().size(); ++boundary) {
        CrashDirectory crashed(old_bytes);
        crashed.fail_at(boundary);
        auto result = myrd::profile::publish_atomically_with_previous(
            crashed, L"profile.myrd", L"profile.prev", new_bytes);
        REQUIRE(!result.ok());
        const auto& label = baseline.trace()[boundary];
        const bool current_commit_boundary =
            label.starts_with(L"publish.profile.myrd")
            || label.starts_with(L"flush.directory");
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       current_commit_boundary
                           ? myrd::profile::ProfileStoreError::commit_indeterminate
                           : myrd::profile::ProfileStoreError::io_failure));
        if (label.starts_with(L"publish.profile.prev")) {
            REQUIRE_EQ(*crashed.file(L"profile.myrd"), old_bytes);
            const auto* previous = crashed.file(L"profile.prev");
            REQUIRE(previous == nullptr || *previous == old_bytes);
        }
    }
}

TEST_CASE(profile_atomic_publication_rejects_corrupt_temp_verification) {
    const auto old_bytes = encoded(record(7, L"old"));
    const auto new_bytes = encoded(record(8, L"new"));
    for (const auto corruption : {
             CrashDirectory::Corruption::change_byte,
             CrashDirectory::Corruption::truncate,
             CrashDirectory::Corruption::append}) {
        CrashDirectory directory(old_bytes);
        directory.corrupt_verification(corruption);
        auto result = myrd::profile::publish_atomically_with_previous(
            directory, L"profile.myrd", L"profile.prev", new_bytes);
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().code,
                   static_cast<std::uint32_t>(
                       myrd::profile::ProfileStoreError::io_failure));
        REQUIRE_EQ(*directory.file(L"profile.myrd"), old_bytes);
        REQUIRE(directory.file(L"profile.prev") == nullptr);
        REQUIRE(directory.no_temps());
    }
}

TEST_CASE(profile_atomic_publication_converts_backend_exceptions) {
    const auto old_bytes = encoded(record(7, L"old"));
    const auto new_bytes = encoded(record(8, L"new"));
    CrashDirectory baseline(old_bytes);
    REQUIRE(myrd::profile::publish_atomically_with_previous(
        baseline, L"profile.myrd", L"profile.prev", new_bytes).ok());

    for (std::size_t boundary = 0;
         boundary < baseline.boundary_count(); ++boundary) {
        CrashDirectory directory(old_bytes);
        directory.throw_at(boundary);
        bool escaped = false;
        myrd::Result<void> result = myrd::Result<void>::success();
        try {
            result = myrd::profile::publish_atomically_with_previous(
                directory, L"profile.myrd", L"profile.prev", new_bytes);
        } catch (...) {
            escaped = true;
        }
        REQUIRE(!escaped);
        REQUIRE(!result.ok());
        const auto* current = directory.file(L"profile.myrd");
        REQUIRE(current != nullptr);
        REQUIRE(*current == old_bytes || *current == new_bytes);
        if (*current == new_bytes) {
            REQUIRE_EQ(result.error().code,
                       static_cast<std::uint32_t>(
                           myrd::profile::ProfileStoreError::commit_indeterminate));
        }
    }
}

TEST_CASE(profile_repair_publication_preserves_previous_at_every_boundary) {
    const auto previous_bytes = encoded(record(1, L"previous"));
    auto corrupt_current = encoded(record(2, L"corrupt"));
    REQUIRE(corrupt_current.size() > myrd::profile::kProfileHeaderBytes);
    corrupt_current.back() ^= std::byte{0xff};
    const auto repaired_bytes = encoded(record(3, L"repaired"));

    CrashDirectory baseline(corrupt_current);
    baseline.seed_file(L"profile.prev", previous_bytes);
    REQUIRE(myrd::profile::publish_current_preserving_previous(
        baseline, L"profile.myrd", repaired_bytes).ok());
    REQUIRE_EQ(*baseline.file(L"profile.myrd"), repaired_bytes);
    REQUIRE_EQ(*baseline.file(L"profile.prev"), previous_bytes);
    REQUIRE_EQ(baseline.replace_flag(L"profile.myrd"),
               std::optional<bool>(true));

    for (std::size_t boundary = 0;
         boundary < baseline.boundary_count(); ++boundary) {
        CrashDirectory crashed(corrupt_current);
        crashed.seed_file(L"profile.prev", previous_bytes);
        crashed.fail_at(boundary);
        auto result = myrd::profile::publish_current_preserving_previous(
            crashed, L"profile.myrd", repaired_bytes);
        REQUIRE(!result.ok());
        const auto* current = crashed.file(L"profile.myrd");
        REQUIRE(current != nullptr);
        REQUIRE(*current == corrupt_current || *current == repaired_bytes);
        REQUIRE_EQ(*crashed.file(L"profile.prev"), previous_bytes);
        if (*current == repaired_bytes) {
            REQUIRE_EQ(result.error().code,
                       static_cast<std::uint32_t>(
                           myrd::profile::ProfileStoreError::commit_indeterminate));
        }
    }
}
