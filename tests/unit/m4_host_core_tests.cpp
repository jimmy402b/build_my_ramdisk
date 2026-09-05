#include "tests/support/test_framework.h"

#include "src/host/host_core.h"
#include "src/m3/m3_provider.h"
#include "src/product/product_version.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

myrd::host::WorkerBinding binding(std::string_view profile) {
    return myrd::host::WorkerBinding{
        uuid(profile),
        uuid("10213243-5465-4687-98a9-bacbdcedfe0f"),
        42,
        100,
        7,
        3,
        myrd::product::kProductVersionMajor,
        myrd::product::kProductVersionMinor,
        myrd::product::kProductVersionPatch};
}

myrd::host::HostCommand command(
    const myrd::host::WorkerBinding& target,
    std::string_view operation) {
    return myrd::host::HostCommand{target, uuid(operation)};
}

class FakeProvider final : public myrd::host::IHostProvider {
public:
    myrd::Result<void> start(const myrd::OperationContext&) override {
        ++start_calls;
        if (start_sets_ready) {
            state_value = myrd::host::HostProviderState::ready;
        }
        return myrd::Result<void>::success();
    }

    myrd::host::HostCheckpointOutcome checkpoint(
        const myrd::OperationContext&,
        myrd::host::HostVolumeBarrierKind) override {
        ++checkpoint_calls;
        {
            std::unique_lock lock(block_mutex);
            checkpoint_entered = true;
            block_cv.notify_all();
            block_cv.wait(lock, [&] { return !block_checkpoint; });
        }
        return {myrd::host::HostCommitDisposition::committed, 8};
    }

    myrd::host::HostStopOutcome stop(
        const myrd::OperationContext&,
        myrd::host::HostStopRequest) override {
        ++stop_calls;
        state_value = myrd::host::HostProviderState::stopped;
        if (next_stop_outcome.has_value()) return *next_stop_outcome;
        myrd::host::HostStopOutcome outcome{};
        outcome.transport_stopped = true;
        outcome.store_closed = true;
        outcome.session_closed = true;
        outcome.writer_closed = true;
        return outcome;
    }

    myrd::Result<myrd::host::HostProviderState> state() const override {
        return myrd::Result<myrd::host::HostProviderState>::success(
            state_value);
    }

    myrd::Result<std::uint64_t> generation() const override {
        return myrd::Result<std::uint64_t>::success(7);
    }

    myrd::Result<myrd::host::HostGeometry> geometry() const override {
        return myrd::Result<myrd::host::HostGeometry>::success(
            {8ULL << 30, 512, 4096});
    }

    myrd::Result<myrd::host::HostStorageStats> storage_stats()
        const override {
        return myrd::Result<myrd::host::HostStorageStats>::success({});
    }

    void wait_for_checkpoint() {
        std::unique_lock lock(block_mutex);
        block_cv.wait(lock, [&] { return checkpoint_entered; });
    }

    void release_checkpoint() {
        std::scoped_lock lock(block_mutex);
        block_checkpoint = false;
        block_cv.notify_all();
    }

    std::atomic<unsigned> start_calls{};
    std::atomic<unsigned> checkpoint_calls{};
    std::atomic<unsigned> stop_calls{};
    myrd::host::HostProviderState state_value{
        myrd::host::HostProviderState::constructed};
    bool block_checkpoint{};
    bool start_sets_ready{true};
    std::optional<myrd::host::HostStopOutcome> next_stop_outcome;

private:
    std::mutex block_mutex;
    std::condition_variable block_cv;
    bool checkpoint_entered{};
};

class FakeProviderFactory final : public myrd::host::IHostProviderFactory {
public:
    myrd::Result<std::shared_ptr<myrd::host::IHostProvider>> open(
        const myrd::OperationContext&) override {
        ++open_calls;
        return myrd::Result<std::shared_ptr<myrd::host::IHostProvider>>::
            success(provider);
    }

    std::atomic<unsigned> open_calls{};
    std::shared_ptr<FakeProvider> provider = std::make_shared<FakeProvider>();
};

} // namespace

TEST_CASE(host_core_construction_is_side_effect_free_and_owns_one_provider) {
    const auto expected = binding("00112233-4455-4677-8899-aabbccddeeff");
    auto factory = std::make_shared<FakeProviderFactory>();

    auto created = myrd::host::HostCore::create(expected, factory);
    REQUIRE(created.ok());
    REQUIRE_EQ(factory->open_calls.load(), 0U);

    auto started = created.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000001"), {});
    REQUIRE(started.ok());
    REQUIRE_EQ(factory->open_calls.load(), 1U);
    REQUIRE_EQ(factory->provider->start_calls.load(), 1U);

    auto duplicate = created.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000002"), {});
    REQUIRE(!duplicate.ok());
    REQUIRE_EQ(factory->open_calls.load(), 1U);
}

TEST_CASE(host_core_rejects_cross_profile_before_provider_access) {
    const auto expected = binding("00112233-4455-4677-8899-aabbccddeeff");
    const auto foreign = binding("11112233-4455-4677-8899-aabbccddeeff");
    auto factory = std::make_shared<FakeProviderFactory>();
    auto created = myrd::host::HostCore::create(expected, factory);
    REQUIRE(created.ok());

    auto result = created.value()->start(
        command(foreign, "30000000-0000-4000-8000-000000000003"), {});
    REQUIRE(!result.ok());
    REQUIRE(result.error.has_value());
    REQUIRE_EQ(
        result.error->code,
        static_cast<std::uint32_t>(
            myrd::host::HostCoreError::binding_mismatch));
    REQUIRE_EQ(factory->open_calls.load(), 0U);
}

TEST_CASE(host_core_adopts_only_a_forward_session_for_the_same_live_worker) {
    const auto expected = binding("00112233-4455-4677-8899-aabbccddeeff");
    auto factory = std::make_shared<FakeProviderFactory>();
    auto created = myrd::host::HostCore::create(expected, factory);
    REQUIRE(created.ok());
    REQUIRE(created.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000013"), {}).ok());

    auto successor = expected;
    ++successor.session_generation;
    auto before_adoption = created.value()->checkpoint(
        command(successor, "30000000-0000-4000-8000-000000000014"), {},
        myrd::host::HostVolumeBarrierKind::windows_volume_flushed);
    REQUIRE(!before_adoption.ok());
    REQUIRE_EQ(factory->provider->checkpoint_calls.load(), 0U);

    REQUIRE(created.value()->adopt_reattached_binding(successor).ok());
    REQUIRE(created.value()->adopt_reattached_binding(successor).ok());
    const auto after_adoption = created.value()->checkpoint(
        command(successor, "30000000-0000-4000-8000-000000000015"), {},
        myrd::host::HostVolumeBarrierKind::windows_volume_flushed);
    REQUIRE(after_adoption.ok());
    REQUIRE_EQ(factory->provider->checkpoint_calls.load(), 1U);

    REQUIRE(!created.value()->adopt_reattached_binding(expected).ok());
    auto changed_image = successor;
    changed_image.image_id =
        uuid("90000000-0000-4000-8000-000000000001");
    ++changed_image.session_generation;
    REQUIRE(!created.value()->adopt_reattached_binding(changed_image).ok());
    auto changed_process = successor;
    ++changed_process.worker_pid;
    ++changed_process.session_generation;
    REQUIRE(!created.value()->adopt_reattached_binding(changed_process).ok());
    auto changed_revision = successor;
    ++changed_revision.profile_revision;
    ++changed_revision.session_generation;
    REQUIRE(!created.value()->adopt_reattached_binding(changed_revision).ok());
}

TEST_CASE(host_core_cleans_up_when_successful_start_is_not_ready) {
    const auto expected = binding("00112233-4455-4677-8899-aabbccddeeff");
    auto factory = std::make_shared<FakeProviderFactory>();
    factory->provider->start_sets_ready = false;
    auto created = myrd::host::HostCore::create(expected, factory);
    REQUIRE(created.ok());

    const auto result = created.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000011"), {});
    REQUIRE(!result.ok());
    REQUIRE(result.provider_opened);
    REQUIRE(!result.provider_started);
    REQUIRE(result.error.has_value());
    REQUIRE_EQ(
        result.error->code,
        static_cast<std::uint32_t>(
            myrd::host::HostCoreError::adapter_invariant));
    REQUIRE(result.cleanup.has_value());
    REQUIRE_EQ(factory->provider->stop_calls.load(), 1U);
    REQUIRE(!created.value()->provider_state(expected).ok());
}

TEST_CASE(host_core_rejects_overlapping_mutations_without_waiting) {
    const auto expected = binding("00112233-4455-4677-8899-aabbccddeeff");
    auto factory = std::make_shared<FakeProviderFactory>();
    auto created = myrd::host::HostCore::create(expected, factory);
    REQUIRE(created.ok());
    REQUIRE(created.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000004"), {}).ok());

    factory->provider->block_checkpoint = true;
    myrd::host::HostCheckpointOutcome checkpoint{};
    std::thread worker([&] {
        checkpoint = created.value()->checkpoint(
            command(expected, "30000000-0000-4000-8000-000000000005"),
            {}, myrd::host::HostVolumeBarrierKind::windows_volume_flushed);
    });
    factory->provider->wait_for_checkpoint();

    auto successor = expected;
    ++successor.session_generation;
    const auto adopted =
        created.value()->adopt_reattached_binding(successor);
    REQUIRE(!adopted.ok());
    REQUIRE_EQ(
        adopted.error().code,
        static_cast<std::uint32_t>(myrd::host::HostCoreError::mutation_busy));

    const auto stopped = created.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000006"), {}, {});
    REQUIRE(!stopped.ok());
    REQUIRE(stopped.error.has_value());
    REQUIRE_EQ(
        stopped.error->code,
        static_cast<std::uint32_t>(myrd::host::HostCoreError::mutation_busy));
    REQUIRE_EQ(factory->provider->stop_calls.load(), 0U);

    factory->provider->release_checkpoint();
    worker.join();
    REQUIRE(checkpoint.ok());
}

TEST_CASE(host_core_rejects_unknown_barriers_before_provider_access) {
    const auto expected = binding("00112233-4455-4677-8899-aabbccddeeff");
    auto factory = std::make_shared<FakeProviderFactory>();
    auto created = myrd::host::HostCore::create(expected, factory);
    REQUIRE(created.ok());
    REQUIRE(created.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000007"), {}).ok());

    const auto checkpoint = created.value()->checkpoint(
        command(expected, "30000000-0000-4000-8000-000000000008"),
        {},
        static_cast<myrd::host::HostVolumeBarrierKind>(0xFF));
    REQUIRE(!checkpoint.ok());
    REQUIRE(checkpoint.error.has_value());
    REQUIRE_EQ(
        checkpoint.error->code,
        static_cast<std::uint32_t>(
            myrd::host::HostCoreError::adapter_invariant));
    REQUIRE_EQ(factory->provider->checkpoint_calls.load(), 0U);

    const auto stopped = created.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000012"),
        {},
        {true, static_cast<myrd::host::HostVolumeBarrierKind>(0xFF)});
    REQUIRE(!stopped.ok());
    REQUIRE(stopped.error.has_value());
    REQUIRE_EQ(factory->provider->stop_calls.load(), 0U);
}

TEST_CASE(host_core_fails_closed_when_requested_stop_checkpoint_is_omitted) {
    const auto expected = binding("00112233-4455-4677-8899-aabbccddeeff");
    auto factory = std::make_shared<FakeProviderFactory>();
    auto created = myrd::host::HostCore::create(expected, factory);
    REQUIRE(created.ok());
    REQUIRE(created.value()->start(
        command(expected, "30000000-0000-4000-8000-000000000009"), {}).ok());

    myrd::host::HostStopOutcome malformed{};
    malformed.transport_stopped = true;
    malformed.store_closed = true;
    malformed.session_closed = true;
    malformed.writer_closed = true;
    factory->provider->next_stop_outcome = malformed;
    const auto stopped = created.value()->stop(
        command(expected, "30000000-0000-4000-8000-000000000010"),
        {},
        {true, myrd::host::HostVolumeBarrierKind::windows_volume_flushed});
    REQUIRE(!stopped.ok());
    REQUIRE(stopped.error.has_value());
    REQUIRE_EQ(
        stopped.error->code,
        static_cast<std::uint32_t>(
            myrd::host::HostCoreError::adapter_invariant));
}

TEST_CASE(host_m3_adapter_maps_all_provider_states_barriers_and_dispositions) {
    using myrd::host::HostCommitDisposition;
    using myrd::host::HostProviderState;
    using myrd::host::HostVolumeBarrierKind;
    using myrd::m3::M3ProviderState;
    using myrd::persistence::CommitDisposition;
    using myrd::persistence::VolumeBarrierKind;

    const std::array states{
        std::pair{M3ProviderState::Constructed, HostProviderState::constructed},
        std::pair{M3ProviderState::Starting, HostProviderState::starting},
        std::pair{M3ProviderState::Ready, HostProviderState::ready},
        std::pair{M3ProviderState::Stopping, HostProviderState::stopping},
        std::pair{M3ProviderState::Stopped, HostProviderState::stopped},
        std::pair{M3ProviderState::Failed, HostProviderState::failed}};
    for (const auto& [source, expected] : states) {
        const auto mapped = myrd::host::m3_adapter::map_state(source);
        REQUIRE(mapped.ok());
        REQUIRE_EQ(mapped.value(), expected);
    }
    REQUIRE(!myrd::host::m3_adapter::map_state(
        static_cast<M3ProviderState>(0xFF)).ok());

    const std::array dispositions{
        std::pair{CommitDisposition::NotCommitted,
                  HostCommitDisposition::not_committed},
        std::pair{CommitDisposition::CommitIndeterminate,
                  HostCommitDisposition::commit_indeterminate},
        std::pair{CommitDisposition::Committed,
                  HostCommitDisposition::committed},
        std::pair{CommitDisposition::CommittedNeedsReconciliation,
                  HostCommitDisposition::committed_needs_reconciliation}};
    for (const auto& [source, expected] : dispositions) {
        const auto mapped = myrd::host::m3_adapter::map_disposition(source);
        REQUIRE(mapped.ok());
        REQUIRE_EQ(mapped.value(), expected);
    }
    REQUIRE(!myrd::host::m3_adapter::map_disposition(
        static_cast<CommitDisposition>(0xFF)).ok());

    const auto production = myrd::host::m3_adapter::map_barrier(
        VolumeBarrierKind::WindowsVolumeFlushed);
    REQUIRE(production.ok());
    REQUIRE_EQ(production.value(), HostVolumeBarrierKind::windows_volume_flushed);
    const auto test = myrd::host::m3_adapter::map_barrier(
        VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(test.ok());
    REQUIRE_EQ(
        test.value(),
        HostVolumeBarrierKind::raw_block_test_no_filesystem_cache);
    REQUIRE(!myrd::host::m3_adapter::map_barrier(
        static_cast<VolumeBarrierKind>(0xFF)).ok());

    const auto reverse_production = myrd::host::m3_adapter::map_barrier(
        HostVolumeBarrierKind::windows_volume_flushed);
    REQUIRE(reverse_production.ok());
    REQUIRE_EQ(
        reverse_production.value(), VolumeBarrierKind::WindowsVolumeFlushed);
    const auto reverse_test = myrd::host::m3_adapter::map_barrier(
        HostVolumeBarrierKind::raw_block_test_no_filesystem_cache);
    REQUIRE(reverse_test.ok());
    REQUIRE_EQ(
        reverse_test.value(),
        VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    REQUIRE(!myrd::host::m3_adapter::map_barrier(
        static_cast<HostVolumeBarrierKind>(0xFF)).ok());
}

TEST_CASE(host_m3_adapter_preserves_checkpoint_error_and_complete_context) {
    myrd::persistence::ImageUuid image{};
    for (std::size_t i = 0; i < image.bytes.size(); ++i) {
        image.bytes[i] = static_cast<std::byte>(i + 1);
    }
    const std::array dispositions{
        myrd::persistence::CommitDisposition::NotCommitted,
        myrd::persistence::CommitDisposition::CommitIndeterminate,
        myrd::persistence::CommitDisposition::Committed,
        myrd::persistence::CommitDisposition::CommittedNeedsReconciliation};
    for (const auto disposition : dispositions) {
        myrd::m3::M3CheckpointOutcome source{disposition, 8};
        myrd::persistence::PersistenceErrorContext exact_context{};
        myrd::Error exact_error{
            myrd::ErrorDomain::persistence,
            0,
            L"checkpoint.publish",
            L"injected publication failure",
            112};
        if (disposition != myrd::persistence::CommitDisposition::Committed) {
            exact_context.operation = "checkpoint.publish";
            exact_context.code = disposition ==
                    myrd::persistence::CommitDisposition::CommitIndeterminate
                ? myrd::persistence::PersistenceErrorCode::CommitIndeterminate
                : myrd::persistence::PersistenceErrorCode::DiskFull;
            exact_context.phase =
                myrd::persistence::SessionPhase::Checkpointing;
            exact_context.native_code = 112;
            exact_context.operation_id = 3909;
            exact_context.image_uuid = image;
            exact_context.generation = 8;
            exact_context.disposition = disposition;
            exact_context.safe_context =
                "bundle volume full during publication";
            exact_error.code = static_cast<std::uint32_t>(exact_context.code);
            source.error = exact_error;
            source.error_context = exact_context;
        }
        const auto mapped =
            myrd::host::m3_adapter::map_checkpoint_outcome(source);
        REQUIRE(mapped.ok());
        const auto& value = mapped.value();
        REQUIRE_EQ(value.generation, 8ULL);
        if (disposition == myrd::persistence::CommitDisposition::Committed) {
            REQUIRE(value.ok());
        } else {
            REQUIRE(value.error.has_value());
            REQUIRE_EQ(value.error->domain, exact_error.domain);
            REQUIRE_EQ(value.error->code, exact_error.code);
            REQUIRE_EQ(value.error->operation, exact_error.operation);
            REQUIRE_EQ(value.error->message, exact_error.message);
            REQUIRE_EQ(value.error->native_code, exact_error.native_code);
            REQUIRE(value.error_context.has_value());
            REQUIRE_EQ(
                value.error_context->operation, exact_context.operation);
            REQUIRE_EQ(
                value.error_context->code,
                static_cast<std::uint32_t>(exact_context.code));
            REQUIRE_EQ(
                value.error_context->phase,
                static_cast<std::uint8_t>(exact_context.phase));
            REQUIRE_EQ(
                value.error_context->native_code, exact_context.native_code);
            REQUIRE_EQ(
                value.error_context->operation_id, exact_context.operation_id);
            REQUIRE_EQ(
                value.error_context->image_uuid,
                exact_context.image_uuid.bytes);
            REQUIRE_EQ(
                value.error_context->generation, exact_context.generation);
            REQUIRE_EQ(
                value.error_context->safe_context,
                exact_context.safe_context);
            REQUIRE(!value.ok());
        }
    }

    const myrd::m3::M3CheckpointOutcome clean{
        myrd::persistence::CommitDisposition::Committed, 9};
    const auto mapped_clean =
        myrd::host::m3_adapter::map_checkpoint_outcome(clean);
    REQUIRE(mapped_clean.ok());
    REQUIRE(mapped_clean.value().ok());

    auto invalid = clean;
    invalid.disposition =
        static_cast<myrd::persistence::CommitDisposition>(0xFF);
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(invalid).ok());

    myrd::persistence::PersistenceErrorContext malformed{};
    malformed.operation = "checkpoint.publish";
    malformed.code = myrd::persistence::PersistenceErrorCode::DiskFull;
    malformed.phase = myrd::persistence::SessionPhase::Checkpointing;
    malformed.native_code = 112;
    malformed.operation_id = 3909;
    malformed.image_uuid = image;
    malformed.generation = 8;
    malformed.disposition =
        myrd::persistence::CommitDisposition::NotCommitted;
    const myrd::Error matching_error{
        myrd::ErrorDomain::persistence,
        static_cast<std::uint32_t>(malformed.code),
        L"checkpoint.publish",
        L"failure",
        112};
    myrd::m3::M3CheckpointOutcome contradictory{
        myrd::persistence::CommitDisposition::NotCommitted,
        8,
        matching_error,
        malformed};
    contradictory.error_context.reset();
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(
        contradictory).ok());
    contradictory.error_context = malformed;
    contradictory.error->code++;
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(
        contradictory).ok());
    contradictory.error = matching_error;
    contradictory.error_context->phase =
        static_cast<myrd::persistence::SessionPhase>(0xFF);
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(
        contradictory).ok());
    contradictory.error_context = malformed;
    contradictory.error_context->code =
        static_cast<myrd::persistence::PersistenceErrorCode>(0xFFFF);
    contradictory.error->code = static_cast<std::uint32_t>(
        contradictory.error_context->code);
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(
        contradictory).ok());
    contradictory.error_context = malformed;
    contradictory.error_context->generation++;
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(
        contradictory).ok());
    contradictory.error_context = malformed;
    contradictory.error_context->operation_id = 0;
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(
        contradictory).ok());
    contradictory = clean;
    contradictory.error = matching_error;
    contradictory.error_context = malformed;
    REQUIRE(!myrd::host::m3_adapter::map_checkpoint_outcome(
        contradictory).ok());
}

TEST_CASE(host_m3_adapter_preserves_every_stop_completion_field) {
    const myrd::Error error{
        myrd::ErrorDomain::protocol,
        0xCAFE,
        L"FakeTransport::stop",
        L"injected writer close failure",
        5};
    myrd::persistence::PersistenceErrorContext context{};
    context.operation = "stop.writer";
    context.code = myrd::persistence::PersistenceErrorCode::AccessDenied;
    context.phase = myrd::persistence::SessionPhase::Closing;
    context.native_code = 5;
    context.operation_id = 99;
    context.image_uuid.bytes[0] = std::byte{1};
    context.generation = 12;
    context.disposition =
        myrd::persistence::CommitDisposition::CommittedNeedsReconciliation;
    context.safe_context = "writer close failed";
    const myrd::m3::M3StopOutcome source{
        myrd::persistence::CommitDisposition::
            CommittedNeedsReconciliation,
        12,
        true,
        false,
        false,
        true,
        false,
        error,
        context};

    const auto mapped = myrd::host::m3_adapter::map_stop_outcome(source);
    REQUIRE(mapped.ok());
    const auto& value = mapped.value();
    REQUIRE_EQ(
        value.disposition,
        myrd::host::HostCommitDisposition::
            committed_needs_reconciliation);
    REQUIRE_EQ(value.generation, 12ULL);
    REQUIRE(value.transport_stopped);
    REQUIRE(!value.checkpoint_completed);
    REQUIRE(!value.store_closed);
    REQUIRE(value.session_closed);
    REQUIRE(!value.writer_closed);
    REQUIRE(value.error.has_value());
    REQUIRE(value.error_context.has_value());
    REQUIRE_EQ(value.error->domain, myrd::ErrorDomain::protocol);
    REQUIRE_EQ(value.error->code, 0xCAFEU);
    REQUIRE_EQ(value.error->operation, L"FakeTransport::stop");
    REQUIRE_EQ(value.error->native_code, 5U);
    REQUIRE_EQ(value.error_context->safe_context, context.safe_context);
    REQUIRE(!value.ok());
}

TEST_CASE(host_m3_adapter_copies_all_geometry_and_storage_statistics) {
    const myrd::persistence::ImageGeometry geometry{8ULL << 30, 512, 4096};
    const auto mapped_geometry =
        myrd::host::m3_adapter::map_geometry(geometry);
    REQUIRE(mapped_geometry.ok());
    REQUIRE_EQ(
        mapped_geometry.value(),
        (myrd::host::HostGeometry{8ULL << 30, 512, 4096}));

    myrd::DynamicBlockStoreStats source{};
    std::uint64_t next = 1;
    source.logical_size_bytes = next++;
    source.reserved_bytes = next++;
    source.committed_bytes = next++;
    source.resident_clean_blocks = next++;
    source.resident_dirty_blocks = next++;
    source.zero_clean_blocks = next++;
    source.zero_dirty_blocks = next++;
    source.loading_blocks = next++;
    source.evicted_blocks_total = next++;
    source.backing_reads_total = next++;
    source.backing_read_bytes_total = next++;
    source.uncached_reads_total = next++;
    source.unmap_bytes_total = next++;
    source.zero_bytes_total = next++;
    source.hard_limit_rejections = next++;
    source.commit_failures = next++;
    source.decommit_failures = next++;
    source.synchronous_pressure_passes_total = next++;
    source.synchronous_pressure_waits_total = next++;
    source.synchronous_pressure_no_progress_total = next++;
    source.synchronous_pressure_cancellations_total = next++;
    source.synchronous_pressure_deadlines_total = next++;
    source.commit_reservation_rollbacks_total = next++;
    source.snapshot_records_total = next++;
    source.snapshot_version_conflicts = next++;
    source.backing_generation = next++;
    source.backing_view_installs_total = next++;
    source.backing_generation_conflicts = next++;
    source.stale_backing_load_retries = next++;
    source.snapshot_active_id = next++;
    source.snapshot_active_state =
        myrd::ConsistentSnapshotState::FullyScanned;
    source.snapshot_cow_current_bytes = next++;
    source.snapshot_cow_peak_bytes = next++;
    source.snapshot_cow_records = next++;
    source.snapshot_blocked_writers = next++;
    source.snapshot_writer_stalls_total = next++;
    source.snapshot_auto_aborts_total = next++;
    source.snapshot_consistent_batches_total = next++;
    source.snapshot_consistent_records_total = next++;

    myrd::host::HostStorageStats expected{};
    next = 1;
    expected.logical_size_bytes = next++;
    expected.reserved_bytes = next++;
    expected.committed_bytes = next++;
    expected.resident_clean_blocks = next++;
    expected.resident_dirty_blocks = next++;
    expected.zero_clean_blocks = next++;
    expected.zero_dirty_blocks = next++;
    expected.loading_blocks = next++;
    expected.evicted_blocks_total = next++;
    expected.backing_reads_total = next++;
    expected.backing_read_bytes_total = next++;
    expected.uncached_reads_total = next++;
    expected.unmap_bytes_total = next++;
    expected.zero_bytes_total = next++;
    expected.hard_limit_rejections = next++;
    expected.commit_failures = next++;
    expected.decommit_failures = next++;
    expected.synchronous_pressure_passes_total = next++;
    expected.synchronous_pressure_waits_total = next++;
    expected.synchronous_pressure_no_progress_total = next++;
    expected.synchronous_pressure_cancellations_total = next++;
    expected.synchronous_pressure_deadlines_total = next++;
    expected.commit_reservation_rollbacks_total = next++;
    expected.snapshot_records_total = next++;
    expected.snapshot_version_conflicts = next++;
    expected.backing_generation = next++;
    expected.backing_view_installs_total = next++;
    expected.backing_generation_conflicts = next++;
    expected.stale_backing_load_retries = next++;
    expected.snapshot_active_id = next++;
    expected.snapshot_active_state =
        myrd::host::HostSnapshotState::fully_scanned;
    expected.snapshot_cow_current_bytes = next++;
    expected.snapshot_cow_peak_bytes = next++;
    expected.snapshot_cow_records = next++;
    expected.snapshot_blocked_writers = next++;
    expected.snapshot_writer_stalls_total = next++;
    expected.snapshot_auto_aborts_total = next++;
    expected.snapshot_consistent_batches_total = next++;
    expected.snapshot_consistent_records_total = next++;

    const auto mapped = myrd::host::m3_adapter::map_storage_stats(source);
    REQUIRE(mapped.ok());
    REQUIRE_EQ(mapped.value(), expected);

    const std::array snapshot_states{
        std::pair{myrd::ConsistentSnapshotState::Active,
                  myrd::host::HostSnapshotState::active},
        std::pair{myrd::ConsistentSnapshotState::Aborted,
                  myrd::host::HostSnapshotState::aborted},
        std::pair{myrd::ConsistentSnapshotState::FullyScanned,
                  myrd::host::HostSnapshotState::fully_scanned}};
    for (const auto& [source_state, expected_state] : snapshot_states) {
        source.snapshot_active_state = source_state;
        const auto state_mapped =
            myrd::host::m3_adapter::map_storage_stats(source);
        REQUIRE(state_mapped.ok());
        REQUIRE_EQ(
            state_mapped.value().snapshot_active_state,
            std::optional<myrd::host::HostSnapshotState>{expected_state});
    }

    source.snapshot_active_state =
        static_cast<myrd::ConsistentSnapshotState>(0xFF);
    REQUIRE(!myrd::host::m3_adapter::map_storage_stats(source).ok());
}
