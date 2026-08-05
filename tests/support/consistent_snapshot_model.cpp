#include "consistent_snapshot_model.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace myrd::test::consistent_model {

Model::Model(std::uint64_t logical_size_bytes,
             std::uint64_t backing_generation)
    : Model(logical_size_bytes,
            std::vector<std::byte>(
                static_cast<std::size_t>(logical_size_bytes),
                std::byte{0}),
            backing_generation) {}

Model::Model(std::uint64_t logical_size_bytes,
             std::vector<std::byte> initial_bytes,
             std::uint64_t backing_generation)
    : logical_size_bytes_(logical_size_bytes),
      block_count_(logical_size_bytes / kBlockBytes +
                   (logical_size_bytes % kBlockBytes != 0 ? 1 : 0)),
      backing_generation_(backing_generation) {
    if (initial_bytes.size() != logical_size_bytes) {
        throw std::invalid_argument("initial image size does not match geometry");
    }
    for (std::uint64_t block = 0; block < block_count_; ++block) {
        BlockState state{};
        state.base_generation = backing_generation_;
        state.value.assign(
            static_cast<std::size_t>(kBlockBytes), std::byte{0});
        const auto block_start = block * kBlockBytes;
        const auto valid_bytes = static_cast<std::size_t>((std::min)(
            kBlockBytes, logical_size_bytes_ - block_start));
        std::copy_n(
            initial_bytes.begin() + static_cast<std::size_t>(block_start),
            valid_bytes,
            state.value.begin());
        blocks_.emplace(block, std::move(state));
    }
}

std::uint64_t Model::logical_size_bytes() const noexcept {
    return logical_size_bytes_;
}

std::uint64_t Model::block_count() const noexcept { return block_count_; }

std::uint64_t Model::backing_generation() const noexcept {
    return backing_generation_;
}

std::uint64_t Model::dirty_block_count() const noexcept {
    return static_cast<std::uint64_t>(std::count_if(
        blocks_.begin(), blocks_.end(), [](const auto& entry) {
            return entry.second.dirty;
        }));
}

std::optional<CutToken> Model::active_cut() const noexcept {
    return active_cut_;
}

bool Model::write(std::uint64_t offset,
                  std::span<const std::byte> data) {
    if (data.empty() || offset > logical_size_bytes_ ||
        data.size() > logical_size_bytes_ - offset) {
        return false;
    }
    const auto first_block = offset / kBlockBytes;
    const auto last_block =
        (offset + static_cast<std::uint64_t>(data.size()) - 1) /
        kBlockBytes;
    for (auto block = first_block; block <= last_block; ++block) {
        const auto found = blocks_.find(block);
        if (found == blocks_.end()) {
            return false;
        }
        const auto& state = found->second;
        if (state.version == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
    }
    std::size_t source_offset = 0;
    for (auto block = first_block; block <= last_block; ++block) {
        auto& state = blocks_.at(block);
        const auto block_start = block * kBlockBytes;
        const auto segment_start = (std::max)(offset, block_start);
        const auto segment_end = (std::min)(
            offset + static_cast<std::uint64_t>(data.size()),
            block_start + kBlockBytes);
        const auto block_offset = static_cast<std::size_t>(
            segment_start - block_start);
        const auto segment_size = static_cast<std::size_t>(
            segment_end - segment_start);
        std::copy_n(
            data.begin() + static_cast<std::ptrdiff_t>(source_offset),
            segment_size,
            state.value.begin() +
                static_cast<std::ptrdiff_t>(block_offset));
        source_offset += segment_size;
        ++state.version;
        state.kind = Kind::Data;
        state.base_generation = backing_generation_;
        state.dirty = true;
    }
    return true;
}

bool Model::zero(std::uint64_t offset,
                 std::uint64_t length,
                 Kind requested_kind) {
    if (requested_kind == Kind::Data || length == 0 ||
        offset > logical_size_bytes_ ||
        length > logical_size_bytes_ - offset) {
        return false;
    }
    const auto first_block = offset / kBlockBytes;
    const auto last_block = (offset + length - 1) / kBlockBytes;
    for (auto block = first_block; block <= last_block; ++block) {
        const auto found = blocks_.find(block);
        if (found == blocks_.end() ||
            found->second.version ==
                (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
    }
    for (auto block = first_block; block <= last_block; ++block) {
        const auto block_start = block * kBlockBytes;
        const auto logical_block_size = (std::min)(
            kBlockBytes, logical_size_bytes_ - block_start);
        const auto segment_start = (std::max)(offset, block_start);
        const auto segment_end = (std::min)(
            offset + length, block_start + logical_block_size);
        const bool whole_block = segment_start == block_start &&
            segment_end == block_start + logical_block_size;
        auto& state = blocks_.at(block);
        std::fill(
            state.value.begin() + static_cast<std::ptrdiff_t>(
                segment_start - block_start),
            state.value.begin() + static_cast<std::ptrdiff_t>(
                segment_end - block_start),
            std::byte{0});
        ++state.version;
        state.kind = whole_block ? requested_kind : Kind::Data;
        state.base_generation = backing_generation_;
        state.dirty = true;
    }
    return true;
}

std::optional<CutToken> Model::begin_cut() {
    if (active_cut_.has_value() || next_cut_id_ == 0 ||
        next_cut_id_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::nullopt;
    }
    const CutToken token{next_cut_id_++};
    std::vector<SnapshotRecord> next_records;
    next_records.reserve(static_cast<std::size_t>(dirty_block_count()));
    for (const auto& [block_index, state] : blocks_) {
        if (!state.dirty) {
            continue;
        }
        SnapshotRecord record{
            block_index,
            state.version,
            state.kind,
            state.base_generation,
            {},
        };
        if (state.kind == Kind::Data) {
            record.data = state.value;
        }
        next_records.push_back(std::move(record));
    }
    active_cut_ = token;
    cut_records_ = std::move(next_records);
    return token;
}

const std::vector<SnapshotRecord>* Model::cut_records(
    CutToken token) const noexcept {
    return active_cut_ == token ? &cut_records_ : nullptr;
}

} // namespace myrd::test::consistent_model
