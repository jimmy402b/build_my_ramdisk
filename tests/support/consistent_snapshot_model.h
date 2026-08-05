#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd::test::consistent_model {

inline constexpr std::uint64_t kBlockBytes = 64ull * 1024ull;
inline constexpr std::uint64_t kSectorBytes = 512ull;

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

enum class OperationKind : std::uint8_t {
    Write = 0,
    Zero = 1,
    Discard = 2,
    Begin = 3,
    Copy = 4,
    Finish = 5,
    Abort = 6,
    Read = 7,
    Evict = 8,
    Load = 9,
};

struct Operation final {
    OperationKind kind{OperationKind::Write};
    std::uint64_t offset{};
    std::uint64_t length{};
    std::uint64_t payload_seed{};
    std::uint64_t max_payload_bytes{kBlockBytes};
    std::size_t record_limit{1};

    bool operator==(const Operation&) const = default;
};

struct Schedule final {
    std::uint64_t seed{};
    std::uint64_t block_count{};
    std::vector<Operation> operations;

    std::uint64_t logical_size_bytes() const noexcept {
        return block_count * kBlockBytes;
    }

    bool operator==(const Schedule&) const = default;
};

Schedule generate_schedule(std::uint64_t seed,
                           std::size_t operation_count);
std::vector<std::byte> operation_payload(const Operation& operation);
std::string describe_operation(std::size_t index,
                               const Operation& operation);
using FailurePredicate = std::function<bool(const Schedule&)>;
std::optional<Schedule> reduce_failure(
    const Schedule& schedule,
    const FailurePredicate& still_fails);

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
    std::optional<std::vector<std::byte>> read(
        std::uint64_t offset,
        std::uint64_t length) const;
    std::optional<CutToken> begin_cut();
    const std::vector<SnapshotRecord>* cut_records(
        CutToken token) const noexcept;
    std::optional<std::vector<SnapshotRecord>> copy_cut(
        CutToken token,
        std::uint64_t& cursor,
        std::uint64_t max_payload_bytes,
        std::size_t record_limit) const;
    bool finish_cut(CutToken token, std::uint64_t cursor) noexcept;
    bool abort_cut(CutToken token) noexcept;

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
