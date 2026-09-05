#pragma once

#include "src/management/service_core.h"

#include <optional>
#include <string_view>
#include <vector>

namespace myrd::tests {

class FakeServiceProfileStore final
    : public management::IServiceProfileStore {
public:
    Result<management::ManagedProfileSnapshot> resolve(
        std::wstring_view target) override;
    Result<management::ManagedProfileSnapshot> load(
        const product::Uuid& profile_id) override;
    Result<management::ManagedProfileSnapshot> publish_runtime(
        const management::ManagedProfileSnapshot& expected,
        const management::ProfileState& proposed_state,
        const management::OperationGateState& proposed_gate) override;

    void add(management::ManagedProfileSnapshot snapshot);
    void fail_publication_at(std::size_t attempt, Error error);
    void clear_publication_failure() noexcept;

    std::size_t resolve_calls{};
    std::size_t load_calls{};
    std::size_t publication_attempts{};
    std::vector<management::ManagedProfileSnapshot> publications;

private:
    std::vector<management::ManagedProfileSnapshot> profiles_;
    std::optional<std::size_t> failed_publication_attempt_;
    std::optional<Error> publication_failure_;
};

} // namespace myrd::tests
