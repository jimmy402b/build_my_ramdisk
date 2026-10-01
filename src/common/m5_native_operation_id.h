#pragma once

#include <cstdint>
#include <optional>

namespace myrd {

// The single definition of the M5 two-phase native operation id rule
// (design spec 3.3).
//
// M4's one-shot checkpoint writes `checkpoint_generation + 3902`.  M5 writes
// `candidate_generation + 3902`.  Those are different numbers for the same
// resulting image, because the candidate generation a prepare freezes is one
// past the generation M4 would have used as its base: M4's id is
// `candidate_generation + 3901`.  The two can therefore never be confused in
// the journal, which is exactly what the reconcile path depends on.
//
// Before Task 5 this constant existed only inside
// `tests/integration/m3_provider_integration_tests.cpp`.  The worker, the
// service's P4 record and the M3 reconcile all have to agree on it, so it is
// defined once here and included from everywhere rather than repeated.
inline constexpr std::uint64_t kM5CheckpointOperationOffset = 3902;

// `native_id = candidate_generation + kM5CheckpointOperationOffset`, modulo
// 2^64.  Zero is the "no operation" encoding and cannot identify a prepare, so
// the single candidate generation that would produce it is rejected here
// instead of being silently aliased to "no operation" at the provider.
[[nodiscard]] constexpr std::optional<std::uint64_t> m5_native_operation_id(
    const std::uint64_t candidate_generation) noexcept {
    const std::uint64_t native_operation_id =
        candidate_generation + kM5CheckpointOperationOffset;
    if (native_operation_id == 0) return std::nullopt;
    return native_operation_id;
}

} // namespace myrd
