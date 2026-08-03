#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace myrd {

inline constexpr std::uint64_t kM2SequentialRegionBytes = 32ull << 20;
inline constexpr std::uint64_t kM2SequentialIoBytes = 1ull << 20;
inline constexpr std::uint64_t kM2SequentialOperationCount =
    kM2SequentialRegionBytes / kM2SequentialIoBytes;
inline constexpr std::uint64_t kM2SequentialWarmupOperationCount = 1;
inline constexpr std::uint64_t kM2RandomIoBytes = 4ull << 10;
inline constexpr std::uint64_t kM2RandomOperationCount = 10'000;
inline constexpr std::uint64_t kM2RandomWarmupOperationCount = 128;
inline constexpr std::uint64_t kM2PerformanceRandomSeed =
    0x4D32504552465144ull;
inline constexpr std::uint64_t kM2SequentialThresholdBasisPoints = 7'000;
inline constexpr std::uint64_t kM2RandomThresholdBasisPoints = 6'000;
inline constexpr std::string_view kM2PerformanceMethodologyId =
    "m2-raw-qd1-v1";

enum class M2RawOperation : std::uint8_t {
    Read = 0,
    Write = 1,
};

constexpr std::string_view to_string(M2RawOperation operation) noexcept {
    switch (operation) {
    case M2RawOperation::Read:
        return "read";
    case M2RawOperation::Write:
        return "write";
    }
    return "unknown";
}

struct M2PerformancePolicy {
    std::string machine_fingerprint;
    std::string build_fingerprint;
    std::string process_policy;
    std::string raw_io_flags;

    bool operator==(const M2PerformancePolicy&) const = default;
};

class IM2RawIo {
public:
    virtual ~IM2RawIo() = default;
    virtual std::uint64_t size_bytes() const noexcept = 0;
    virtual Result<std::size_t> transfer(
        const OperationContext& context,
        M2RawOperation operation,
        std::uint64_t offset,
        std::span<std::byte> buffer) = 0;
};

class IM2PerformanceClock {
public:
    virtual ~IM2PerformanceClock() = default;
    virtual std::uint64_t frequency_hz() const noexcept = 0;
    virtual Result<std::uint64_t> now_ticks() = 0;
};

struct M2ProbeConfig {
    std::uint64_t region_offset{};
    M2RawOperation operation{M2RawOperation::Write};
    M2PerformancePolicy policy;
};

struct M2SequentialPerformanceSample {
    std::uint64_t bytes{};
    std::uint64_t operations{};
    std::uint64_t elapsed_ticks{};
    std::uint64_t clock_frequency_hz{};
    std::uint64_t bytes_per_second{};
};

struct M2RandomPerformanceSample {
    std::uint64_t bytes{};
    std::uint64_t operations{};
    std::uint64_t elapsed_ticks{};
    std::uint64_t clock_frequency_hz{};
    std::uint64_t operations_per_second{};
};

struct M2PerformanceProbeResult {
    std::string methodology_id{std::string(kM2PerformanceMethodologyId)};
    M2RawOperation operation{M2RawOperation::Write};
    std::uint32_t queue_depth{1};
    std::uint64_t region_offset{};
    std::uint64_t region_bytes{kM2SequentialRegionBytes};
    std::uint64_t sequential_io_bytes{kM2SequentialIoBytes};
    std::uint64_t random_io_bytes{kM2RandomIoBytes};
    std::uint64_t random_seed{kM2PerformanceRandomSeed};
    std::uint64_t sequential_warmup_operations{
        kM2SequentialWarmupOperationCount};
    std::uint64_t random_warmup_operations{kM2RandomWarmupOperationCount};
    M2PerformancePolicy policy;
    M2SequentialPerformanceSample sequential;
    M2RandomPerformanceSample random;
};

struct M2PerformanceComparison {
    std::uint64_t sequential_ratio_basis_points{};
    std::uint64_t random_ratio_basis_points{};
    std::uint64_t sequential_threshold_basis_points{
        kM2SequentialThresholdBasisPoints};
    std::uint64_t random_threshold_basis_points{
        kM2RandomThresholdBasisPoints};
    bool sequential_pass{};
    bool random_pass{};
    bool overall_pass{};
};

Result<M2PerformanceProbeResult> run_m2_performance_probe(
    IM2RawIo& io,
    IM2PerformanceClock& clock,
    const OperationContext& context,
    const M2ProbeConfig& config);

Result<M2PerformanceComparison> compare_m2_performance(
    const M2PerformanceProbeResult& fixed_baseline,
    const M2PerformanceProbeResult& dynamic_sample);

} // namespace myrd
