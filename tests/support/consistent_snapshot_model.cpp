#include "consistent_snapshot_model.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace myrd::test::consistent_model {

namespace {

class DeterministicRandom final {
public:
    explicit DeterministicRandom(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        auto value = (state_ += 0x9E37'79B9'7F4A'7C15ull);
        value = (value ^ (value >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D0'49BB'1331'11EBull;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_{};
};

Operation make_operation(DeterministicRandom& random,
                         std::size_t index,
                         std::uint64_t logical_size_bytes) {
    Operation operation{};
    operation.kind = static_cast<OperationKind>(
        index < 7 ? index : random.next() % 7);
    operation.payload_seed = random.next();
    operation.max_payload_bytes =
        (1 + random.next() % 4) * kBlockBytes;
    operation.record_limit =
        static_cast<std::size_t>(1 + random.next() % 8);

    const auto total_sectors = logical_size_bytes / kSectorBytes;
    const auto mode = random.next() % 6;
    std::uint64_t first_sector = random.next() % total_sectors;
    std::uint64_t sector_count = 1;
    if (mode == 0) {
        first_sector =
            (random.next() % (logical_size_bytes / kBlockBytes)) *
            (kBlockBytes / kSectorBytes);
        sector_count = kBlockBytes / kSectorBytes;
    } else if (mode == 1 && total_sectors > 2) {
        first_sector = random.next() % (total_sectors - 1);
        sector_count = 2;
    } else if (mode == 2) {
        first_sector = total_sectors - 1;
    } else {
        const auto remaining = total_sectors - first_sector;
        sector_count = 1 + random.next() % (std::min)(remaining, 16ull);
    }
    operation.offset = first_sector * kSectorBytes;
    operation.length = sector_count * kSectorBytes;
    return operation;
}

const char* operation_name(OperationKind kind) noexcept {
    switch (kind) {
    case OperationKind::Write:
        return "write";
    case OperationKind::Zero:
        return "zero";
    case OperationKind::Discard:
        return "discard";
    case OperationKind::Begin:
        return "begin";
    case OperationKind::Copy:
        return "copy";
    case OperationKind::Finish:
        return "finish";
    case OperationKind::Abort:
        return "abort";
    }
    return "unknown";
}

} // namespace

Schedule generate_schedule(std::uint64_t seed,
                           std::size_t operation_count) {
    DeterministicRandom random(seed);
    Schedule schedule{};
    schedule.seed = seed;
    schedule.block_count = 2 + random.next() % 63;
    schedule.operations.reserve(operation_count);
    for (std::size_t index = 0; index < operation_count; ++index) {
        schedule.operations.push_back(make_operation(
            random, index, schedule.logical_size_bytes()));
    }
    return schedule;
}

std::vector<std::byte> operation_payload(const Operation& operation) {
    if (operation.kind != OperationKind::Write) {
        return {};
    }
    std::vector<std::byte> payload(
        static_cast<std::size_t>(operation.length));
    auto state = operation.payload_seed;
    for (auto& value : payload) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        value = std::byte{static_cast<std::uint8_t>(
            (state * 0x2545'F491'4F6C'DD1Dull) >> 56)};
    }
    return payload;
}

std::string describe_operation(std::size_t index,
                               const Operation& operation) {
    std::ostringstream stream;
    stream << "index=" << index
           << " kind=" << operation_name(operation.kind)
           << " offset=" << operation.offset
           << " length=" << operation.length
           << " payload_seed=0x" << std::hex << operation.payload_seed
           << std::dec
           << " max_payload=" << operation.max_payload_bytes
           << " record_limit=" << operation.record_limit;
    return stream.str();
}

std::optional<Schedule> reduce_failure(
    const Schedule& schedule,
    const FailurePredicate& still_fails) {
    if (!still_fails || !still_fails(schedule)) {
        return std::nullopt;
    }
    Schedule reduced = schedule;
    if (reduced.operations.empty()) {
        return reduced;
    }

    std::size_t low = 1;
    std::size_t high = reduced.operations.size();
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        auto candidate = reduced;
        candidate.operations.resize(middle);
        if (still_fails(candidate)) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    reduced.operations.resize(low);

    auto& boundary = reduced.operations.back();
    const bool has_range =
        boundary.kind == OperationKind::Write ||
        boundary.kind == OperationKind::Zero ||
        boundary.kind == OperationKind::Discard;
    if (has_range && boundary.length > kSectorBytes &&
        boundary.length % kSectorBytes == 0) {
        std::uint64_t low_sectors = 1;
        std::uint64_t high_sectors = boundary.length / kSectorBytes;
        while (low_sectors < high_sectors) {
            const auto middle =
                low_sectors + (high_sectors - low_sectors) / 2;
            auto candidate = reduced;
            candidate.operations.back().length = middle * kSectorBytes;
            if (still_fails(candidate)) {
                high_sectors = middle;
            } else {
                low_sectors = middle + 1;
            }
        }
        auto candidate = reduced;
        candidate.operations.back().length = low_sectors * kSectorBytes;
        if (still_fails(candidate)) {
            reduced = std::move(candidate);
        }
    }
    return reduced;
}

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
        if (!whole_block && state.dirty && state.kind != Kind::Data) {
            continue;
        }
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

std::optional<std::vector<SnapshotRecord>> Model::copy_cut(
    CutToken token,
    std::uint64_t& cursor,
    std::uint64_t max_payload_bytes,
    std::size_t record_limit) const {
    if (active_cut_ != token || cursor > block_count_ ||
        max_payload_bytes < kBlockBytes ||
        max_payload_bytes % kBlockBytes != 0 || record_limit == 0) {
        return std::nullopt;
    }

    std::vector<SnapshotRecord> batch;
    std::uint64_t payload_bytes = 0;
    while (cursor < block_count_ && batch.size() < record_limit) {
        const auto found = std::lower_bound(
            cut_records_.begin(),
            cut_records_.end(),
            cursor,
            [](const SnapshotRecord& record, std::uint64_t block_index) {
                return record.block_index < block_index;
            });
        if (found == cut_records_.end()) {
            cursor = block_count_;
            break;
        }
        if (found->block_index > cursor) {
            cursor = found->block_index;
        }
        if (found->kind == Kind::Data &&
            max_payload_bytes - payload_bytes < kBlockBytes) {
            break;
        }
        batch.push_back(*found);
        if (found->kind == Kind::Data) {
            payload_bytes += kBlockBytes;
        }
        ++cursor;
    }
    return batch;
}

bool Model::finish_cut(CutToken token, std::uint64_t cursor) noexcept {
    if (active_cut_ != token || cursor != block_count_) {
        return false;
    }
    active_cut_.reset();
    cut_records_.clear();
    return true;
}

bool Model::abort_cut(CutToken token) noexcept {
    if (active_cut_ != token) {
        return false;
    }
    active_cut_.reset();
    cut_records_.clear();
    return true;
}

} // namespace myrd::test::consistent_model
