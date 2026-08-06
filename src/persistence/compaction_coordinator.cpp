#include "compaction_coordinator.h"

namespace myrd::persistence {
namespace {

template <class T>
Result<T> compaction_failure(
    const PersistenceErrorCode code,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"CompactionCoordinator::begin",
        message,
        0});
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<void> run_phase_hook(
    const CompactionPhaseHook* hook,
    const CompactionPhase phase) {
    if (hook == nullptr || !*hook) return Result<void>::success();
    try {
        return (*hook)(phase);
    } catch (...) {
        return compaction_failure<void>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"compaction phase hook threw an exception");
    }
}
#endif

// Compare a/b with c/d exactly without cross multiplication. Each reciprocal
// step reverses ordering, and Euclidean reduction guarantees termination.
int compare_positive_fractions(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c,
    std::uint64_t d) noexcept {
    bool reversed = false;
    for (;;) {
        const auto lhs_quotient = a / b;
        const auto rhs_quotient = c / d;
        if (lhs_quotient != rhs_quotient) {
            const int comparison = lhs_quotient < rhs_quotient ? -1 : 1;
            return reversed ? -comparison : comparison;
        }

        const auto lhs_remainder = a % b;
        const auto rhs_remainder = c % d;
        if (lhs_remainder == 0 || rhs_remainder == 0) {
            int comparison = 0;
            if (lhs_remainder == 0 && rhs_remainder != 0) {
                comparison = -1;
            } else if (lhs_remainder != 0 && rhs_remainder == 0) {
                comparison = 1;
            }
            return reversed ? -comparison : comparison;
        }

        a = b;
        b = lhs_remainder;
        c = d;
        d = rhs_remainder;
        reversed = !reversed;
    }
}

}  // namespace

PreparedCompactionOperation::PreparedCompactionOperation(
    PublishOperationLease lease,
    BackingView captured_view) noexcept
    : lease_(std::move(lease)),
      captured_view_(std::move(captured_view)) {}

PreparedCompactionOperation::PreparedCompactionOperation(
    PreparedCompactionOperation&& other) noexcept
    : lease_(std::move(other.lease_)),
      captured_view_(std::move(other.captured_view_)) {
    other.lease_.reset();
    other.captured_view_ = {};
}

PreparedCompactionOperation& PreparedCompactionOperation::operator=(
    PreparedCompactionOperation&& other) noexcept {
    if (this != &other) {
        abandon_noexcept();
        lease_ = std::move(other.lease_);
        captured_view_ = std::move(other.captured_view_);
        other.lease_.reset();
        other.captured_view_ = {};
    }
    return *this;
}

PreparedCompactionOperation::~PreparedCompactionOperation() {
    abandon_noexcept();
}

bool PreparedCompactionOperation::active() const noexcept {
    return lease_.has_value() && lease_->active() &&
        captured_view_.store != nullptr;
}

Result<void> PreparedCompactionOperation::abort() {
    captured_view_ = {};
    if (!lease_.has_value()) return Result<void>::success();
    auto result = lease_->abort();
    lease_.reset();
    if (!result.ok()) return Result<void>::failure(result.error());
    return Result<void>::success();
}

void PreparedCompactionOperation::abandon_noexcept() noexcept {
    captured_view_ = {};
    if (lease_.has_value()) {
        (void)lease_->abort();
        lease_.reset();
    }
}

Result<PreparedCompactionOperation> CompactionCoordinator::begin(
    const OperationContext& context) {
    return begin_impl(context
#if defined(MYRD_ENABLE_TEST_SEAMS)
        , nullptr
#endif
    );
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<PreparedCompactionOperation>
CompactionCoordinator::begin_for_testing(
    const OperationContext& context,
    CompactionPhaseHook hook) {
    if (!hook) {
        return compaction_failure<PreparedCompactionOperation>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction test phase hook is empty");
    }
    return begin_impl(context, &hook);
}
#endif

Result<PreparedCompactionOperation> CompactionCoordinator::begin_impl(
    const OperationContext& context
#if defined(MYRD_ENABLE_TEST_SEAMS)
    , const CompactionPhaseHook* hook
#endif
) {
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<PreparedCompactionOperation>::failure(live.error());
    }
    auto begun = session_->begin_compaction();
    if (!begun.ok()) {
        return Result<PreparedCompactionOperation>::failure(begun.error());
    }
    auto lease = begun.move_value();

#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto phase = run_phase_hook(
        hook, CompactionPhase::OperationGateAcquired);
    if (!phase.ok()) {
        return Result<PreparedCompactionOperation>::failure(phase.error());
    }
#endif

    auto captured = store_->capture_backing_view();
    if (captured.store == nullptr ||
        captured.generation == kInvalidGeneration) {
        return compaction_failure<PreparedCompactionOperation>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction requires a valid immutable durable backing view");
    }

#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = run_phase_hook(
        hook, CompactionPhase::DurableViewCaptured);
    if (!phase.ok()) {
        return Result<PreparedCompactionOperation>::failure(phase.error());
    }
#endif

    live = check_operation_context(context);
    if (!live.ok()) {
        return Result<PreparedCompactionOperation>::failure(live.error());
    }
    return Result<PreparedCompactionOperation>::success(
        PreparedCompactionOperation{
            std::move(lease), std::move(captured)});
}

CompactionRecommendation should_compact(
    const CompactionMetrics& metrics,
    const CompactionPolicy& policy) noexcept {
    CompactionRecommendation result{};

    result.journal_physical_bytes =
        policy.journal_physical_bytes_at_or_above.has_value() &&
        metrics.journal_physical_bytes >=
            *policy.journal_physical_bytes_at_or_above;
    result.checkpoint_count = policy.checkpoints_at_or_above.has_value() &&
        metrics.checkpoints_since_compaction >= *policy.checkpoints_at_or_above;
    result.index_depth = policy.index_depth_at_or_above.has_value() &&
        metrics.index_depth >= *policy.index_depth_at_or_above;

    if (policy.journal_to_base_ratio_at_or_above.has_value()) {
        const auto threshold = *policy.journal_to_base_ratio_at_or_above;
        if (threshold.numerator == 0 || threshold.denominator == 0) {
            result.policy_valid = false;
        } else if (metrics.base_physical_bytes == 0) {
            // A non-empty journal over an empty base is an infinite ratio;
            // define 0/0 as no pressure rather than an automatic trigger.
            result.journal_to_base_ratio =
                metrics.journal_physical_bytes != 0;
        } else {
            result.journal_to_base_ratio = compare_positive_fractions(
                metrics.journal_physical_bytes,
                metrics.base_physical_bytes,
                threshold.numerator,
                threshold.denominator) >= 0;
        }
    }

    result.recommended = result.policy_valid &&
        (result.journal_physical_bytes || result.journal_to_base_ratio ||
         result.checkpoint_count || result.index_depth);
    return result;
}

}  // namespace myrd::persistence
