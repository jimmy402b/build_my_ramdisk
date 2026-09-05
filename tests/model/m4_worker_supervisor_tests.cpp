#include "src/management/worker_supervisor.h"
#include "src/management/service_core.h"
#include "tests/support/m4_fake_clock.h"
#include "tests/support/test_framework.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::management;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

WorkerRuntimeIdentity identity(const std::uint64_t generation = 7) {
    return {
        uuid("11111111-1111-4111-8111-111111111111"),
        uuid("22222222-2222-4222-8222-222222222222"),
        4242,
        0x123456789ULL,
        generation,
        11,
        product::kProductVersionMajor,
        product::kProductVersionMinor,
        product::kProductVersionPatch};
}

WorkerDeviceIdentity device() {
    return {
        9,
        8ULL << 30,
        3,
        0,
        2,
        0,
        uuid("33333333-3333-4333-8333-333333333333"),
        uuid("44444444-4444-4444-8444-444444444444"),
        uuid("55555555-5555-4555-8555-555555555555")};
}

WorkerRuntimeSnapshot runtime(
    const WorkerRuntimeIdentity& expected,
    const std::uint64_t next_revision = 12) {
    auto next = expected;
    ++next.session_generation;
    next.profile_revision = next_revision;
    return {
        next,
        device(),
        10,
        20,
        91,
        true,
        true,
        6,
        99'000,
        uuid("66666666-6666-4666-8666-666666666666"),
        4,
        true};
}

Error fake_error(const WorkerSupervisorError code, const wchar_t* message) {
    return {
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(code),
        L"m4_worker_supervisor_tests",
        message};
}

struct ProcessState final {
    std::uint32_t pid{4242};
    std::uint64_t creation_time{0x123456789ULL};
    bool running{true};
    std::size_t pins{};
};

class Pin final : public IWorkerProcessIdentityPin {
public:
    explicit Pin(ProcessState& state) noexcept : state_(&state) {
        ++state_->pins;
    }
    ~Pin() noexcept override { --state_->pins; }
    std::uint32_t process_id() const noexcept override {
        return state_->pid;
    }
    std::uint64_t process_creation_time() const noexcept override {
        return state_->creation_time;
    }
    bool is_running() const noexcept override { return state_->running; }

private:
    ProcessState* state_{};
};

class Processes final : public IWorkerProcessIdentityQuery {
public:
    Result<std::unique_ptr<IWorkerProcessIdentityPin>> pin_process(
        const std::uint32_t process_id) override {
        if (process_id != state.pid) {
            return Result<std::unique_ptr<IWorkerProcessIdentityPin>>::failure(
                fake_error(
                    WorkerSupervisorError::process_identity_mismatch,
                    L"unknown process"));
        }
        std::unique_ptr<IWorkerProcessIdentityPin> pin =
            std::make_unique<Pin>(state);
        return Result<std::unique_ptr<IWorkerProcessIdentityPin>>::success(
            std::move(pin));
    }

    ProcessState state;
};

class Connector;

class Connection final : public IWorkerConnection {
public:
    explicit Connection(Connector& owner) noexcept : owner_(&owner) {}
    ~Connection() noexcept override;

private:
    Connector* owner_{};
};

class Connector final : public IAuthenticatedWorkerConnector {
public:
    Result<WorkerConnectionOpen> reattach_and_rotate(
        const WorkerRuntimeIdentity& expected,
        const std::uint64_t next_profile_revision,
        const product::Uuid&) override {
        ++attempts;
        if (active != 0) {
            return Result<WorkerConnectionOpen>::failure(fake_error(
                WorkerSupervisorError::duplicate_attachment,
                L"worker already has an attached manager"));
        }
        ++active;
        auto snapshot = runtime(expected, next_profile_revision);
        if (recover_current) snapshot.identity = expected;
        if (mutate_snapshot) ++snapshot.identity.worker_pid;
        if (process_to_stop != nullptr) process_to_stop->running = false;
        return Result<WorkerConnectionOpen>::success(WorkerConnectionOpen(
            std::make_unique<Connection>(*this),
            std::move(snapshot),
            recover_current
                ? WorkerReattachmentDisposition::recovered_current
                : WorkerReattachmentDisposition::rotated_successor));
    }

    std::size_t attempts{};
    std::size_t active{};
    bool mutate_snapshot{};
    bool recover_current{};
    ProcessState* process_to_stop{};
};

Connection::~Connection() noexcept { --owner_->active; }

class Scheduler final : public IReattachmentCheckpointScheduler {
public:
    Disposition enqueue_immediate_checkpoint_intent(
        const Intent& intent) noexcept override {
        ++calls;
        if (blocked) return Disposition::blocked;
        for (const auto& existing : intents) {
            if (existing.equivalent_checkpoint_truth(intent)) {
                return Disposition::already_pending;
            }
        }
        intents.push_back(intent);
        if (after_enqueue) after_enqueue();
        return Disposition::queued;
    }

    bool blocked{};
    std::size_t calls{};
    std::function<void()> after_enqueue;
    std::vector<Intent> intents;
};

struct Fixture final {
    Fixture() : clock(100'000, 1'000), supervisor(
        clock, processes, connector, scheduler) {}

    WorkerRuntimeSnapshot attach() {
        auto result = supervisor.reattach(
            identity(), 12,
            uuid("77777777-7777-4777-8777-777777777777"));
        REQUIRE(result.ok());
        return result.move_value();
    }

    tests::FakeServiceClock clock;
    Processes processes;
    Connector connector;
    Scheduler scheduler;
    WorkerSupervisor supervisor;
};

TEST_CASE(worker_supervisor_uses_exact_heartbeat_boundaries_without_sleep) {
    Fixture fixture;
    fixture.attach();

    fixture.clock.advance(kWorkerHeartbeatIntervalMs - 1);
    auto observed = fixture.supervisor.observe();
    REQUIRE_EQ(observed.health, product::HealthState::healthy);
    REQUIRE(!observed.heartbeat_due);
    REQUIRE(!observed.reconnect_required);

    fixture.clock.advance(1);
    observed = fixture.supervisor.observe();
    REQUIRE(observed.heartbeat_due);
    REQUIRE_EQ(observed.health, product::HealthState::healthy);

    fixture.clock.advance(
        kWorkerDegradedAfterMs - kWorkerHeartbeatIntervalMs - 1);
    observed = fixture.supervisor.observe();
    REQUIRE_EQ(observed.health, product::HealthState::healthy);
    REQUIRE(!observed.reconnect_required);

    fixture.clock.advance(1);
    observed = fixture.supervisor.observe();
    REQUIRE_EQ(observed.health, product::HealthState::degraded);
    REQUIRE(observed.reconnect_required);
    REQUIRE(!observed.automatic_kill_allowed);

    fixture.clock.advance(
        kWorkerNeedsAttentionAfterMs - kWorkerDegradedAfterMs - 1);
    REQUIRE_EQ(
        fixture.supervisor.observe().health,
        product::HealthState::degraded);
    fixture.clock.advance(1);
    observed = fixture.supervisor.observe();
    REQUIRE_EQ(observed.health, product::HealthState::needs_attention);
    REQUIRE(!observed.automatic_kill_allowed);
}

TEST_CASE(worker_supervisor_valid_heartbeat_recovers_from_needs_attention) {
    Fixture fixture;
    auto heartbeat = fixture.attach();
    fixture.clock.advance(kWorkerNeedsAttentionAfterMs);
    REQUIRE_EQ(
        fixture.supervisor.observe().health,
        product::HealthState::needs_attention);

    ++heartbeat.heartbeat_sequence;
    ++heartbeat.snapshot_sequence;
    ++heartbeat.dirty_epoch;
    auto accepted = fixture.supervisor.accept_heartbeat(heartbeat);
    REQUIRE(accepted.ok());
    const auto observed = fixture.supervisor.observe();
    REQUIRE_EQ(observed.health, product::HealthState::healthy);
    REQUIRE(!observed.heartbeat_due);
    REQUIRE(!observed.reconnect_required);
    REQUIRE_EQ(observed.runtime.value(), heartbeat);
}

TEST_CASE(worker_supervisor_rejects_stale_heartbeat_without_resetting_timer) {
    Fixture fixture;
    auto heartbeat = fixture.attach();
    fixture.clock.advance(kWorkerDegradedAfterMs);

    ++heartbeat.heartbeat_sequence;

    auto rejected = fixture.supervisor.accept_heartbeat(heartbeat);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(
        fixture.supervisor.observe().health,
        product::HealthState::degraded);
}

TEST_CASE(worker_supervisor_holds_process_identity_pin_for_attachment_lifetime) {
    Fixture fixture;
    fixture.attach();
    REQUIRE_EQ(fixture.processes.state.pins, 1U);
    fixture.supervisor.disconnect();
    REQUIRE_EQ(fixture.processes.state.pins, 0U);
    REQUIRE_EQ(fixture.connector.active, 0U);
}

TEST_CASE(worker_supervisor_denies_pid_reuse_before_connector_access) {
    Fixture fixture;
    ++fixture.processes.state.creation_time;
    auto attached = fixture.supervisor.reattach(
        identity(), 12,
        uuid("77777777-7777-4777-8777-777777777777"));
    REQUIRE(!attached.ok());
    REQUIRE_EQ(fixture.connector.attempts, 0U);
    REQUIRE_EQ(fixture.processes.state.pins, 0U);
}

TEST_CASE(worker_supervisor_denies_mismatched_authenticated_snapshot) {
    Fixture fixture;
    fixture.connector.mutate_snapshot = true;
    auto attached = fixture.supervisor.reattach(
        identity(), 12,
        uuid("77777777-7777-4777-8777-777777777777"));
    REQUIRE(!attached.ok());
    REQUIRE_EQ(fixture.connector.active, 0U);
    REQUIRE_EQ(fixture.processes.state.pins, 0U);
}

TEST_CASE(worker_supervisor_revalidates_pinned_process_after_authentication) {
    Fixture fixture;
    fixture.connector.process_to_stop = &fixture.processes.state;
    auto attached = fixture.supervisor.reattach(
        identity(), 12,
        uuid("77777777-7777-4777-8777-777777777777"));
    REQUIRE(!attached.ok());
    REQUIRE_EQ(fixture.connector.active, 0U);
    REQUIRE_EQ(fixture.processes.state.pins, 0U);
}

TEST_CASE(worker_supervisor_accepts_authenticated_current_generation_recovery) {
    Fixture fixture;
    fixture.connector.recover_current = true;
    auto attached = fixture.supervisor.reattach(
        identity(), 12,
        uuid("77777777-7777-4777-8777-777777777777"));
    REQUIRE(attached.ok());
    REQUIRE_EQ(attached.value().identity, identity());
}

TEST_CASE(worker_supervisor_denies_duplicate_manager_and_never_kills_worker) {
    Fixture fixture;
    fixture.attach();
    auto duplicate = fixture.supervisor.reattach(
        identity(8), 13,
        uuid("88888888-8888-4888-8888-888888888888"));
    REQUIRE(!duplicate.ok());
    REQUIRE_EQ(fixture.connector.active, 1U);
    fixture.clock.advance(kWorkerNeedsAttentionAfterMs * 10);
    const auto observed = fixture.supervisor.observe();
    REQUIRE_EQ(observed.health, product::HealthState::needs_attention);
    REQUIRE(!observed.automatic_kill_allowed);
    REQUIRE_EQ(fixture.connector.active, 1U);
}

TEST_CASE(worker_supervisor_enqueues_exact_overdue_dirty_checkpoint_intent) {
    Fixture fixture;
    const auto attached = fixture.attach();
    REQUIRE_EQ(fixture.scheduler.intents.size(), 1U);
    const auto& intent = fixture.scheduler.intents.front();
    REQUIRE_EQ(intent.profile_id, attached.identity.profile_id);
    REQUIRE_EQ(intent.image_id, attached.identity.image_id);
    REQUIRE_EQ(
        intent.worker_session_generation,
        attached.identity.session_generation);
    REQUIRE_EQ(intent.snapshot_sequence, attached.snapshot_sequence);
    REQUIRE_EQ(intent.dirty_epoch, attached.dirty_epoch);
    REQUIRE_EQ(intent.checkpoint_policy_revision, 6U);
    REQUIRE_EQ(intent.checkpoint_deadline_unix_ms, 99'000U);
}

TEST_CASE(worker_supervisor_does_not_enqueue_not_due_checkpoint_intent) {
    Fixture fixture;
    fixture.clock.set(98'999, 1'000);
    fixture.attach();
    REQUIRE(fixture.scheduler.intents.empty());
}

TEST_CASE(worker_supervisor_retains_blocked_checkpoint_until_one_retry_queues) {
    Fixture fixture;
    fixture.scheduler.blocked = true;
    auto attached = fixture.attach();
    REQUIRE(fixture.scheduler.intents.empty());
    REQUIRE_EQ(fixture.scheduler.calls, 1U);
    auto observed = fixture.supervisor.observe();
    REQUIRE(observed.checkpoint_intent_pending);
    REQUIRE_EQ(observed.health, product::HealthState::degraded);

    fixture.scheduler.blocked = false;
    ++attached.heartbeat_sequence;
    ++attached.snapshot_sequence;
    REQUIRE(fixture.supervisor.accept_heartbeat(attached).ok());
    REQUIRE_EQ(fixture.scheduler.calls, 2U);
    REQUIRE_EQ(fixture.scheduler.intents.size(), 1U);
    observed = fixture.supervisor.observe();
    REQUIRE(!observed.checkpoint_intent_pending);
    REQUIRE_EQ(observed.health, product::HealthState::healthy);

    ++attached.heartbeat_sequence;
    ++attached.snapshot_sequence;
    REQUIRE(fixture.supervisor.accept_heartbeat(attached).ok());
    REQUIRE_EQ(fixture.scheduler.calls, 2U);
    REQUIRE_EQ(fixture.scheduler.intents.size(), 1U);
}

TEST_CASE(worker_supervisor_cancels_blocked_intent_on_clean_or_disabled_policy) {
    {
        Fixture fixture;
        fixture.scheduler.blocked = true;
        auto heartbeat = fixture.attach();
        heartbeat.dirty = false;
        ++heartbeat.heartbeat_sequence;
        ++heartbeat.snapshot_sequence;
        REQUIRE(fixture.supervisor.accept_heartbeat(heartbeat).ok());
        REQUIRE_EQ(fixture.scheduler.calls, 1U);
        REQUIRE(!fixture.supervisor.observe().checkpoint_intent_pending);
        REQUIRE(!fixture.supervisor.retry_pending_checkpoint_intent().has_value());
    }
    {
        Fixture fixture;
        fixture.scheduler.blocked = true;
        auto heartbeat = fixture.attach();
        heartbeat.checkpoint_policy_enabled = false;
        ++heartbeat.heartbeat_sequence;
        ++heartbeat.snapshot_sequence;
        REQUIRE(fixture.supervisor.accept_heartbeat(heartbeat).ok());
        REQUIRE_EQ(fixture.scheduler.calls, 1U);
        REQUIRE(!fixture.supervisor.observe().checkpoint_intent_pending);
        REQUIRE(!fixture.supervisor.retry_pending_checkpoint_intent().has_value());
    }
}

TEST_CASE(worker_supervisor_refreshes_blocked_intent_to_latest_dirty_epoch) {
    Fixture fixture;
    fixture.scheduler.blocked = true;
    auto heartbeat = fixture.attach();
    ++heartbeat.heartbeat_sequence;
    ++heartbeat.snapshot_sequence;
    ++heartbeat.dirty_epoch;
    fixture.scheduler.blocked = false;

    REQUIRE(fixture.supervisor.accept_heartbeat(heartbeat).ok());
    REQUIRE_EQ(fixture.scheduler.calls, 2U);
    REQUIRE_EQ(fixture.scheduler.intents.size(), 1U);
    REQUIRE_EQ(
        fixture.scheduler.intents.front().snapshot_sequence,
        heartbeat.snapshot_sequence);
    REQUIRE_EQ(
        fixture.scheduler.intents.front().dirty_epoch,
        heartbeat.dirty_epoch);
    REQUIRE(!fixture.supervisor.observe().checkpoint_intent_pending);
}

TEST_CASE(worker_supervisor_refreshes_blocked_intent_to_latest_policy_truth) {
    Fixture fixture;
    fixture.scheduler.blocked = true;
    auto heartbeat = fixture.attach();
    ++heartbeat.heartbeat_sequence;
    ++heartbeat.snapshot_sequence;
    ++heartbeat.checkpoint_policy_revision;
    heartbeat.checkpoint_deadline_unix_ms = 99'500;
    fixture.scheduler.blocked = false;

    REQUIRE(fixture.supervisor.accept_heartbeat(heartbeat).ok());
    REQUIRE_EQ(fixture.scheduler.calls, 2U);
    REQUIRE_EQ(fixture.scheduler.intents.size(), 1U);
    REQUIRE_EQ(
        fixture.scheduler.intents.front().checkpoint_policy_revision,
        heartbeat.checkpoint_policy_revision);
    REQUIRE_EQ(
        fixture.scheduler.intents.front().checkpoint_deadline_unix_ms,
        heartbeat.checkpoint_deadline_unix_ms);
    REQUIRE(!fixture.supervisor.observe().checkpoint_intent_pending);
}

TEST_CASE(worker_supervisor_disconnect_during_checkpoint_enqueue_is_safe) {
    Fixture fixture;
    fixture.scheduler.blocked = true;
    fixture.attach();
    fixture.scheduler.blocked = false;
    fixture.scheduler.after_enqueue = [&fixture] {
        fixture.supervisor.disconnect();
    };

    const auto retried = fixture.supervisor.retry_pending_checkpoint_intent();
    REQUIRE(retried.has_value());
    REQUIRE_EQ(
        retried.value(),
        IReattachmentCheckpointScheduler::Disposition::queued);
    REQUIRE_EQ(fixture.scheduler.intents.size(), 1U);
    const auto observed = fixture.supervisor.observe();
    REQUIRE(!observed.attached);
    REQUIRE(!observed.checkpoint_intent_pending);
    REQUIRE_EQ(fixture.processes.state.pins, 0U);
    REQUIRE_EQ(fixture.connector.active, 0U);
    REQUIRE(!fixture.supervisor.retry_pending_checkpoint_intent().has_value());
}

TEST_CASE(worker_supervisor_treats_monotonic_clock_rollback_as_attention) {
    Fixture fixture;
    fixture.attach();
    fixture.clock.set(100'000, 999);
    const auto observed = fixture.supervisor.observe();
    REQUIRE_EQ(observed.health, product::HealthState::needs_attention);
    REQUIRE(observed.reconnect_required);
    REQUIRE(!observed.automatic_kill_allowed);
}

TEST_CASE(worker_supervisor_scm_policy_has_exact_delays_and_reset_boundary) {
    REQUIRE_EQ(kScmRestartDelaysSeconds[0], 5U);
    REQUIRE_EQ(kScmRestartDelaysSeconds[1], 15U);
    REQUIRE_EQ(kScmRestartDelaysSeconds[2], 60U);
    constexpr std::uint64_t start = 1'000;
    constexpr std::uint64_t reset =
        static_cast<std::uint64_t>(kScmFailureResetSeconds) * 1'000U;
    REQUIRE(!scm_failure_count_should_reset(start, start + reset - 1));
    REQUIRE(scm_failure_count_should_reset(start, start + reset));
    REQUIRE(!scm_failure_count_should_reset(start, start - 1));
}

} // namespace
