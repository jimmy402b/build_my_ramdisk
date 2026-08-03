#include "m2_performance_probe.h"

#include <algorithm>
#include <limits>
#include <new>
#include <vector>

namespace myrd {

namespace {

Result<M2PerformanceProbeResult> probe_error(
    std::uint32_t code,
    const wchar_t* operation,
    const wchar_t* message) {
    return Result<M2PerformanceProbeResult>::failure(
        {ErrorDomain::evidence, code, operation, message});
}

Result<M2PerformanceComparison> comparison_error(
    std::uint32_t code,
    const wchar_t* message) {
    return Result<M2PerformanceComparison>::failure(
        {ErrorDomain::evidence, code,
         L"compare_m2_performance", message});
}

Result<void> transfer_exact(
    IM2RawIo& io,
    const OperationContext& context,
    M2RawOperation operation,
    std::uint64_t offset,
    std::span<std::byte> buffer) {
    auto live = check_operation_context(context);
    if (!live) return live;
    auto transferred = io.transfer(context, operation, offset, buffer);
    if (!transferred) return Result<void>::failure(transferred.error());
    if (transferred.value() != buffer.size()) {
        return Result<void>::failure({
            ErrorDomain::evidence, 0xB101,
            L"run_m2_performance_probe(transfer_exact)",
            L"raw performance transfer completed short"});
    }
    return Result<void>::success();
}

Result<std::uint64_t> checked_rate(
    std::uint64_t units,
    std::uint64_t frequency,
    std::uint64_t elapsed_ticks,
    const wchar_t* operation) {
    if (frequency == 0 || elapsed_ticks == 0 ||
        units > (std::numeric_limits<std::uint64_t>::max)() / frequency) {
        return Result<std::uint64_t>::failure({
            ErrorDomain::evidence, 0xB102, operation,
            L"invalid or overflowing performance rate"});
    }
    return Result<std::uint64_t>::success(
        (units * frequency) / elapsed_ticks);
}

Result<std::uint64_t> elapsed(
    IM2PerformanceClock& clock,
    std::uint64_t start,
    const wchar_t* operation) {
    auto end = clock.now_ticks();
    if (!end) return Result<std::uint64_t>::failure(end.error());
    if (end.value() <= start) {
        return Result<std::uint64_t>::failure({
            ErrorDomain::evidence, 0xB103, operation,
            L"performance clock did not advance"});
    }
    return Result<std::uint64_t>::success(end.value() - start);
}

class FixedRandom final {
public:
    explicit FixedRandom(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ull;
        auto value = state_;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_;
};

Result<void> run_random_operations(
    IM2RawIo& io,
    const OperationContext& context,
    const M2ProbeConfig& config,
    std::span<std::byte> buffer,
    std::uint64_t count) {
    FixedRandom random(kM2PerformanceRandomSeed);
    constexpr auto slots =
        kM2SequentialRegionBytes / kM2RandomIoBytes;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto slot = random.next() % slots;
        auto transferred = transfer_exact(
            io, context, config.operation,
            config.region_offset + slot * kM2RandomIoBytes,
            buffer.first(static_cast<std::size_t>(kM2RandomIoBytes)));
        if (!transferred) return transferred;
    }
    return Result<void>::success();
}

Result<std::uint64_t> ratio_basis_points(
    std::uint64_t candidate,
    std::uint64_t baseline) {
    if (baseline == 0 ||
        candidate > (std::numeric_limits<std::uint64_t>::max)() / 10'000) {
        return Result<std::uint64_t>::failure({
            ErrorDomain::evidence, 0xB104,
            L"ratio_basis_points",
            L"zero baseline or overflowing performance ratio"});
    }
    return Result<std::uint64_t>::success(
        (candidate * 10'000) / baseline);
}

} // namespace

Result<M2PerformanceProbeResult> run_m2_performance_probe(
    IM2RawIo& io,
    IM2PerformanceClock& clock,
    const OperationContext& context,
    const M2ProbeConfig& config) {
    if ((config.region_offset % (64ull << 10)) != 0) {
        return probe_error(0xB105, L"run_m2_performance_probe",
                           L"performance region must be 64 KiB aligned");
    }
    if (config.policy.machine_fingerprint.empty() ||
        config.policy.build_fingerprint.empty() ||
        config.policy.process_policy.empty()) {
        return probe_error(0xB106, L"run_m2_performance_probe",
                           L"performance comparison policy is incomplete");
    }
    if (config.region_offset > io.size_bytes() ||
        kM2SequentialRegionBytes > io.size_bytes() - config.region_offset) {
        return probe_error(0xB107, L"run_m2_performance_probe",
                           L"performance region is outside the raw device");
    }
    if (clock.frequency_hz() == 0) {
        return probe_error(0xB108, L"run_m2_performance_probe",
                           L"performance clock frequency is zero");
    }

    try {
        std::vector<std::byte> buffer(
            static_cast<std::size_t>(kM2SequentialIoBytes));
        for (std::size_t index = 0; index < buffer.size(); ++index) {
            buffer[index] = std::byte{
                static_cast<unsigned char>((index * 131u + 17u) & 0xFFu)};
        }

        for (std::uint64_t index = 0;
             index < kM2SequentialWarmupOperationCount; ++index) {
            auto warmup = transfer_exact(
                io, context, config.operation, config.region_offset, buffer);
            if (!warmup) {
                return Result<M2PerformanceProbeResult>::failure(
                    warmup.error());
            }
        }

        auto sequential_start = clock.now_ticks();
        if (!sequential_start) {
            return Result<M2PerformanceProbeResult>::failure(
                sequential_start.error());
        }
        for (std::uint64_t index = 0;
             index < kM2SequentialOperationCount; ++index) {
            auto transferred = transfer_exact(
                io, context, config.operation,
                config.region_offset + index * kM2SequentialIoBytes,
                buffer);
            if (!transferred) {
                return Result<M2PerformanceProbeResult>::failure(
                    transferred.error());
            }
        }
        auto sequential_elapsed = elapsed(
            clock, sequential_start.value(),
            L"run_m2_performance_probe(sequential)");
        if (!sequential_elapsed) {
            return Result<M2PerformanceProbeResult>::failure(
                sequential_elapsed.error());
        }

        auto random_warmup = run_random_operations(
            io, context, config, buffer, kM2RandomWarmupOperationCount);
        if (!random_warmup) {
            return Result<M2PerformanceProbeResult>::failure(
                random_warmup.error());
        }

        auto random_start = clock.now_ticks();
        if (!random_start) {
            return Result<M2PerformanceProbeResult>::failure(
                random_start.error());
        }
        auto random_timed = run_random_operations(
            io, context, config, buffer, kM2RandomOperationCount);
        if (!random_timed) {
            return Result<M2PerformanceProbeResult>::failure(
                random_timed.error());
        }
        auto random_elapsed = elapsed(
            clock, random_start.value(),
            L"run_m2_performance_probe(random)");
        if (!random_elapsed) {
            return Result<M2PerformanceProbeResult>::failure(
                random_elapsed.error());
        }

        auto sequential_rate = checked_rate(
            kM2SequentialRegionBytes, clock.frequency_hz(),
            sequential_elapsed.value(),
            L"run_m2_performance_probe(sequential_rate)");
        if (!sequential_rate) {
            return Result<M2PerformanceProbeResult>::failure(
                sequential_rate.error());
        }
        auto random_rate = checked_rate(
            kM2RandomOperationCount, clock.frequency_hz(),
            random_elapsed.value(),
            L"run_m2_performance_probe(random_rate)");
        if (!random_rate) {
            return Result<M2PerformanceProbeResult>::failure(
                random_rate.error());
        }

        M2PerformanceProbeResult result;
        result.operation = config.operation;
        result.region_offset = config.region_offset;
        result.policy = config.policy;
        result.sequential = {
            kM2SequentialRegionBytes,
            kM2SequentialOperationCount,
            sequential_elapsed.value(),
            clock.frequency_hz(),
            sequential_rate.value(),
        };
        result.random = {
            kM2RandomIoBytes * kM2RandomOperationCount,
            kM2RandomOperationCount,
            random_elapsed.value(),
            clock.frequency_hz(),
            random_rate.value(),
        };
        return Result<M2PerformanceProbeResult>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return probe_error(0xB109, L"run_m2_performance_probe",
                           L"failed to allocate the reusable probe buffer");
    } catch (...) {
        return probe_error(0xB10A, L"run_m2_performance_probe",
                           L"unexpected performance probe exception");
    }
}

Result<M2PerformanceComparison> compare_m2_performance(
    const M2PerformanceProbeResult& fixed_baseline,
    const M2PerformanceProbeResult& dynamic_sample) {
    if (fixed_baseline.policy != dynamic_sample.policy) {
        return comparison_error(
            0xB110, L"machine/build/process policies do not match");
    }
    if (fixed_baseline.methodology_id != kM2PerformanceMethodologyId ||
        dynamic_sample.methodology_id != kM2PerformanceMethodologyId ||
        fixed_baseline.operation != dynamic_sample.operation ||
        fixed_baseline.queue_depth != 1 || dynamic_sample.queue_depth != 1) {
        return comparison_error(
            0xB111, L"performance methodologies are not comparable");
    }
    auto sequential_ratio = ratio_basis_points(
        dynamic_sample.sequential.bytes_per_second,
        fixed_baseline.sequential.bytes_per_second);
    if (!sequential_ratio) {
        return Result<M2PerformanceComparison>::failure(
            sequential_ratio.error());
    }
    auto random_ratio = ratio_basis_points(
        dynamic_sample.random.operations_per_second,
        fixed_baseline.random.operations_per_second);
    if (!random_ratio) {
        return Result<M2PerformanceComparison>::failure(random_ratio.error());
    }

    M2PerformanceComparison comparison;
    comparison.sequential_ratio_basis_points = sequential_ratio.value();
    comparison.random_ratio_basis_points = random_ratio.value();
    comparison.sequential_pass =
        sequential_ratio.value() >= kM2SequentialThresholdBasisPoints;
    comparison.random_pass =
        random_ratio.value() >= kM2RandomThresholdBasisPoints;
    comparison.overall_pass =
        comparison.sequential_pass && comparison.random_pass;
    return Result<M2PerformanceComparison>::success(comparison);
}

} // namespace myrd
