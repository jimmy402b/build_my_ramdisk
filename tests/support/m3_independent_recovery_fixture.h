#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace myrd::test::m3_recovery_oracle {

inline constexpr std::uint64_t kVirtualBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kLogicalBlockBytes = 64ULL * 1024ULL;
inline constexpr std::uint64_t kLogicalBlockCount =
    kVirtualBytes / kLogicalBlockBytes;
inline constexpr std::uint64_t kGoldenGeneration = 5;

enum class SlotMutation {
    None,
    BaseIdentity,
    RootCrc,
    ChainDigest,
};

struct SlotSpec final {
    std::uint8_t slot_index{};
    std::uint64_t generation{kGoldenGeneration};
    SlotMutation mutation{SlotMutation::None};
};

// Builds the complete bundle from test-owned raw bytes.  No production codec,
// writer, selector, CRC, or SHA implementation participates in construction.
class IndependentRecoveryFixture final {
public:
    IndependentRecoveryFixture();
    ~IndependentRecoveryFixture() noexcept;

    IndependentRecoveryFixture(const IndependentRecoveryFixture&) = delete;
    IndependentRecoveryFixture& operator=(
        const IndependentRecoveryFixture&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    void publish(const std::optional<SlotSpec>& a,
                 const std::optional<SlotSpec>& b) const;
    void damage_root_record() const;
    void damage_chain_covered_data() const;

private:
    std::filesystem::path root_;
};

}  // namespace myrd::test::m3_recovery_oracle
