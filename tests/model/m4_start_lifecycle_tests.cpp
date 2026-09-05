#include "tests/support/test_framework.h"

#include "src/host/device_lifecycle.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace {

using myrd::host::ILifecyclePhaseExecutor;
using myrd::host::LifecycleCommand;
using myrd::host::LifecycleCompletion;
using myrd::host::LifecycleIdentityState;
using myrd::host::LifecyclePhase;
using myrd::host::LifecyclePhaseDisposition;
using myrd::host::LifecyclePhaseExecution;
using myrd::host::LifecyclePlanDisposition;
using myrd::host::LifecycleProfilePublication;
using myrd::host::LifecycleRequest;
using myrd::host::LifecycleResource;
using myrd::host::LifecycleOwnershipToken;
using myrd::host::RegisteredLifecycle;

myrd::product::Uuid uuid(const char* text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

LifecycleRequest request(
    LifecycleCommand command,
    RegisteredLifecycle lifecycle,
    LifecycleIdentityState identity) {
    return LifecycleRequest{
        uuid("12345678-1234-4234-8234-1234567890ab"),
        uuid("abcdef01-2345-4678-9abc-def012345678"),
        command,
        lifecycle,
        identity};
}

std::vector<LifecyclePhase> phases(
    const myrd::host::LifecyclePlan& plan) {
    return {plan.phase_span().begin(), plan.phase_span().end()};
}

std::optional<LifecycleResource> resource_for(LifecyclePhase phase) {
    switch (phase) {
    case LifecyclePhase::create_runtime_and_worker:
        return LifecycleResource::runtime_and_worker;
    case LifecyclePhase::acquire_writer_lock_and_recover:
        return LifecycleResource::writer_lock;
    case LifecyclePhase::start_proxy:
        return LifecycleResource::proxy;
    case LifecyclePhase::create_exact_device:
        return LifecycleResource::exact_device;
    case LifecyclePhase::assign_exact_mount:
        return LifecycleResource::mount;
    default:
        return std::nullopt;
    }
}

class FakeExecutor final : public ILifecyclePhaseExecutor {
public:
    LifecyclePhaseExecution execute(
        const LifecycleRequest& value,
        LifecyclePhase phase) noexcept override {
        REQUIRE_EQ(value.operation_id, expected_operation);
        calls.push_back(phase);
        if (fail_at.has_value() && calls.size() - 1 == fail_at.value()) {
            return LifecyclePhaseExecution{failure};
        }
        const auto resource = resource_for(phase);
        if (resource.has_value() && reused_phase != phase) {
            return LifecyclePhaseExecution{
                LifecyclePhaseDisposition::succeeded,
                LifecycleOwnershipToken{
                    value.operation_id,
                    resource.value(),
                    static_cast<std::uint64_t>(calls.size())}};
        }
        return LifecyclePhaseExecution{
            LifecyclePhaseDisposition::succeeded};
    }

    bool cleanup_new_resource(
        const LifecycleRequest& value,
        const LifecycleOwnershipToken& token) noexcept override {
        REQUIRE_EQ(value.operation_id, expected_operation);
        REQUIRE_EQ(token.operation_id, value.operation_id);
        REQUIRE(token.nonce != 0);
        cleaned.push_back(token.resource);
        return cleanup_succeeds;
    }

    myrd::product::Uuid expected_operation =
        uuid("12345678-1234-4234-8234-1234567890ab");
    std::optional<std::size_t> fail_at;
    LifecyclePhaseDisposition failure{
        LifecyclePhaseDisposition::transient_failure};
    std::optional<LifecyclePhase> reused_phase;
    bool cleanup_succeeds{true};
    std::vector<LifecyclePhase> calls;
    std::vector<LifecycleResource> cleaned;
};

std::vector<LifecycleResource> cleanup_for_successful_prefix(
    std::span<const LifecyclePhase> plan,
    std::size_t failed_index) {
    std::vector<LifecycleResource> result;
    for (std::size_t index = 0; index < failed_index; ++index) {
        const auto resource = resource_for(plan[index]);
        if (resource.has_value()) result.push_back(resource.value());
    }
    std::reverse(result.begin(), result.end());
    return result;
}

} // namespace

TEST_CASE(lifecycle_complete_create_reaches_running_only_after_checkpoint_ready_and_io) {
    const auto input = request(
        LifecycleCommand::create_and_start,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::none);
    const auto plan = myrd::host::plan_lifecycle(input);
    REQUIRE_EQ(plan.disposition, LifecyclePlanDisposition::allowed);
    const std::vector<LifecyclePhase> expected{
        LifecyclePhase::create_and_verify_bundle,
        LifecyclePhase::publish_provisioning_profile,
        LifecyclePhase::create_runtime_and_worker,
        LifecyclePhase::acquire_writer_lock_and_recover,
        LifecyclePhase::start_proxy,
        LifecyclePhase::create_exact_device,
        LifecyclePhase::locate_exact_physical_drive,
        LifecyclePhase::prove_new_uninitialized,
        LifecyclePhase::provision_gpt_ntfs,
        LifecyclePhase::record_volume_identity,
        LifecyclePhase::flush_exact_volume,
        LifecyclePhase::first_checkpoint,
        LifecyclePhase::publish_ready_profile,
        LifecyclePhase::assign_exact_mount,
        LifecyclePhase::readiness_io};
    REQUIRE_EQ(phases(plan), expected);

    FakeExecutor executor;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(result.succeeded);
    REQUIRE_EQ(result.completion, LifecycleCompletion::running);
    REQUIRE_EQ(executor.calls, expected);
    REQUIRE(executor.cleaned.empty());
    REQUIRE(result.bundle_preserved);
    REQUIRE(result.profile_published);
    REQUIRE(result.volume_identity_recorded);
    REQUIRE(result.first_checkpoint_acknowledged);
    REQUIRE(result.ready_published);
    REQUIRE(!result.needs_attention);
}

TEST_CASE(lifecycle_no_start_stops_after_bundle_and_provisioning_profile) {
    const auto input = request(
        LifecycleCommand::create_stopped,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::none);
    const auto plan = myrd::host::plan_lifecycle(input);
    const std::vector<LifecyclePhase> expected{
        LifecyclePhase::create_and_verify_bundle,
        LifecyclePhase::publish_provisioning_profile};
    REQUIRE_EQ(plan.disposition, LifecyclePlanDisposition::allowed);
    REQUIRE_EQ(phases(plan), expected);

    FakeExecutor executor;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(result.succeeded);
    REQUIRE_EQ(result.completion, LifecycleCompletion::provisioning_stopped);
    REQUIRE_EQ(executor.calls, expected);
    REQUIRE(executor.cleaned.empty());
    REQUIRE(result.bundle_preserved);
    REQUIRE(result.profile_published);
    REQUIRE(!result.volume_identity_recorded);
    REQUIRE(!result.first_checkpoint_acknowledged);
    REQUIRE(!result.ready_published);
}

TEST_CASE(lifecycle_later_start_resumes_provisioning_without_unsafe_reformat) {
    const auto fresh = request(
        LifecycleCommand::start,
        RegisteredLifecycle::provisioning,
        LifecycleIdentityState::none);
    const auto fresh_plan = myrd::host::plan_lifecycle(fresh);
    REQUIRE_EQ(fresh_plan.disposition, LifecyclePlanDisposition::allowed);
    REQUIRE(std::find(
        fresh_plan.phase_span().begin(), fresh_plan.phase_span().end(),
        LifecyclePhase::provision_gpt_ntfs)
        != fresh_plan.phase_span().end());

    const auto already_formatted = request(
        LifecycleCommand::start,
        RegisteredLifecycle::provisioning,
        LifecycleIdentityState::complete);
    const auto resume_plan = myrd::host::plan_lifecycle(already_formatted);
    REQUIRE_EQ(resume_plan.disposition, LifecyclePlanDisposition::allowed);
    REQUIRE(std::find(
        resume_plan.phase_span().begin(), resume_plan.phase_span().end(),
        LifecyclePhase::provision_gpt_ntfs)
        == resume_plan.phase_span().end());
    REQUIRE(std::find(
        resume_plan.phase_span().begin(), resume_plan.phase_span().end(),
        LifecyclePhase::verify_existing_volume_identity)
        != resume_plan.phase_span().end());
    REQUIRE(std::find(
        resume_plan.phase_span().begin(), resume_plan.phase_span().end(),
        LifecyclePhase::first_checkpoint)
        != resume_plan.phase_span().end());

    const auto ambiguous = request(
        LifecycleCommand::start,
        RegisteredLifecycle::provisioning,
        LifecycleIdentityState::partial_or_mismatched);
    const auto rejected = myrd::host::plan_lifecycle(ambiguous);
    REQUIRE_EQ(
        rejected.disposition,
        LifecyclePlanDisposition::ambiguous_provisioning_identity);
    REQUIRE(rejected.phase_span().empty());

    FakeExecutor executor;
    const auto result = myrd::host::run_lifecycle(ambiguous, executor);
    REQUIRE(!result.succeeded);
    REQUIRE_EQ(
        result.completion,
        LifecycleCompletion::provisioning_stopped);
    REQUIRE_EQ(
        result.profile_publication,
        LifecycleProfilePublication::provisioning);
    REQUIRE(result.profile_published);
    REQUIRE(result.needs_attention);
    REQUIRE(executor.calls.empty());
    REQUIRE(executor.cleaned.empty());
}

TEST_CASE(lifecycle_import_is_read_only_until_one_atomic_ready_publication) {
    const auto input = request(
        LifecycleCommand::import_stopped,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::complete);
    const auto plan = myrd::host::plan_lifecycle(input);
    const std::vector<LifecyclePhase> expected{
        LifecyclePhase::inspect_bundle_read_only,
        LifecyclePhase::publish_imported_ready_profile};
    REQUIRE_EQ(plan.disposition, LifecyclePlanDisposition::allowed);
    REQUIRE_EQ(phases(plan), expected);

    FakeExecutor executor;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(result.succeeded);
    REQUIRE_EQ(result.completion, LifecycleCompletion::ready_stopped);
    REQUIRE_EQ(executor.calls, expected);
    REQUIRE(executor.cleaned.empty());
    REQUIRE(!result.bundle_preserved);
    REQUIRE(result.profile_published);
    REQUIRE(result.ready_published);
}

TEST_CASE(lifecycle_every_create_phase_failure_preserves_data_and_cleans_only_new_resources) {
    const auto input = request(
        LifecycleCommand::create_and_start,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::none);
    const auto plan = myrd::host::plan_lifecycle(input);
    REQUIRE_EQ(plan.disposition, LifecyclePlanDisposition::allowed);
    const auto all_phases = plan.phase_span();
    REQUIRE_EQ(all_phases.size(), 15U);

    for (std::size_t failed = 0; failed < all_phases.size(); ++failed) {
        FakeExecutor executor;
        executor.fail_at = failed;
        const auto result = myrd::host::run_lifecycle(input, executor);
        REQUIRE(!result.succeeded);
        REQUIRE_EQ(result.failed_phase, std::optional(all_phases[failed]));
        REQUIRE_EQ(executor.calls.size(), failed + 1);
        REQUIRE_EQ(
            executor.cleaned,
            cleanup_for_successful_prefix(all_phases, failed));
        REQUIRE(result.cleanup_complete);

        const auto bundle_completed = failed > 0;
        const auto profile_completed = failed > 1;
        const auto identity_completed = failed > 9;
        const auto checkpoint_completed = failed > 11;
        const auto ready_completed = failed > 12;
        REQUIRE_EQ(result.bundle_preserved, bundle_completed);
        REQUIRE_EQ(result.profile_published, profile_completed);
        REQUIRE_EQ(result.volume_identity_recorded, identity_completed);
        REQUIRE_EQ(
            result.first_checkpoint_acknowledged,
            checkpoint_completed);
        REQUIRE_EQ(result.ready_published, ready_completed);
        REQUIRE_EQ(result.needs_attention, profile_completed);
        REQUIRE_EQ(
            result.completion,
            !profile_completed
                ? LifecycleCompletion::no_profile
                : ready_completed
                    ? LifecycleCompletion::ready_stopped
                    : LifecycleCompletion::provisioning_stopped);
    }
}

TEST_CASE(lifecycle_commit_indeterminate_is_never_reported_as_transient_or_success) {
    const auto input = request(
        LifecycleCommand::create_and_start,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::none);
    FakeExecutor executor;
    executor.fail_at = 11;
    executor.failure = LifecyclePhaseDisposition::commit_indeterminate;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(!result.succeeded);
    REQUIRE_EQ(result.completion, LifecycleCompletion::commit_indeterminate);
    REQUIRE(result.needs_attention);
    REQUIRE(result.profile_published);
    REQUIRE(result.volume_identity_recorded);
    REQUIRE(!result.first_checkpoint_acknowledged);
    REQUIRE(!result.ready_published);
}

TEST_CASE(lifecycle_cleanup_requires_a_newly_owned_token_and_never_assumes_reuse_is_owned) {
    const auto input = request(
        LifecycleCommand::start,
        RegisteredLifecycle::ready,
        LifecycleIdentityState::complete);
    FakeExecutor executor;
    executor.reused_phase = LifecyclePhase::create_exact_device;
    executor.fail_at = 4;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(!result.succeeded);
    const std::vector<LifecycleResource> expected{
        LifecycleResource::proxy,
        LifecycleResource::writer_lock,
        LifecycleResource::runtime_and_worker};
    REQUIRE_EQ(executor.cleaned, expected);
    REQUIRE_EQ(result.cleaned_resources, expected);
    REQUIRE(std::find(
        executor.cleaned.begin(), executor.cleaned.end(),
        LifecycleResource::exact_device) == executor.cleaned.end());
}

TEST_CASE(lifecycle_provisioning_profile_publication_indeterminate_is_attention_not_absence) {
    const auto input = request(
        LifecycleCommand::create_and_start,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::none);
    FakeExecutor executor;
    executor.fail_at = 1;
    executor.failure = LifecyclePhaseDisposition::commit_indeterminate;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(!result.succeeded);
    REQUIRE_EQ(result.completion, LifecycleCompletion::commit_indeterminate);
    REQUIRE_EQ(
        result.profile_publication,
        LifecycleProfilePublication::commit_indeterminate);
    REQUIRE(!result.profile_published);
    REQUIRE(result.needs_attention);
    REQUIRE(result.bundle_preserved);
}

TEST_CASE(lifecycle_imported_profile_publication_indeterminate_is_attention_not_absence) {
    const auto input = request(
        LifecycleCommand::import_stopped,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::complete);
    FakeExecutor executor;
    executor.fail_at = 1;
    executor.failure = LifecyclePhaseDisposition::commit_indeterminate;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(!result.succeeded);
    REQUIRE_EQ(result.completion, LifecycleCompletion::commit_indeterminate);
    REQUIRE_EQ(
        result.profile_publication,
        LifecycleProfilePublication::commit_indeterminate);
    REQUIRE(!result.profile_published);
    REQUIRE(result.needs_attention);
    REQUIRE(executor.cleaned.empty());
}

TEST_CASE(lifecycle_ready_publication_indeterminate_preserves_known_provisioning_profile) {
    const auto input = request(
        LifecycleCommand::create_and_start,
        RegisteredLifecycle::absent,
        LifecycleIdentityState::none);
    FakeExecutor executor;
    executor.fail_at = 12;
    executor.failure = LifecyclePhaseDisposition::commit_indeterminate;
    const auto result = myrd::host::run_lifecycle(input, executor);
    REQUIRE(!result.succeeded);
    REQUIRE_EQ(result.completion, LifecycleCompletion::commit_indeterminate);
    REQUIRE_EQ(
        result.profile_publication,
        LifecycleProfilePublication::commit_indeterminate);
    REQUIRE(result.profile_published);
    REQUIRE(result.volume_identity_recorded);
    REQUIRE(result.first_checkpoint_acknowledged);
    REQUIRE(!result.ready_published);
    REQUIRE(result.needs_attention);
}
