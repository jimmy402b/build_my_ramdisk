#include "test_framework.h"

#include "src/m2/m2_performance_probe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TransferRecord {
    myrd::M2RawOperation operation{};
    std::uint64_t offset{};
    std::size_t length{};
};

class FakeRawIo final : public myrd::IM2RawIo {
public:
    std::uint64_t size_bytes() const noexcept override {
        return 256ull << 20;
    }

    myrd::Result<std::size_t> transfer(
        const myrd::OperationContext& context,
        myrd::M2RawOperation operation,
        std::uint64_t offset,
        std::span<std::byte> buffer) override {
        auto live = myrd::check_operation_context(context);
        if (!live) {
            return myrd::Result<std::size_t>::failure(live.error());
        }
        records.push_back({operation, offset, buffer.size()});
        const auto call = records.size();
        if (cancel_source != nullptr && call == cancel_after_call) {
            cancel_source->request_stop();
        }
        if (fail_call != 0 && call == fail_call) {
            return myrd::Result<std::size_t>::failure({
                myrd::ErrorDomain::storage, 0xB001,
                L"FakeRawIo::transfer", L"injected transfer failure"});
        }
        if (short_call != 0 && call == short_call) {
            return myrd::Result<std::size_t>::success(
                buffer.empty() ? 0 : buffer.size() - 1);
        }
        return myrd::Result<std::size_t>::success(buffer.size());
    }

    std::vector<TransferRecord> records;
    std::size_t fail_call{};
    std::size_t short_call{};
    std::stop_source* cancel_source{};
    std::size_t cancel_after_call{};
};

class ScriptClock final : public myrd::IM2PerformanceClock {
public:
    explicit ScriptClock(std::vector<std::uint64_t> values)
        : values_(std::move(values)) {}

    std::uint64_t frequency_hz() const noexcept override {
        return 1'000'000;
    }

    myrd::Result<std::uint64_t> now_ticks() override {
        if (next_ >= values_.size()) {
            return myrd::Result<std::uint64_t>::failure({
                myrd::ErrorDomain::internal, 0xB002,
                L"ScriptClock::now_ticks", L"clock script exhausted"});
        }
        return myrd::Result<std::uint64_t>::success(values_[next_++]);
    }

    std::size_t calls() const noexcept { return next_; }

private:
    std::vector<std::uint64_t> values_;
    std::size_t next_{};
};

myrd::M2PerformancePolicy policy(std::string suffix = {}) {
    return {"machine" + suffix, "build" + suffix,
            "single-thread-normal-priority" + suffix};
}

myrd::M2ProbeConfig config() {
    myrd::M2ProbeConfig value;
    value.region_offset = 64ull << 20;
    value.operation = myrd::M2RawOperation::Write;
    value.policy = policy();
    return value;
}

myrd::M2PerformanceProbeResult sample(
    std::uint64_t sequential_rate,
    std::uint64_t random_rate,
    myrd::M2PerformancePolicy value_policy = policy()) {
    myrd::M2PerformanceProbeResult value;
    value.policy = std::move(value_policy);
    value.operation = myrd::M2RawOperation::Write;
    value.queue_depth = 1;
    value.sequential.bytes_per_second = sequential_rate;
    value.random.operations_per_second = random_rate;
    value.methodology_id = std::string(myrd::kM2PerformanceMethodologyId);
    return value;
}

} // namespace

TEST_CASE(m2_performance_uses_exact_qd1_methodology_and_excludes_warmup) {
    FakeRawIo io;
    ScriptClock clock({100, 100 + 1'000'000, 500, 500 + 2'000'000});
    myrd::OperationContext context{};

    auto result = myrd::run_m2_performance_probe(
        io, clock, context, config());

    REQUIRE(result.ok());
    REQUIRE_EQ(clock.calls(), std::size_t{4});
    REQUIRE_EQ(result.value().methodology_id,
               std::string(myrd::kM2PerformanceMethodologyId));
    REQUIRE_EQ(result.value().operation, myrd::M2RawOperation::Write);
    REQUIRE_EQ(result.value().queue_depth, std::uint32_t{1});
    REQUIRE_EQ(result.value().sequential.bytes,
               myrd::kM2SequentialRegionBytes);
    REQUIRE_EQ(result.value().sequential.operations,
               myrd::kM2SequentialOperationCount);
    REQUIRE_EQ(result.value().random.bytes,
               myrd::kM2RandomIoBytes * myrd::kM2RandomOperationCount);
    REQUIRE_EQ(result.value().random.operations,
               myrd::kM2RandomOperationCount);
    REQUIRE_EQ(result.value().sequential.elapsed_ticks, 1'000'000ull);
    REQUIRE_EQ(result.value().random.elapsed_ticks, 2'000'000ull);
    REQUIRE_EQ(result.value().sequential.bytes_per_second,
               myrd::kM2SequentialRegionBytes);
    REQUIRE_EQ(result.value().random.operations_per_second, 5'000ull);

    const auto expected_calls = myrd::kM2SequentialWarmupOperationCount +
        myrd::kM2SequentialOperationCount +
        myrd::kM2RandomWarmupOperationCount +
        myrd::kM2RandomOperationCount;
    REQUIRE_EQ(io.records.size(),
               static_cast<std::size_t>(expected_calls));

    for (const auto& record : io.records) {
        REQUIRE_EQ(record.operation, myrd::M2RawOperation::Write);
    }

    const auto sequential_begin = static_cast<std::size_t>(
        myrd::kM2SequentialWarmupOperationCount);
    for (std::uint64_t index = 0;
         index < myrd::kM2SequentialOperationCount; ++index) {
        const auto& record = io.records[
            sequential_begin + static_cast<std::size_t>(index)];
        REQUIRE_EQ(record.offset,
                   config().region_offset +
                       index * myrd::kM2SequentialIoBytes);
        REQUIRE_EQ(record.length,
                   static_cast<std::size_t>(myrd::kM2SequentialIoBytes));
    }

    const auto random_begin = sequential_begin +
        static_cast<std::size_t>(myrd::kM2SequentialOperationCount) +
        static_cast<std::size_t>(myrd::kM2RandomWarmupOperationCount);
    for (std::size_t index = random_begin; index < io.records.size(); ++index) {
        const auto& record = io.records[index];
        REQUIRE_EQ(record.length,
                   static_cast<std::size_t>(myrd::kM2RandomIoBytes));
        REQUIRE_EQ(record.offset % myrd::kM2RandomIoBytes, 0ull);
        REQUIRE(record.offset >= config().region_offset);
        REQUIRE(record.offset + record.length <=
                config().region_offset + myrd::kM2SequentialRegionBytes);
    }
}

TEST_CASE(m2_performance_random_offsets_are_fixed_seed_reproducible) {
    FakeRawIo first_io;
    FakeRawIo second_io;
    ScriptClock first_clock({1, 2, 3, 4});
    ScriptClock second_clock({11, 12, 13, 14});
    myrd::OperationContext context{};

    REQUIRE(myrd::run_m2_performance_probe(
                first_io, first_clock, context, config()).ok());
    REQUIRE(myrd::run_m2_performance_probe(
                second_io, second_clock, context, config()).ok());
    REQUIRE_EQ(first_io.records.size(), second_io.records.size());
    for (std::size_t index = 0; index < first_io.records.size(); ++index) {
        REQUIRE_EQ(first_io.records[index].offset,
                   second_io.records[index].offset);
        REQUIRE_EQ(first_io.records[index].length,
                   second_io.records[index].length);
    }
}

TEST_CASE(m2_performance_rejects_short_io_and_transport_error) {
    myrd::OperationContext context{};

    FakeRawIo short_io;
    short_io.short_call = 2;
    ScriptClock short_clock({1, 2, 3, 4});
    auto short_result = myrd::run_m2_performance_probe(
        short_io, short_clock, context, config());
    REQUIRE(short_result.has_error());

    FakeRawIo failed_io;
    failed_io.fail_call = 2;
    ScriptClock failed_clock({1, 2, 3, 4});
    auto failed_result = myrd::run_m2_performance_probe(
        failed_io, failed_clock, context, config());
    REQUIRE(failed_result.has_error());
}

TEST_CASE(m2_performance_rejects_invalid_geometry_and_clock_samples) {
    myrd::OperationContext context{};
    FakeRawIo io;

    auto misaligned = config();
    misaligned.region_offset++;
    ScriptClock unused_clock({1, 2, 3, 4});
    REQUIRE(myrd::run_m2_performance_probe(
                io, unused_clock, context, misaligned).has_error());

    auto out_of_range = config();
    out_of_range.region_offset = io.size_bytes();
    ScriptClock unused_clock_2({1, 2, 3, 4});
    REQUIRE(myrd::run_m2_performance_probe(
                io, unused_clock_2, context, out_of_range).has_error());

    ScriptClock non_monotonic({100, 99, 200, 201});
    REQUIRE(myrd::run_m2_performance_probe(
                io, non_monotonic, context, config()).has_error());
}

TEST_CASE(m2_performance_honors_cancel_and_deadline_before_and_during_probe) {
    FakeRawIo io;
    ScriptClock clock({1, 2, 3, 4});

    std::stop_source already_cancelled;
    already_cancelled.request_stop();
    myrd::OperationContext cancelled_context{already_cancelled.get_token()};
    auto cancelled = myrd::run_m2_performance_probe(
        io, clock, cancelled_context, config());
    REQUIRE(cancelled.has_error());
    REQUIRE(io.records.empty());

    FakeRawIo expired_io;
    ScriptClock expired_clock({1, 2, 3, 4});
    myrd::OperationContext expired_context{
        {}, std::chrono::steady_clock::now() - std::chrono::milliseconds(1)};
    auto expired = myrd::run_m2_performance_probe(
        expired_io, expired_clock, expired_context, config());
    REQUIRE(expired.has_error());
    REQUIRE(expired_io.records.empty());

    FakeRawIo midflight_io;
    std::stop_source midflight_source;
    midflight_io.cancel_source = &midflight_source;
    midflight_io.cancel_after_call = 2;
    ScriptClock midflight_clock({1, 2, 3, 4});
    myrd::OperationContext midflight_context{midflight_source.get_token()};
    auto midflight = myrd::run_m2_performance_probe(
        midflight_io, midflight_clock, midflight_context, config());
    REQUIRE(midflight.has_error());
    REQUIRE_EQ(midflight_io.records.size(), std::size_t{2});
}

TEST_CASE(m2_performance_thresholds_are_exact_and_fail_closed) {
    auto exact = myrd::compare_m2_performance(
        sample(1'000'000, 10'000), sample(700'000, 6'000));
    REQUIRE(exact.ok());
    REQUIRE_EQ(exact.value().sequential_ratio_basis_points, 7'000ull);
    REQUIRE_EQ(exact.value().random_ratio_basis_points, 6'000ull);
    REQUIRE_EQ(exact.value().sequential_threshold_basis_points, 7'000ull);
    REQUIRE_EQ(exact.value().random_threshold_basis_points, 6'000ull);
    REQUIRE(exact.value().sequential_pass);
    REQUIRE(exact.value().random_pass);
    REQUIRE(exact.value().overall_pass);

    auto below = myrd::compare_m2_performance(
        sample(1'000'000, 10'000), sample(699'999, 5'999));
    REQUIRE(below.ok());
    REQUIRE(!below.value().sequential_pass);
    REQUIRE(!below.value().random_pass);
    REQUIRE(!below.value().overall_pass);

    REQUIRE(myrd::compare_m2_performance(
                sample(0, 10'000), sample(1, 6'000)).has_error());
    REQUIRE(myrd::compare_m2_performance(
                sample(1'000'000, 0), sample(700'000, 1)).has_error());
    REQUIRE(myrd::compare_m2_performance(
                sample(1'000'000, 10'000),
                sample(700'000, 6'000, policy("-other"))).has_error());

    auto wrong_method = sample(700'000, 6'000);
    wrong_method.methodology_id = "different-method";
    REQUIRE(myrd::compare_m2_performance(
                sample(1'000'000, 10'000), wrong_method).has_error());

    REQUIRE(myrd::compare_m2_performance(
                sample(1'000'000, 10'000, {}),
                sample(700'000, 6'000, {})).has_error());

    const auto baseline = sample(1'000'000, 10'000);
    for (int changed_field = 0; changed_field < 7; ++changed_field) {
        auto changed = sample(700'000, 6'000);
        switch (changed_field) {
        case 0: changed.region_bytes++; break;
        case 1: changed.sequential_io_bytes++; break;
        case 2: changed.random_io_bytes++; break;
        case 3: changed.random_seed++; break;
        case 4: changed.sequential_warmup_operations++; break;
        case 5: changed.random_warmup_operations++; break;
        case 6: changed.region_offset++; break;
        }
        REQUIRE(myrd::compare_m2_performance(
                    baseline, changed).has_error());
    }
}
