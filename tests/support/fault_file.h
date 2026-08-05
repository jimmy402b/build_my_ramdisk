#pragma once

#include "src/persistence/file_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace myrd::persistence::test {

enum class FaultFilePhase : std::uint8_t {
    Read,
    Write,
    Flush,
    Length,
    Truncate,
    Close,
    Count,
};

enum class FaultFileAction : std::uint8_t {
    ShortRead,
    ShortWrite,
    ZeroProgress,
    DiskFull,
    TornWrite,
    FlushError,
    ReadbackCorruption,
    CloseError,
    NativeError,
};

struct FaultFileRule {
    FaultFilePhase phase{};
    // Rules without byte_offset target exactly this phase call. Byte-addressed
    // rules arm on this call and remain pending until a later half-open request
    // range actually contains byte_offset.
    std::uint64_t occurrence{1};
    std::optional<std::uint64_t> byte_offset;
    FaultFileAction action{};
    std::uint32_t native_code{};
};

using FaultFileObserver =
    std::function<void(FaultFilePhase, std::uint64_t)>;

struct FaultFileRequest final {
    FaultFilePhase phase{};
    std::uint64_t call{};
    std::uint64_t offset{};
    std::uint64_t length{};
};

class FaultFileState final {
public:
    [[nodiscard]] std::uint64_t logical_length() const;
    [[nodiscard]] std::uint64_t physical_bytes() const;
    [[nodiscard]] std::size_t allocated_pages() const;
    [[nodiscard]] std::uint64_t calls(FaultFilePhase phase) const;
    [[nodiscard]] bool closed() const;
    void force_length_for_test(std::uint64_t logical_length);
    void clear_requests_for_test();
    [[nodiscard]] std::vector<FaultFileRequest> requests() const;
    [[nodiscard]] bool request_trace_overflowed() const;

private:
    friend class FaultFileBackend;

    static constexpr std::uint64_t kPageBytes = 4096;
    using Page = std::array<std::byte, kPageBytes>;

    explicit FaultFileState(
        std::uint64_t logical_length,
        std::vector<FaultFileRule> rules,
        FaultFileObserver observer);

    // This mutex is the single linearization boundary for every backend
    // operation and every public state getter.
    mutable std::mutex mutex_;
    std::uint64_t logical_length_{};
    std::map<std::uint64_t, Page> pages_;
    std::array<std::uint64_t,
        static_cast<std::size_t>(FaultFilePhase::Count)> calls_{};
    std::vector<FaultFileRule> rules_;
    std::vector<bool> consumed_rules_;
    const FaultFileObserver observer_;
    bool closed_{};
    static constexpr std::size_t kMaxRequestTrace = 8192;
    std::array<FaultFileRequest, kMaxRequestTrace> requests_{};
    std::size_t request_count_{};
    bool request_trace_overflowed_{};
};

struct FaultFile {
    std::unique_ptr<IPersistenceFile> file;
    std::shared_ptr<FaultFileState> state;
};

[[nodiscard]] FaultFile make_fault_file(
    std::uint64_t logical_length = 0,
    std::vector<FaultFileRule> rules = {},
    FaultFileObserver observer = {});

} // namespace myrd::persistence::test
