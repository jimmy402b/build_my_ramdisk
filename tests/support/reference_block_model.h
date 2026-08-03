#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace myrd::reference {

// These values are deliberately restated instead of importing production
// block geometry. A disagreement between the contract and production must be
// visible to the model test rather than shared through one helper.
inline constexpr std::uint64_t kReferenceBlockBytes = 64ull * 1024ull;
inline constexpr std::uint64_t kReferenceSectorBytes = 512ull;

enum class DirtyKind : std::uint8_t {
    Data = 0,
    Zero = 1,
    Discard = 2,
};

enum class PersistResult : std::uint8_t {
    Persisted = 0,
    InvalidBlock,
    VersionChanged,
    BackingGenerationConflict,
};

struct DirtyRecord {
    std::uint64_t block_index{};
    std::uint64_t version{};
    DirtyKind kind{DirtyKind::Data};
    std::uint64_t base_generation{};
    std::vector<std::byte> data;

    bool operator==(const DirtyRecord&) const = default;
};

struct BlockState {
    std::uint64_t version{};
    std::uint64_t dirty_base_generation{};
    std::uint64_t clean_generation{};
    DirtyKind dirty_kind{DirtyKind::Data};
    bool dirty{};
    bool resident{};
    bool zero_state{};
};

struct Gauges {
    std::uint64_t committed_blocks{};
    std::uint64_t resident_clean_blocks{};
    std::uint64_t resident_dirty_blocks{};
    std::uint64_t zero_clean_blocks{};
    std::uint64_t zero_dirty_blocks{};
};

class BlockModel final {
public:
    BlockModel(std::uint64_t logical_size_bytes,
               std::vector<std::byte> initial_bytes,
               std::uint64_t initial_generation)
        : logical_size_bytes_(logical_size_bytes),
          block_count_((logical_size_bytes + kReferenceBlockBytes - 1) /
                       kReferenceBlockBytes),
          visible_bytes_(std::move(initial_bytes)),
          backing_bytes_(visible_bytes_),
          blocks_(static_cast<std::size_t>(block_count_)),
          backing_generation_(initial_generation) {
        for (auto& block : blocks_) {
            block.clean_generation = initial_generation;
        }
    }

    std::uint64_t logical_size_bytes() const noexcept {
        return logical_size_bytes_;
    }

    std::uint64_t block_count() const noexcept { return block_count_; }

    std::uint64_t backing_generation() const noexcept {
        return backing_generation_;
    }

    const std::vector<std::byte>& visible_bytes() const noexcept {
        return visible_bytes_;
    }

    const std::vector<std::byte>& backing_bytes() const noexcept {
        return backing_bytes_;
    }

    const BlockState& block(std::uint64_t block_index) const {
        return blocks_[static_cast<std::size_t>(block_index)];
    }

    bool valid_io(std::uint64_t offset, std::uint64_t length) const noexcept {
        return length != 0 &&
               (offset % kReferenceSectorBytes) == 0 &&
               (length % kReferenceSectorBytes) == 0 &&
               offset <= logical_size_bytes_ &&
               length <= logical_size_bytes_ - offset;
    }

    std::vector<std::byte> read(std::uint64_t offset,
                                std::uint64_t length,
                                bool materialize = true) {
        if (!valid_io(offset, length)) {
            return {};
        }
        if (materialize) {
            for_each_segment(offset, length, [&](std::uint64_t block_index,
                                                  std::uint64_t,
                                                  std::uint64_t) {
                auto& state = blocks_[static_cast<std::size_t>(block_index)];
                if (!state.zero_state) {
                    state.resident = true;
                }
            });
        }
        const auto first = static_cast<std::size_t>(offset);
        const auto last = static_cast<std::size_t>(offset + length);
        return std::vector<std::byte>(visible_bytes_.begin() + first,
                                      visible_bytes_.begin() + last);
    }

    bool write(std::uint64_t offset, std::span<const std::byte> input) {
        if (!valid_io(offset, input.size())) {
            return false;
        }
        std::copy(input.begin(), input.end(),
                  visible_bytes_.begin() + static_cast<std::size_t>(offset));
        for_each_segment(offset, input.size(), [&](std::uint64_t block_index,
                                                    std::uint64_t,
                                                    std::uint64_t) {
            auto& state = blocks_[static_cast<std::size_t>(block_index)];
            ++state.version;
            state.dirty = true;
            state.resident = true;
            state.zero_state = false;
            state.dirty_kind = DirtyKind::Data;
            state.dirty_base_generation = backing_generation_;
        });
        return true;
    }

    bool zero(std::uint64_t offset,
              std::uint64_t length,
              DirtyKind requested_kind) {
        if (!valid_io(offset, length) || requested_kind == DirtyKind::Data) {
            return false;
        }
        for_each_segment(offset, length,
                         [&](std::uint64_t block_index,
                             std::uint64_t block_offset,
                             std::uint64_t segment_length) {
            auto& state = blocks_[static_cast<std::size_t>(block_index)];
            const auto block_start = block_index * kReferenceBlockBytes;
            const auto logical_block_size = (std::min)(
                kReferenceBlockBytes, logical_size_bytes_ - block_start);
            const bool covers_logical_block =
                block_offset == 0 && segment_length == logical_block_size;
            if (!covers_logical_block && state.zero_state) {
                return;
            }

            const auto first = static_cast<std::size_t>(
                block_start + block_offset);
            std::fill_n(visible_bytes_.begin() + first,
                        static_cast<std::size_t>(segment_length),
                        std::byte{0});
            ++state.version;
            state.dirty = true;
            state.dirty_base_generation = backing_generation_;
            if (covers_logical_block) {
                state.resident = false;
                state.zero_state = true;
                state.dirty_kind = requested_kind;
            } else {
                state.resident = true;
                state.zero_state = false;
                state.dirty_kind = DirtyKind::Data;
            }
        });
        return true;
    }

    std::vector<DirtyRecord> snapshot(std::uint64_t& cursor,
                                      std::size_t max_payload_bytes,
                                      std::size_t record_limit) const {
        std::vector<DirtyRecord> records;
        if (cursor > block_count_ ||
            max_payload_bytes < kReferenceBlockBytes ||
            (max_payload_bytes % kReferenceBlockBytes) != 0 ||
            record_limit == 0) {
            return records;
        }

        std::size_t payload_bytes = 0;
        auto scan = cursor;
        while (scan < block_count_ && records.size() < record_limit) {
            const auto& state = blocks_[static_cast<std::size_t>(scan)];
            if (!state.dirty) {
                ++scan;
                continue;
            }
            if (state.dirty_kind == DirtyKind::Data &&
                max_payload_bytes - payload_bytes < kReferenceBlockBytes) {
                cursor = scan;
                return records;
            }

            DirtyRecord record{
                scan,
                state.version,
                state.dirty_kind,
                state.dirty_base_generation,
                {},
            };
            if (state.dirty_kind == DirtyKind::Data) {
                const auto first = static_cast<std::size_t>(
                    scan * kReferenceBlockBytes);
                const auto valid_bytes = static_cast<std::size_t>((std::min)(
                    kReferenceBlockBytes,
                    logical_size_bytes_ - scan * kReferenceBlockBytes));
                record.data.assign(
                    static_cast<std::size_t>(kReferenceBlockBytes),
                    std::byte{0});
                std::copy_n(
                    visible_bytes_.begin() + first,
                    valid_bytes,
                    record.data.begin());
                payload_bytes +=
                    static_cast<std::size_t>(kReferenceBlockBytes);
            }
            records.push_back(std::move(record));
            ++scan;
        }
        cursor = scan;
        return records;
    }

    bool install_backing(std::uint64_t generation,
                         std::vector<std::byte> complete_view) {
        if (generation <= backing_generation_ ||
            complete_view.size() != visible_bytes_.size()) {
            return false;
        }
        backing_generation_ = generation;
        backing_bytes_ = std::move(complete_view);
        return true;
    }

    PersistResult mark_persisted(std::uint64_t block_index,
                                 std::uint64_t expected_version,
                                 DirtyKind expected_kind,
                                 std::uint64_t persisted_generation) {
        if (block_index >= block_count_) {
            return PersistResult::InvalidBlock;
        }
        auto& state = blocks_[static_cast<std::size_t>(block_index)];
        if (!state.dirty || state.version != expected_version ||
            state.dirty_kind != expected_kind) {
            return PersistResult::VersionChanged;
        }
        if (persisted_generation <= state.dirty_base_generation ||
            persisted_generation > backing_generation_) {
            return PersistResult::BackingGenerationConflict;
        }
        state.dirty = false;
        state.clean_generation = persisted_generation;
        return PersistResult::Persisted;
    }

    std::uint64_t evict_all_clean() noexcept {
        std::uint64_t evicted = 0;
        for (auto& state : blocks_) {
            if (!state.dirty && state.resident && !state.zero_state) {
                state.resident = false;
                ++evicted;
            }
        }
        return evicted;
    }

    Gauges gauges() const noexcept {
        Gauges result{};
        for (const auto& state : blocks_) {
            if (state.zero_state) {
                if (state.dirty) {
                    ++result.zero_dirty_blocks;
                } else {
                    ++result.zero_clean_blocks;
                }
            } else if (state.resident) {
                ++result.committed_blocks;
                if (state.dirty) {
                    ++result.resident_dirty_blocks;
                } else {
                    ++result.resident_clean_blocks;
                }
            }
        }
        return result;
    }

    std::uint64_t dirty_blocks() const noexcept {
        return static_cast<std::uint64_t>(std::count_if(
            blocks_.begin(), blocks_.end(),
            [](const BlockState& state) { return state.dirty; }));
    }

    bool validate() const noexcept {
        if (logical_size_bytes_ == 0 ||
            visible_bytes_.size() != logical_size_bytes_ ||
            backing_bytes_.size() != logical_size_bytes_ ||
            blocks_.size() != block_count_) {
            return false;
        }
        for (std::uint64_t block_index = 0;
             block_index < block_count_;
             ++block_index) {
            const auto& state = blocks_[static_cast<std::size_t>(block_index)];
            if (state.clean_generation > backing_generation_ ||
                state.dirty_base_generation > backing_generation_ ||
                (state.zero_state && state.resident) ||
                (state.dirty && !state.zero_state && !state.resident) ||
                (state.dirty && state.zero_state &&
                 state.dirty_kind == DirtyKind::Data) ||
                (state.dirty && !state.zero_state &&
                 state.dirty_kind != DirtyKind::Data)) {
                return false;
            }
            if (state.zero_state) {
                const auto block_start = block_index * kReferenceBlockBytes;
                const auto logical_block_size = (std::min)(
                    kReferenceBlockBytes, logical_size_bytes_ - block_start);
                const auto first = visible_bytes_.begin() +
                    static_cast<std::size_t>(block_start);
                if (!std::all_of(
                        first,
                        first + static_cast<std::size_t>(logical_block_size),
                        [](std::byte value) { return value == std::byte{0}; })) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    template <class Visitor>
    void for_each_segment(std::uint64_t offset,
                          std::uint64_t length,
                          Visitor&& visitor) const {
        auto remaining = length;
        auto position = offset;
        while (remaining != 0) {
            const auto block_index = position / kReferenceBlockBytes;
            const auto block_offset = position % kReferenceBlockBytes;
            const auto segment_length = (std::min)(
                remaining, kReferenceBlockBytes - block_offset);
            visitor(block_index, block_offset, segment_length);
            position += segment_length;
            remaining -= segment_length;
        }
    }

    std::uint64_t logical_size_bytes_{};
    std::uint64_t block_count_{};
    std::vector<std::byte> visible_bytes_;
    std::vector<std::byte> backing_bytes_;
    std::vector<BlockState> blocks_;
    std::uint64_t backing_generation_{};
};

} // namespace myrd::reference
