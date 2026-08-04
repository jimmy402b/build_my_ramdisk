#pragma once

#include "src/persistence/file_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
    std::uint64_t occurrence{1};
    std::optional<std::uint64_t> byte_offset;
    FaultFileAction action{};
    std::uint32_t native_code{};
};

using FaultFileObserver =
    std::function<void(FaultFilePhase, std::uint64_t)>;

class FaultFileState final {
public:
    [[nodiscard]] std::uint64_t logical_length() const noexcept;
    [[nodiscard]] std::uint64_t physical_bytes() const noexcept;
    [[nodiscard]] std::size_t allocated_pages() const noexcept;
    [[nodiscard]] std::uint64_t calls(FaultFilePhase phase) const noexcept;
    [[nodiscard]] bool closed() const noexcept;

private:
    friend class FaultFileBackend;

    static constexpr std::uint64_t kPageBytes = 4096;
    using Page = std::array<std::byte, kPageBytes>;

    explicit FaultFileState(
        std::uint64_t logical_length,
        std::vector<FaultFileRule> rules,
        FaultFileObserver observer);

    std::uint64_t logical_length_{};
    std::map<std::uint64_t, Page> pages_;
    std::array<std::uint64_t,
        static_cast<std::size_t>(FaultFilePhase::Count)> calls_{};
    std::vector<FaultFileRule> rules_;
    std::vector<bool> consumed_rules_;
    FaultFileObserver observer_;
    bool closed_{};
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
