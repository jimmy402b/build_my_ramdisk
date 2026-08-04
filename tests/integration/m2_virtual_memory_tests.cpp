#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "src/storage/dynamic_block_store.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kKiB = 1024ull;
constexpr std::uint64_t kMiB = 1024ull * kKiB;
constexpr std::uint64_t kGiB = 1024ull * kMiB;
constexpr std::uint64_t kTiB = 1024ull * kGiB;
constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;
constexpr std::uint64_t kOneGiBBlocks = kGiB / kBlock;
constexpr std::uint64_t kEmptyCommitLimit = 128ull * kMiB;
constexpr std::uint64_t kWriteCommitLimit = (11ull * kGiB) / 10ull;
constexpr std::uint64_t kRequiredReclaim = (9ull * kGiB) / 10ull;
// Windows CRT may retain small freed heap segments used by sparse leaves and
// the arena's committed-block index. Keep this separate from measured sample
// noise; exact reservation release, handles, and threads remain zero-tolerance.
// The 1 MiB ceiling matches the independently reviewed Task 16 resource gate.
constexpr std::uint64_t kAllocatorAllowance = 1ull * kMiB;
constexpr auto kReclaimDeadline = std::chrono::seconds(10);

struct ProcessSample {
    std::uint64_t private_usage{};
    std::uint64_t pagefile_usage{};
    std::uint64_t working_set{};
    std::uint64_t peak_working_set{};
    std::uint64_t handles{};
    std::uint64_t threads{};
    bool ok{};
};

struct BaselineSample {
    ProcessSample median{};
    std::uint64_t private_noise{};
    std::uint64_t pagefile_noise{};
    std::uint64_t working_set_noise{};
};

std::uint64_t saturating_subtract(
    std::uint64_t left, std::uint64_t right) noexcept {
    return left > right ? left - right : 0;
}

std::uint64_t current_process_thread_count() noexcept {
    const auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::uint64_t count = 0;
    if (::Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == ::GetCurrentProcessId()) {
                ++count;
            }
            entry.dwSize = sizeof(entry);
        } while (::Thread32Next(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    return count;
}

ProcessSample sample_process() noexcept {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    DWORD handles = 0;
    const auto threads = current_process_thread_count();
    const bool memory_ok = ::GetProcessMemoryInfo(
        ::GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
        sizeof(memory)) != FALSE;
    const bool handles_ok =
        ::GetProcessHandleCount(::GetCurrentProcess(), &handles) != FALSE;
    return ProcessSample{
        static_cast<std::uint64_t>(memory.PrivateUsage),
        static_cast<std::uint64_t>(memory.PagefileUsage),
        static_cast<std::uint64_t>(memory.WorkingSetSize),
        static_cast<std::uint64_t>(memory.PeakWorkingSetSize),
        static_cast<std::uint64_t>(handles),
        threads,
        memory_ok && handles_ok && threads != 0,
    };
}

void print_sample(std::string_view phase, const ProcessSample& sample) {
    std::cout << "VM_SAMPLE phase=" << phase
              << " private_usage=" << sample.private_usage
              << " pagefile_usage=" << sample.pagefile_usage
              << " working_set=" << sample.working_set
              << " peak_working_set=" << sample.peak_working_set
              << " handles=" << sample.handles
              << " threads=" << sample.threads << '\n';
}

void print_store_stats(std::string_view phase,
                       const myrd::DynamicBlockStoreStats& stats,
                       std::uint64_t allocated_leaves) {
    std::cout << "VM_STORE_STATS phase=" << phase
              << " logical_size=" << stats.logical_size_bytes
              << " reserved=" << stats.reserved_bytes
              << " committed=" << stats.committed_bytes
              << " resident_clean=" << stats.resident_clean_blocks
              << " resident_dirty=" << stats.resident_dirty_blocks
              << " zero_clean=" << stats.zero_clean_blocks
              << " zero_dirty=" << stats.zero_dirty_blocks
              << " loading=" << stats.loading_blocks
              << " unmap_bytes=" << stats.unmap_bytes_total
              << " commit_failures=" << stats.commit_failures
              << " decommit_failures=" << stats.decommit_failures
              << " allocated_leaves=" << allocated_leaves << '\n';
}

std::uint64_t median(std::vector<std::uint64_t> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

BaselineSample capture_baseline(std::string_view phase) {
    constexpr std::size_t kSampleCount = 7;
    (void)sample_process();
    std::array<ProcessSample, kSampleCount> samples{};
    for (auto& sample : samples) {
        sample = sample_process();
        REQUIRE(sample.ok);
        ::Sleep(1);
    }

    const auto collect = [&](auto member) {
        std::vector<std::uint64_t> values;
        values.reserve(samples.size());
        for (const auto& sample : samples) values.push_back(sample.*member);
        return values;
    };
    const auto private_values = collect(&ProcessSample::private_usage);
    const auto pagefile_values = collect(&ProcessSample::pagefile_usage);
    const auto working_values = collect(&ProcessSample::working_set);
    BaselineSample baseline;
    baseline.median.private_usage = median(private_values);
    baseline.median.pagefile_usage = median(pagefile_values);
    baseline.median.working_set = median(working_values);
    baseline.median.peak_working_set =
        median(collect(&ProcessSample::peak_working_set));
    baseline.median.handles = median(collect(&ProcessSample::handles));
    baseline.median.threads = median(collect(&ProcessSample::threads));
    baseline.median.ok = true;
    baseline.private_noise =
        *std::max_element(private_values.begin(), private_values.end()) -
        *std::min_element(private_values.begin(), private_values.end());
    baseline.pagefile_noise =
        *std::max_element(pagefile_values.begin(), pagefile_values.end()) -
        *std::min_element(pagefile_values.begin(), pagefile_values.end());
    baseline.working_set_noise =
        *std::max_element(working_values.begin(), working_values.end()) -
        *std::min_element(working_values.begin(), working_values.end());
    print_sample(phase, baseline.median);
    std::cout << "VM_BASELINE_NOISE phase=" << phase
              << " private_noise=" << baseline.private_noise
              << " pagefile_noise=" << baseline.pagefile_noise
              << " working_set_noise=" << baseline.working_set_noise
              << " allocator_allowance=" << kAllocatorAllowance << '\n';
    return baseline;
}

ProcessSample stable_sample(std::string_view phase) {
    constexpr std::size_t kSampleCount = 5;
    std::array<ProcessSample, kSampleCount> samples{};
    for (auto& sample : samples) {
        sample = sample_process();
        REQUIRE(sample.ok);
        ::Sleep(1);
    }
    const auto collect = [&](auto member) {
        std::vector<std::uint64_t> values;
        values.reserve(samples.size());
        for (const auto& sample : samples) values.push_back(sample.*member);
        return values;
    };
    ProcessSample result;
    result.private_usage = median(collect(&ProcessSample::private_usage));
    result.pagefile_usage = median(collect(&ProcessSample::pagefile_usage));
    result.working_set = median(collect(&ProcessSample::working_set));
    result.peak_working_set = median(collect(&ProcessSample::peak_working_set));
    result.handles = median(collect(&ProcessSample::handles));
    result.threads = median(collect(&ProcessSample::threads));
    result.ok = true;
    print_sample(phase, result);
    return result;
}

void require_commit_preflight() {
#if defined(NDEBUG)
    constexpr bool kReleaseBuild = true;
#else
    constexpr bool kReleaseBuild = false;
#endif
    REQUIRE(sizeof(void*) == 8);
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    REQUIRE(::GetPerformanceInfo(&performance, sizeof(performance)) != FALSE);
    REQUIRE(performance.CommitLimit >= performance.CommitTotal);
    const auto available_pages =
        performance.CommitLimit - performance.CommitTotal;
    const auto available_commit =
        static_cast<std::uint64_t>(available_pages) *
        static_cast<std::uint64_t>(performance.PageSize);
    std::cout << "VM_PREFLIGHT pointer_bits=" << (sizeof(void*) * 8)
              << " release_build=" << kReleaseBuild
              << " available_commit=" << available_commit
              << " required_commit=" << (kWriteCommitLimit + 256ull * kMiB)
              << '\n';
    REQUIRE(kReleaseBuild);
    REQUIRE(available_commit >= kWriteCommitLimit + 256ull * kMiB);
}

std::set<std::uintptr_t> exact_reserved_allocations(
    std::uint64_t reservation_size) {
    SYSTEM_INFO system{};
    ::GetSystemInfo(&system);
    const auto maximum = reinterpret_cast<std::uintptr_t>(
        system.lpMaximumApplicationAddress);
    std::set<std::uintptr_t> allocations;
    std::uintptr_t address = 0;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION information{};
        const auto queried = ::VirtualQuery(
            reinterpret_cast<const void*>(address),
            &information, sizeof(information));
        REQUIRE(queried == sizeof(information));
        const auto base = reinterpret_cast<std::uintptr_t>(
            information.BaseAddress);
        const auto region_size =
            static_cast<std::uint64_t>(information.RegionSize);
        if (information.State == MEM_RESERVE &&
            information.AllocationBase == information.BaseAddress &&
            region_size == reservation_size) {
            allocations.insert(base);
        }
        REQUIRE(region_size != 0);
        REQUIRE(base <= (std::numeric_limits<std::uintptr_t>::max)() -
                            static_cast<std::uintptr_t>(region_size));
        const auto next =
            base + static_cast<std::uintptr_t>(region_size);
        REQUIRE(next > address);
        address = next;
    }
    return allocations;
}

std::uintptr_t require_one_new_reservation(
    const std::set<std::uintptr_t>& before,
    const std::set<std::uintptr_t>& after,
    std::uint64_t reservation_size) {
    std::vector<std::uintptr_t> added;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    if (added.size() != 1) {
        std::cerr << "VM_RESERVATION_DIFF reservation_size=" << reservation_size
                  << " before=" << before.size() << " after=" << after.size()
                  << " added=" << added.size() << '\n';
    }
    REQUIRE(added.size() == 1);
    std::cout << "VM_RESERVATION size=" << reservation_size
              << " base=" << added.front() << '\n';
    return added.front();
}

MEMORY_BASIC_INFORMATION query_address(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION information{};
    REQUIRE(::VirtualQuery(reinterpret_cast<const void*>(address),
                           &information, sizeof(information)) ==
            sizeof(information));
    return information;
}

void require_reserved_noaccess(
    std::uintptr_t address, std::uintptr_t allocation_base) {
    const auto information = query_address(address);
    REQUIRE(information.State == MEM_RESERVE);
    REQUIRE(reinterpret_cast<std::uintptr_t>(information.AllocationBase) ==
            allocation_base);
    REQUIRE(information.AllocationProtect == PAGE_NOACCESS);
}

void require_committed_readwrite(
    std::uintptr_t address, std::uintptr_t allocation_base) {
    const auto information = query_address(address);
    REQUIRE(information.State == MEM_COMMIT);
    REQUIRE(reinterpret_cast<std::uintptr_t>(information.AllocationBase) ==
            allocation_base);
    REQUIRE((information.Protect & 0xffu) == PAGE_READWRITE);
}

void require_released(std::uintptr_t address) {
    const auto information = query_address(address);
    REQUIRE(information.State == MEM_FREE);
    REQUIRE(information.AllocationBase == nullptr);
}

myrd::DynamicBlockStoreCreateArgs store_args(
    std::uint64_t logical_size, bool large_workload) {
    const auto low = large_workload ? kGiB : 4ull * kBlock;
    const auto soft = large_workload ? 2ull * kGiB : 8ull * kBlock;
    const auto hard = large_workload ? 3ull * kGiB : 16ull * kBlock;
    return {
        logical_size,
        512,
        {
            soft,
            hard,
            low,
            4ull * kBlock,
            4,
            256,
        },
        myrd::InitialBlockState::CleanZero,
        {},
        0,
    };
}

void require_ok(const myrd::Result<void>& result, std::string_view operation,
                std::uint64_t index = 0) {
    if (!result.ok()) {
        std::wcerr << L"VM_OPERATION_FAIL operation="
                   << std::wstring(operation.begin(), operation.end())
                   << L" index=" << index
                   << L" domain=" << static_cast<unsigned>(result.error().domain)
                   << L" code=" << result.error().code
                   << L" native=" << result.error().native_code
                   << L" detail=" << result.error().message << L'\n';
    }
    REQUIRE(result.ok());
}

void fill_block_pattern(std::uint64_t block_index,
                        std::span<std::byte> bytes) noexcept {
    REQUIRE(bytes.size() == static_cast<std::size_t>(kBlock));
    constexpr auto kMix = 0x9E37'79B9'7F4A'7C15ull;
    for (std::size_t offset = 0; offset < bytes.size();
         offset += sizeof(std::uint64_t)) {
        const auto word = static_cast<std::uint64_t>(offset) /
                          sizeof(std::uint64_t);
        auto value = (block_index + 1) * kMix;
        value ^= word * 0xBF58'476D'1CE4'E5B9ull;
        value ^= value >> 30;
        value *= 0x94D0'49BB'1331'11EBull;
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
}

void require_process_returned_to_baseline(
    const BaselineSample& baseline,
    const ProcessSample& after,
    std::string_view phase) {
    const auto private_limit = baseline.median.private_usage +
                               baseline.private_noise + kAllocatorAllowance;
    const auto pagefile_limit = baseline.median.pagefile_usage +
                                baseline.pagefile_noise + kAllocatorAllowance;
    const auto working_set_limit = baseline.median.working_set +
                                   baseline.working_set_noise +
                                   kAllocatorAllowance;
    std::cout << "VM_CLEANUP_GATE phase=" << phase
              << " observed_private=" << after.private_usage
              << " private_limit=" << private_limit
              << " observed_pagefile=" << after.pagefile_usage
              << " pagefile_limit=" << pagefile_limit
              << " observed_working_set=" << after.working_set
              << " working_set_limit=" << working_set_limit
              << " observed_handles=" << after.handles
              << " expected_handles=" << baseline.median.handles
              << " observed_threads=" << after.threads
              << " expected_threads=" << baseline.median.threads << '\n';
    REQUIRE(after.private_usage <= private_limit);
    REQUIRE(after.pagefile_usage <= pagefile_limit);
    REQUIRE(after.working_set <= working_set_limit);
    REQUIRE(after.handles == baseline.median.handles);
    REQUIRE(after.threads == baseline.median.threads);
}

ProcessSample wait_for_private_reclaim(
    std::uint64_t peak_private, std::uint64_t required_reclaim) {
    const auto deadline = std::chrono::steady_clock::now() + kReclaimDeadline;
    auto best = sample_process();
    REQUIRE(best.ok);
    while (saturating_subtract(peak_private, best.private_usage) <
               required_reclaim &&
           std::chrono::steady_clock::now() < deadline) {
        ::Sleep(20);
        const auto current = sample_process();
        REQUIRE(current.ok);
        if (current.private_usage < best.private_usage) best = current;
    }
    print_sample("64g_after_unmap", best);
    return best;
}

} // namespace

TEST_CASE(real_virtual_memory_64_gib_commit_decommit_and_process_metrics) {
    require_commit_preflight();
    const auto baseline = capture_baseline("64g_baseline");
    const auto before_reservations = exact_reserved_allocations(64ull * kGiB);

    auto created = myrd::DynamicBlockStore::create(store_args(64ull * kGiB, true));
    REQUIRE(created.ok());
    auto store = created.move_value();
    const auto after_reservations = exact_reserved_allocations(64ull * kGiB);
    const auto allocation_base = require_one_new_reservation(
        before_reservations, after_reservations, 64ull * kGiB);
    require_reserved_noaccess(allocation_base, allocation_base);

    const auto constructed = stable_sample("64g_constructed");
    const auto initial_stats = store->stats();
    print_store_stats("64g_constructed", initial_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(initial_stats.logical_size_bytes == 64ull * kGiB);
    REQUIRE(initial_stats.reserved_bytes == 64ull * kGiB);
    REQUIRE(initial_stats.committed_bytes == 0);
    REQUIRE(store->allocated_leaf_count_for_test() == 0);
    const auto fixed_metadata_delta = saturating_subtract(
        constructed.private_usage, baseline.median.private_usage);
    std::cout << "VM_EMPTY_GATE observed_delta=" << fixed_metadata_delta
              << " threshold=" << kEmptyCommitLimit << '\n';
    REQUIRE(fixed_metadata_delta <= kEmptyCommitLimit);
    REQUIRE(constructed.threads == baseline.median.threads + 1);

    std::array<std::byte, static_cast<std::size_t>(kBlock)> block{};
    const std::array<std::uint64_t, 3> sparse_blocks{
        0, store->total_blocks() / 2, store->total_blocks() - 1};
    for (const auto block_index : sparse_blocks) {
        require_ok(store->read({}, block_index * kBlock, block),
                   "sparse-zero-read", block_index);
        REQUIRE(std::all_of(block.begin(), block.end(),
                            [](std::byte value) { return value == std::byte{}; }));
        require_reserved_noaccess(
            allocation_base + block_index * kBlock, allocation_base);
    }
    const auto sparse_read_stats = store->stats();
    print_store_stats("64g_after_sparse_reads", sparse_read_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(sparse_read_stats.committed_bytes == 0);
    REQUIRE(store->allocated_leaf_count_for_test() == 0);
    (void)stable_sample("64g_after_sparse_reads");

    for (std::uint64_t block_index = 0; block_index < kOneGiBBlocks;
         ++block_index) {
        fill_block_pattern(block_index, block);
        require_ok(store->write({}, block_index * kBlock, block),
                   "one-gib-write", block_index);
    }
    const auto write_stats = store->stats();
    print_store_stats("64g_after_write", write_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(write_stats.committed_bytes == kGiB);
    REQUIRE(write_stats.resident_dirty_blocks == kOneGiBBlocks);
    REQUIRE(store->allocated_leaf_count_for_test() == 16);
    require_committed_readwrite(allocation_base, allocation_base);
    require_committed_readwrite(
        allocation_base + (kOneGiBBlocks / 2) * kBlock, allocation_base);
    require_committed_readwrite(
        allocation_base + (kOneGiBBlocks - 1) * kBlock, allocation_base);
    const auto after_write = stable_sample("64g_after_write");
    const auto peak_delta = saturating_subtract(
        after_write.private_usage, baseline.median.private_usage);
    const auto data_private_growth = saturating_subtract(
        after_write.private_usage, constructed.private_usage);
    std::cout << "VM_WRITE_GATE store_committed="
              << write_stats.committed_bytes
              << " observed_total_delta=" << peak_delta
              << " observed_data_delta=" << data_private_growth
              << " upper_threshold="
              << (fixed_metadata_delta + kWriteCommitLimit)
              << " lower_threshold=" << kRequiredReclaim << '\n';
    REQUIRE(peak_delta <= fixed_metadata_delta + kWriteCommitLimit);
    REQUIRE(data_private_growth >= kRequiredReclaim);

    for (std::uint64_t block_index = 0; block_index < kOneGiBBlocks;
         ++block_index) {
        require_ok(store->read({}, block_index * kBlock, block),
                   "one-gib-readback", block_index);
        std::array<std::byte, static_cast<std::size_t>(kBlock)> expected{};
        fill_block_pattern(block_index, expected);
        REQUIRE(std::equal(block.begin(), block.end(), expected.begin()));
    }
    const auto after_read = stable_sample("64g_after_readback");
    const auto readback_stats = store->stats();
    print_store_stats("64g_after_readback", readback_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(readback_stats.committed_bytes == kGiB);
    REQUIRE(readback_stats.resident_dirty_blocks == kOneGiBBlocks);
    const auto peak_private = (std::max)(
        after_write.private_usage, after_read.private_usage);

    const myrd::DataRange range{0, kGiB};
    require_ok(store->unmap(
                   {{}, std::chrono::steady_clock::now() +
                            std::chrono::seconds(60)},
                   std::span<const myrd::DataRange>(&range, 1)),
               "one-gib-unmap");
    const auto unmapped_stats = store->stats();
    print_store_stats("64g_after_unmap", unmapped_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(unmapped_stats.committed_bytes == 0);
    REQUIRE(unmapped_stats.resident_dirty_blocks == 0);
    REQUIRE(unmapped_stats.zero_dirty_blocks == kOneGiBBlocks);
    REQUIRE(unmapped_stats.unmap_bytes_total == kGiB);
    require_reserved_noaccess(allocation_base, allocation_base);
    require_reserved_noaccess(
        allocation_base + (kOneGiBBlocks / 2) * kBlock, allocation_base);
    require_reserved_noaccess(
        allocation_base + (kOneGiBBlocks - 1) * kBlock, allocation_base);
    const auto reclaim_started = std::chrono::steady_clock::now();
    const auto after_unmap =
        wait_for_private_reclaim(peak_private, kRequiredReclaim);
    const auto reclaim_elapsed =
        std::chrono::steady_clock::now() - reclaim_started;
    const auto reclaimed = saturating_subtract(
        peak_private, after_unmap.private_usage);
    std::cout << "VM_RECLAIM_GATE reclaimed=" << reclaimed
              << " threshold=" << kRequiredReclaim
              << " deadline_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     kReclaimDeadline).count()
              << " actual_elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     reclaim_elapsed).count()
              << '\n';
    REQUIRE(reclaimed >= kRequiredReclaim);
    REQUIRE(store->validate_invariants_for_test().ok());
    print_store_stats("64g_before_destroy", store->stats(),
                      store->allocated_leaf_count_for_test());

    store->close();
    store.reset();
    require_released(allocation_base);
    const auto destroyed = stable_sample("64g_destroyed");
    require_process_returned_to_baseline(baseline, destroyed, "64g_destroyed");
}

TEST_CASE(real_virtual_memory_1_tib_sparse_addressing_and_release) {
    require_commit_preflight();
    const auto baseline = capture_baseline("1t_baseline");
    const auto before_reservations = exact_reserved_allocations(kTiB);

    auto created = myrd::DynamicBlockStore::create(store_args(kTiB, false));
    REQUIRE(created.ok());
    auto store = created.move_value();
    const auto after_reservations = exact_reserved_allocations(kTiB);
    const auto allocation_base = require_one_new_reservation(
        before_reservations, after_reservations, kTiB);
    require_reserved_noaccess(allocation_base, allocation_base);

    const auto constructed = stable_sample("1t_constructed");
    const auto initial_stats = store->stats();
    print_store_stats("1t_constructed", initial_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(initial_stats.logical_size_bytes == kTiB);
    REQUIRE(initial_stats.reserved_bytes == kTiB);
    REQUIRE(initial_stats.committed_bytes == 0);
    REQUIRE(store->total_blocks() == kTiB / kBlock);
    REQUIRE(store->allocated_leaf_count_for_test() == 0);
    REQUIRE(saturating_subtract(
                constructed.private_usage, baseline.median.private_usage) <=
            kEmptyCommitLimit);

    const std::array<std::uint64_t, 3> sparse_blocks{
        0, store->total_blocks() / 2, store->total_blocks() - 1};
    std::array<std::byte, static_cast<std::size_t>(kBlock)> block{};
    for (const auto block_index : sparse_blocks) {
        require_ok(store->read({}, block_index * kBlock, block),
                   "one-tib-zero-read", block_index);
        REQUIRE(std::all_of(block.begin(), block.end(),
                            [](std::byte value) { return value == std::byte{}; }));
        require_reserved_noaccess(
            allocation_base + block_index * kBlock, allocation_base);
    }
    const auto sparse_zero_stats = store->stats();
    print_store_stats("1t_after_sparse_zero_reads", sparse_zero_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(sparse_zero_stats.committed_bytes == 0);
    REQUIRE(store->allocated_leaf_count_for_test() == 0);
    (void)stable_sample("1t_after_sparse_zero_reads");

    for (const auto block_index : sparse_blocks) {
        fill_block_pattern(block_index, block);
        require_ok(store->write({}, block_index * kBlock, block),
                   "one-tib-sparse-write", block_index);
        require_committed_readwrite(
            allocation_base + block_index * kBlock, allocation_base);
    }
    const auto sparse_stats = store->stats();
    print_store_stats("1t_after_sparse_writes", sparse_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(sparse_stats.committed_bytes == sparse_blocks.size() * kBlock);
    REQUIRE(sparse_stats.resident_dirty_blocks == sparse_blocks.size());
    REQUIRE(store->allocated_leaf_count_for_test() == sparse_blocks.size());
    const auto after_sparse_writes = stable_sample("1t_after_sparse_writes");

    for (const auto block_index : sparse_blocks) {
        require_ok(store->read({}, block_index * kBlock, block),
                   "one-tib-sparse-readback", block_index);
        std::array<std::byte, static_cast<std::size_t>(kBlock)> expected{};
        fill_block_pattern(block_index, expected);
        REQUIRE(std::equal(block.begin(), block.end(), expected.begin()));
    }
    const auto after_sparse = stable_sample("1t_after_sparse_readback");
    const auto sparse_readback_stats = store->stats();
    print_store_stats("1t_after_sparse_readback", sparse_readback_stats,
                      store->allocated_leaf_count_for_test());
    REQUIRE(sparse_readback_stats == sparse_stats);
    const auto sparse_private_delta = saturating_subtract(
        (std::max)(after_sparse_writes.private_usage,
                   after_sparse.private_usage),
        constructed.private_usage);
    std::cout << "VM_1T_SPARSE_GATE committed="
              << sparse_stats.committed_bytes
              << " leaves=" << store->allocated_leaf_count_for_test()
              << " private_delta=" << sparse_private_delta << '\n';
    REQUIRE(sparse_private_delta <= 16ull * kMiB);
    REQUIRE(store->validate_invariants_for_test().ok());
    print_store_stats("1t_before_destroy", store->stats(),
                      store->allocated_leaf_count_for_test());

    store->close();
    store.reset();
    require_released(allocation_base);
    const auto destroyed = stable_sample("1t_destroyed");
    require_process_returned_to_baseline(baseline, destroyed, "1t_destroyed");
}
