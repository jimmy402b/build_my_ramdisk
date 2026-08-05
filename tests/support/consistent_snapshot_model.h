#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace myrd::test::consistent_model {

inline constexpr std::uint64_t kBlockBytes = 64ull * 1024ull;

enum class Kind : std::uint8_t {
    Data = 0,
    Zero = 1,
    Discard = 2,
};

struct SnapshotRecord final {
    std::uint64_t block_index{};
    std::uint64_t version{};
    Kind kind{Kind::Data};
    std::uint64_t base_generation{};
    std::vector<std::byte> data;

    bool operator==(const SnapshotRecord&) const = default;
};

struct CutToken final {
    std::uint64_t id{};

    bool operator==(const CutToken&) const = default;
};

class Model final {
public:
    Model(std::uint64_t logical_size_bytes,
          std::uint64_t backing_generation);
    Model(std::uint64_t logical_size_bytes,
          std::vector<std::byte> initial_bytes,
          std::uint64_t backing_generation);

    std::uint64_t logical_size_bytes() const noexcept;
    std::uint64_t block_count() const noexcept;
    std::uint64_t backing_generation() const noexcept;
    std::uint64_t dirty_block_count() const noexcept;
    std::optional<CutToken> active_cut() const noexcept;

    bool write(std::uint64_t offset, std::span<const std::byte> data);
    bool zero(std::uint64_t offset,
              std::uint64_t length,
              Kind requested_kind);
    std::optional<CutToken> begin_cut();
    const std::vector<SnapshotRecord>* cut_records(
        CutToken token) const noexcept;

private:
    struct BlockState final {
        std::uint64_t version{};
        Kind kind{Kind::Data};
        std::uint64_t base_generation{};
        std::vector<std::byte> value;
        bool dirty{};
    };

    std::uint64_t logical_size_bytes_{};
    std::uint64_t block_count_{};
    std::uint64_t backing_generation_{};
    std::map<std::uint64_t, BlockState> blocks_;
    std::optional<CutToken> active_cut_;
    std::vector<SnapshotRecord> cut_records_;
    std::uint64_t next_cut_id_{1};
};

} // namespace myrd::test::consistent_model
