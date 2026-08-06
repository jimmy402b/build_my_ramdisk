#include "compaction_coordinator.h"

namespace myrd::persistence {
namespace {

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
