#include "tests/support/test_framework.h"

#include "src/management/service_maintenance.h"

#include <string>

using namespace myrd;
using namespace myrd::management;

namespace {

MaintenanceInput running_input() {
    MaintenanceInput input{};
    input.state.lifecycle = profile::ProfileLifecycle::ready;
    input.state.disk.runtime = product::RuntimeState::running;
    input.state.disk.activity = product::ActivityState::idle;
    input.state.disk.persistence = product::PersistenceState::dirty;
    input.state.disk.health = product::HealthState::healthy;
    input.checkpoint = profile::CheckpointPolicy{};
    input.memory = profile::MemoryPolicy{};
    return input;
}

} // namespace

TEST_CASE(m5_maintenance_checkpoints_when_the_interval_has_elapsed) {
    // The consumer that did not exist: `interval_seconds` was serialized, validated
    // and read, and then dropped.  The policy is a pure function so this case can
    // ask it the only question that matters -- "is a checkpoint due?".
    auto input = running_input();
    input.checkpoint.interval_enabled = true;
    input.checkpoint.interval_seconds = 60;
    input.since_last_checkpoint_ms = 59'999;
    REQUIRE(checkpoint_due_reason(input).empty());
    input.since_last_checkpoint_ms = 60'000;
    REQUIRE_EQ(checkpoint_due_reason(input), std::string("interval"));
}

TEST_CASE(m5_maintenance_defaults_promise_at_most_one_minute_of_loss) {
    // **T1-10's defaults, pinned.**  The product promises that a crash loses at most one
    // interval; with a five-minute default that promise was true and useless, and with the
    // byte threshold off a disk that absorbed a gigabyte in ten seconds still waited out the
    // rest of the minute.  A default is only a promise if something asserts it.
    const profile::CheckpointPolicy defaults{};
    REQUIRE_EQ(defaults.interval_enabled, true);
    REQUIRE_EQ(defaults.interval_seconds, 60ULL);
    REQUIRE_EQ(defaults.dirty_threshold_enabled, true);
    REQUIRE_EQ(defaults.dirty_threshold_bytes, 64ULL * 1024ULL * 1024ULL);
}

TEST_CASE(m5_maintenance_interval_is_off_when_the_policy_says_so) {
    auto input = running_input();
    input.checkpoint.interval_enabled = false;
    input.checkpoint.interval_seconds = 60;
    input.since_last_checkpoint_ms = 10'000'000;
    REQUIRE(checkpoint_due_reason(input).empty());
}

TEST_CASE(m5_maintenance_checkpoints_on_the_dirty_threshold) {
    auto input = running_input();
    input.checkpoint.interval_enabled = false;
    input.checkpoint.dirty_threshold_enabled = true;
    input.checkpoint.dirty_threshold_bytes = 4ull * 1024 * 1024;
    input.dirty_bytes = 4ull * 1024 * 1024 - 1;
    REQUIRE(checkpoint_due_reason(input).empty());
    input.dirty_bytes = 4ull * 1024 * 1024;
    REQUIRE_EQ(checkpoint_due_reason(input), std::string("dirty-threshold"));
}

TEST_CASE(m5_maintenance_checkpoints_on_memory_pressure_before_the_threshold) {
    // Both are due; memory pressure wins because it is the condition that gets worse
    // on its own.  Asserting the *reason* and not just "a checkpoint" is the point:
    // the log a user reads has to say why the disk was written to.
    auto input = running_input();
    input.memory.soft_limit_bytes = 1000;
    input.resident_bytes = 1000;
    input.checkpoint.dirty_threshold_enabled = true;
    input.checkpoint.dirty_threshold_bytes = 1;
    input.dirty_bytes = 1;
    input.since_last_checkpoint_ms = 10'000'000;
    REQUIRE_EQ(checkpoint_due_reason(input), std::string("memory-pressure"));
}

TEST_CASE(m5_maintenance_ignores_a_profile_that_is_not_running) {
    // A stopped disk has nothing to checkpoint, and a `starting` or `stopping` one is
    // a window rather than a state to act in.
    for (const auto runtime : {product::RuntimeState::stopped,
                               product::RuntimeState::starting,
                               product::RuntimeState::stopping,
                               product::RuntimeState::orphaned}) {
        auto input = running_input();
        input.state.disk.runtime = runtime;
        input.checkpoint.interval_enabled = true;
        input.checkpoint.interval_seconds = 1;
        input.since_last_checkpoint_ms = 10'000'000;
        input.memory.soft_limit_bytes = 1;
        input.resident_bytes = 100;
        REQUIRE(checkpoint_due_reason(input).empty());
    }
}

TEST_CASE(m5_maintenance_treats_a_zero_memory_soft_limit_as_unconfigured) {
    // A profile whose memory limit was never set must not checkpoint on every tick:
    // `resident >= 0` is true for every running disk.
    auto input = running_input();
    input.memory.soft_limit_bytes = 0;
    input.resident_bytes = 1'000'000;
    input.checkpoint.interval_enabled = false;
    REQUIRE(checkpoint_due_reason(input).empty());
}
// **The compaction policy's own triggers.**  Same shape as the checkpoint cases above and for the same reason:
// `compaction` has been serialized since the schema was written, and until now nothing read it -- a disk grew
// until the journal filled and then failed, however the policy was configured.
TEST_CASE(m5_maintenance_compaction_waits_for_a_running_disk) {
    auto input = running_input();
    input.journal_occupancy_percent = 99;
    input.compaction.journal_occupancy_percent = 70;
    REQUIRE_EQ(compaction_due_reason(input), std::string("journal-occupancy"));

    input.state.disk.runtime = product::RuntimeState::stopped;
    REQUIRE(compaction_due_reason(input).empty());
}

TEST_CASE(m5_maintenance_compaction_respects_the_automatic_switch) {
    auto input = running_input();
    input.journal_occupancy_percent = 99;
    input.compaction.automatic = false;
    REQUIRE(compaction_due_reason(input).empty());
}

TEST_CASE(m5_maintenance_compaction_orders_occupancy_before_reclaimable_before_count) {
    auto input = running_input();
    input.journal_occupancy_percent = 80;
    input.reclaimable_percent = 60;
    input.checkpoints_since_compaction = 100;
    input.compaction = {};
    // All three say yes; the reason names the one the journal enforces first.
    REQUIRE_EQ(compaction_due_reason(input), std::string("journal-occupancy"));

    input.journal_occupancy_percent = 10;
    REQUIRE_EQ(compaction_due_reason(input), std::string("reclaimable"));

    input.reclaimable_percent = 10;
    REQUIRE_EQ(compaction_due_reason(input), std::string("checkpoint-count"));
}

TEST_CASE(m5_maintenance_compaction_ignores_triggers_set_to_zero) {
    // Zero means "this trigger is off", not "compact immediately" -- the mistake the dirty-threshold test above
    // exists to prevent.
    auto input = running_input();
    input.checkpoints_since_compaction = 1000;
    input.compaction.journal_occupancy_percent = 0;
    input.compaction.reclaimable_percent = 0;
    input.compaction.checkpoint_count_threshold = 0;
    REQUIRE(compaction_due_reason(input).empty());
}