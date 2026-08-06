#include "test_framework.h"

#include "src/persistence/image_session.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace myrd::persistence {
namespace {

constexpr std::array<SessionPhase, 9> kPhases{
    SessionPhase::Opening,
    SessionPhase::Ready,
    SessionPhase::Checkpointing,
    SessionPhase::Compacting,
    SessionPhase::ResolvingCommit,
    SessionPhase::Reconciling,
    SessionPhase::Closing,
    SessionPhase::Closed,
    SessionPhase::PersistenceFaulted,
};

// This is the normative Task 29 table. The extra Verifying phase is a
// read-only phase already defined by Task 13 and is tested separately below.
constexpr std::array<std::array<bool, 9>, 9> kAllowed{{
    //                         Open Ready Check Compact Resolve Recon Close Closed Fault
    /* Opening            */ {false, true, false, false, false, false, true, true, false},
    /* Ready              */ {false, false, true, true, false, false, true, false, true},
    /* Checkpointing      */ {false, true, false, false, true, true, false, false, true},
    /* Compacting         */ {false, true, false, false, true, true, false, false, true},
    /* ResolvingCommit    */ {false, true, false, false, false, true, true, false, true},
    /* Reconciling        */ {false, true, false, false, false, false, true, false, true},
    /* Closing            */ {false, false, false, false, false, false, false, true, true},
    /* Closed             */ {true, false, false, false, false, false, false, false, false},
    /* PersistenceFaulted */ {false, false, false, false, false, false, true, false, false},
}};

PersistenceFailure sample_failure(PersistenceErrorCode code,
                                  SessionPhase phase,
                                  std::uint64_t operation_id) {
    PersistenceFailure failure{};
    failure.common_error = {
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"checkpoint",
        L"test persistence failure",
        5};
    failure.context.operation = "checkpoint";
    failure.context.code = code;
    failure.context.phase = phase;
    failure.context.operation_id = operation_id;
    failure.context.generation = 1;
    failure.context.image_uuid.bytes[0] = std::byte{0x42};
    failure.context.disposition =
        code == PersistenceErrorCode::CommitIndeterminate
            ? CommitDisposition::CommitIndeterminate
            : CommitDisposition::NotCommitted;
    return failure;
}

TEST_CASE(image_session_exposes_exhaustive_transition_table) {
    for (std::size_t from_index = 0; from_index < kPhases.size(); ++from_index) {
        for (std::size_t to_index = 0; to_index < kPhases.size(); ++to_index) {
            const auto from = kPhases[from_index];
            const auto to = kPhases[to_index];
            REQUIRE_EQ(ImageSession::is_valid_transition(from, to),
                       kAllowed[from_index][to_index]);

        }
    }
}

TEST_CASE(image_session_verifying_is_read_only_and_returns_to_ready) {
    ImageSession session(SessionPhase::Verifying);
    auto read = session.begin_immutable_read();
    REQUIRE(read.ok());
    REQUIRE(!session.begin_checkpoint().ok());
    read.value().release();
}

TEST_CASE(image_session_serializes_publish_affecting_operations) {
    ImageSession session(SessionPhase::Ready);
    auto checkpoint = session.begin_checkpoint();
    REQUIRE(checkpoint.ok());
    auto compact = session.begin_compaction();
    REQUIRE(!compact.ok());
    REQUIRE_EQ(compact.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(compact.error().code,
               static_cast<std::uint32_t>(PersistenceErrorCode::OperationBusy));

    REQUIRE(checkpoint.value().finish(CommitDisposition::NotCommitted, false).ok());
    auto compact_after = session.begin_compaction();
    REQUIRE(compact_after.ok());
    auto committed = compact_after.value().finish(CommitDisposition::Committed, true);
    REQUIRE(committed.ok());
    REQUIRE(committed.value().durable_success);
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
}

TEST_CASE(image_session_concurrent_publish_race_has_exactly_one_winner) {
    ImageSession session(SessionPhase::Ready);
    constexpr std::size_t thread_count = 32;
    std::atomic<std::size_t> attempted{0};
    std::atomic<std::size_t> acquired{0};
    std::atomic<std::size_t> busy{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&] {
            auto operation = session.begin_checkpoint();
            attempted.fetch_add(1, std::memory_order_acq_rel);
            if (!operation.ok()) {
                if (operation.error().code == static_cast<std::uint32_t>(
                        PersistenceErrorCode::OperationBusy)) {
                    busy.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            acquired.fetch_add(1, std::memory_order_relaxed);
            while (attempted.load(std::memory_order_acquire) != thread_count) {
                std::this_thread::yield();
            }
            REQUIRE(operation.value()
                        .finish(CommitDisposition::NotCommitted, false)
                        .ok());
        });
    }
    for (auto& thread : threads) thread.join();
    REQUIRE_EQ(acquired.load(), std::size_t{1});
    REQUIRE_EQ(busy.load(), thread_count - 1);
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
}

TEST_CASE(image_session_allows_immutable_reads_during_publish_operation) {
    ImageSession session(SessionPhase::Ready);
    auto operation = session.begin_checkpoint();
    REQUIRE(operation.ok());
    auto first = session.begin_immutable_read();
    auto second = session.begin_immutable_read();
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE_EQ(session.snapshot().active_immutable_reads, std::size_t{2});
    second.value().release();
    first.value().release();
    REQUIRE(operation.value().finish(CommitDisposition::NotCommitted, false).ok());
}

TEST_CASE(image_session_indeterminate_preserves_first_failure_and_blocks_mutation) {
    ImageSession session(SessionPhase::Ready);
    auto operation = session.begin_checkpoint();
    REQUIRE(operation.ok());
    const auto first_failure = sample_failure(
        PersistenceErrorCode::CommitIndeterminate,
        SessionPhase::Checkpointing, 101);
    auto outcome = operation.value().finish(
        CommitDisposition::CommitIndeterminate, false, first_failure);
    REQUIRE(outcome.ok());
    REQUIRE_EQ(session.phase(), SessionPhase::ResolvingCommit);
    REQUIRE_EQ(session.snapshot().disposition,
               CommitDisposition::CommitIndeterminate);
    REQUIRE(session.snapshot().first_failure.has_value());
    REQUIRE_EQ(session.snapshot().first_failure->context.operation_id, 101ULL);

    auto blocked = session.begin_checkpoint();
    REQUIRE(!blocked.ok());
    REQUIRE_EQ(blocked.error().code,
               static_cast<std::uint32_t>(PersistenceErrorCode::CommitIndeterminate));

    auto resolver = session.begin_commit_resolution();
    REQUIRE(resolver.ok());
    const auto later_failure = sample_failure(
        PersistenceErrorCode::IoError, SessionPhase::ResolvingCommit, 202);
    REQUIRE(resolver.value().finish(
                CommitDisposition::CommitIndeterminate, false, later_failure)
                .ok());
    REQUIRE_EQ(session.phase(), SessionPhase::PersistenceFaulted);
    REQUIRE_EQ(session.snapshot().first_failure->context.operation_id, 101ULL);
}

TEST_CASE(image_session_post_commit_enters_reconciling_without_losing_disposition) {
    ImageSession session(SessionPhase::Ready);
    auto operation = session.begin_checkpoint();
    REQUIRE(operation.ok());
    const auto failure = sample_failure(
        PersistenceErrorCode::IoError,
        SessionPhase::Checkpointing, 303);
    auto outcome = operation.value().finish(
        CommitDisposition::CommittedNeedsReconciliation, true, failure);
    REQUIRE(outcome.ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Reconciling);
    REQUIRE_EQ(session.snapshot().disposition,
               CommitDisposition::CommittedNeedsReconciliation);
    REQUIRE(session.snapshot().first_failure.has_value());
}

TEST_CASE(image_session_resolution_and_reconciliation_follow_durable_facts) {
    ImageSession proved_old(SessionPhase::Ready);
    auto old_checkpoint = proved_old.begin_checkpoint();
    REQUIRE(old_checkpoint.ok());
    REQUIRE(old_checkpoint.value()
                .finish(CommitDisposition::CommitIndeterminate, false,
                        sample_failure(PersistenceErrorCode::CommitIndeterminate,
                                       SessionPhase::Checkpointing, 401))
                .ok());
    auto old_resolver = proved_old.begin_commit_resolution();
    REQUIRE(old_resolver.ok());
    REQUIRE(old_resolver.value()
                .finish(CommitDisposition::NotCommitted, false)
                .ok());
    REQUIRE_EQ(proved_old.phase(), SessionPhase::Ready);
    REQUIRE_EQ(proved_old.snapshot().disposition,
               CommitDisposition::NotCommitted);
    REQUIRE_EQ(proved_old.snapshot().first_failure->context.operation_id, 401ULL);

    ImageSession proved_new(SessionPhase::Ready);
    auto new_checkpoint = proved_new.begin_checkpoint();
    REQUIRE(new_checkpoint.ok());
    REQUIRE(new_checkpoint.value()
                .finish(CommitDisposition::CommitIndeterminate, false,
                        sample_failure(PersistenceErrorCode::CommitIndeterminate,
                                       SessionPhase::Checkpointing, 402))
                .ok());
    auto new_resolver = proved_new.begin_commit_resolution();
    REQUIRE(new_resolver.ok());
    REQUIRE(new_resolver.value()
                .finish(CommitDisposition::CommittedNeedsReconciliation, true)
                .ok());
    REQUIRE_EQ(proved_new.phase(), SessionPhase::Reconciling);
    auto reconcile = proved_new.begin_reconciliation();
    REQUIRE(reconcile.ok());
    auto reconciled = reconcile.value().finish(CommitDisposition::Committed, true);
    REQUIRE(reconciled.ok());
    REQUIRE(reconciled.value().durable_success);
    REQUIRE_EQ(proved_new.phase(), SessionPhase::Ready);
    REQUIRE_EQ(proved_new.snapshot().first_failure->context.operation_id, 402ULL);
}

TEST_CASE(image_session_committed_requires_explicit_durable_proof) {
    ImageSession session(SessionPhase::Ready);
    auto operation = session.begin_checkpoint();
    REQUIRE(operation.ok());
    auto not_proven = operation.value().finish(CommitDisposition::Committed, false);
    REQUIRE(!not_proven.ok());
    REQUIRE_EQ(session.snapshot().disposition, CommitDisposition::NotCommitted);
    REQUIRE(operation.value().abort().ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
}

TEST_CASE(image_session_close_is_idempotent_and_drains_operation_and_reads) {
    ImageSession session(SessionPhase::Ready);
    auto operation = session.begin_checkpoint();
    REQUIRE(operation.ok());
    auto read = session.begin_immutable_read();
    REQUIRE(read.ok());

    std::atomic<bool> close_returned{false};
    std::optional<Result<SessionCloseOutcome>> close_result;
    std::thread closer([&] {
        close_result.emplace(session.close());
        close_returned.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (!session.snapshot().close_requested &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(session.snapshot().close_requested);
    REQUIRE(!close_returned.load(std::memory_order_acquire));
    REQUIRE(operation.value().stop_token().stop_requested());
    REQUIRE(!session.begin_compaction().ok());
    REQUIRE(!session.begin_immutable_read().ok());
    REQUIRE(operation.value().finish(CommitDisposition::NotCommitted, false).ok());
    read.value().release();
    closer.join();
    REQUIRE(close_returned.load(std::memory_order_acquire));
    REQUIRE(close_result.has_value());
    REQUIRE(close_result->ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Closed);

    auto second_close = session.close();
    REQUIRE(second_close.ok());
    REQUIRE_EQ(second_close.value().phase, SessionPhase::Closed);
    REQUIRE(!second_close.value().durable_success);
}

TEST_CASE(image_session_close_wait_observes_cancel_and_remains_sticky) {
    ImageSession session(SessionPhase::Ready);
    auto operation = session.begin_checkpoint();
    REQUIRE(operation.ok());

    std::stop_source source;
    source.request_stop();
    OperationContext context{};
    context.stop_token = source.get_token();
    auto cancelled = session.close(context);
    REQUIRE(!cancelled.ok());
    REQUIRE_EQ(cancelled.error().code,
               static_cast<std::uint32_t>(PersistenceErrorCode::Cancelled));
    REQUIRE(session.snapshot().close_requested);
    REQUIRE(operation.value().stop_token().stop_requested());
    REQUIRE(!session.begin_compaction().ok());
    REQUIRE(!session.begin_immutable_read().ok());

    REQUIRE(operation.value().abort().ok());
    REQUIRE(session.close().ok());
}

TEST_CASE(image_session_close_wait_observes_deadline) {
    ImageSession session(SessionPhase::Ready);
    auto read = session.begin_immutable_read();
    REQUIRE(read.ok());

    OperationContext context{};
    context.deadline = std::chrono::steady_clock::now();
    auto timed_out = session.close(context);
    REQUIRE(!timed_out.ok());
    REQUIRE_EQ(timed_out.error().code,
               static_cast<std::uint32_t>(PersistenceErrorCode::DeadlineExceeded));
    read.value().release();
    REQUIRE(session.close().ok());
}

} // namespace
} // namespace myrd::persistence
