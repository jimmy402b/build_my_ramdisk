#include "tests/support/m4_fake_profile_store.h"

#include <string>
#include <utility>

namespace myrd::tests {
namespace {

template <class T>
Result<T> profile_error(const wchar_t* operation, const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::internal,
        static_cast<std::uint32_t>(
            management::ServiceCoreError::profile_not_found),
        operation,
        message});
}

std::wstring wide_uuid(const product::Uuid& value) {
    const auto text = value.to_string();
    return std::wstring(text.begin(), text.end());
}

} // namespace

Result<management::ManagedProfileSnapshot>
FakeServiceProfileStore::resolve(std::wstring_view target) {
    ++resolve_calls;
    for (const auto& profile : profiles_) {
        if (profile.record.profile.display_name == target
            || wide_uuid(profile.record.profile.profile_uuid) == target) {
            return Result<management::ManagedProfileSnapshot>::success(
                profile);
        }
    }
    return profile_error<management::ManagedProfileSnapshot>(
        L"fake_profile.resolve", L"Fake profile target was not found");
}

Result<management::ManagedProfileSnapshot>
FakeServiceProfileStore::load(const product::Uuid& profile_id) {
    ++load_calls;
    for (const auto& profile : profiles_) {
        if (profile.record.profile.profile_uuid == profile_id) {
            return Result<management::ManagedProfileSnapshot>::success(
                profile);
        }
    }
    return profile_error<management::ManagedProfileSnapshot>(
        L"fake_profile.load", L"Fake profile identifier was not found");
}

Result<management::ManagedProfileSnapshot>
FakeServiceProfileStore::publish_runtime(
    const management::ManagedProfileSnapshot& expected,
    const management::ProfileState& proposed_state,
    const management::OperationGateState& proposed_gate) {
    ++publication_attempts;
    if (failed_publication_attempt_.has_value()
        && failed_publication_attempt_.value() == publication_attempts) {
        return Result<management::ManagedProfileSnapshot>::failure(
            publication_failure_.value());
    }
    for (auto& profile : profiles_) {
        if (profile.record.profile.profile_uuid
                != expected.record.profile.profile_uuid) {
            continue;
        }
        if (profile != expected) {
            return profile_error<management::ManagedProfileSnapshot>(
                L"fake_profile.publish",
                L"Fake profile compare-and-swap conflict");
        }
        profile.state = proposed_state;
        profile.operation_gate = proposed_gate;
        publications.push_back(profile);
        return Result<management::ManagedProfileSnapshot>::success(profile);
    }
    return profile_error<management::ManagedProfileSnapshot>(
        L"fake_profile.publish", L"Fake profile identifier was not found");
}

void FakeServiceProfileStore::add(
    management::ManagedProfileSnapshot snapshot) {
    profiles_.push_back(std::move(snapshot));
}

void FakeServiceProfileStore::fail_publication_at(
    std::size_t attempt, Error error) {
    failed_publication_attempt_ = attempt;
    publication_failure_ = std::move(error);
}

void FakeServiceProfileStore::clear_publication_failure() noexcept {
    failed_publication_attempt_.reset();
    publication_failure_.reset();
}

} // namespace myrd::tests
