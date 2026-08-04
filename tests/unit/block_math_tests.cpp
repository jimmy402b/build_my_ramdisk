#include "test_framework.h"

#include "src/storage/block_constants.h"
#include "src/storage/block_math.h"
#include "src/storage/dynamic_block_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {

constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTiB = 1024ULL * kGiB;

myrd::DynamicBlockStoreOptions valid_options() {
    return myrd::DynamicBlockStoreOptions{
        2 * myrd::kBlockSizeBytes,
        3 * myrd::kBlockSizeBytes,
        myrd::kBlockSizeBytes,
        2 * myrd::kBlockSizeBytes,
        8,
        64,
        myrd::kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250},
    };
}

myrd::DynamicBlockStoreCreateArgs clean_zero_args() {
    return myrd::DynamicBlockStoreCreateArgs{
        2 * myrd::kBlockSizeBytes,
        512,
        valid_options(),
        myrd::InitialBlockState::CleanZero,
        nullptr,
        0,
    };
}

} // namespace

TEST_CASE(m2_block_constants_are_exact_and_fixed_width) {
    static_assert(std::is_same_v<decltype(myrd::kBlockSizeBytes),
                                 const std::uint64_t>);
    static_assert(std::is_same_v<decltype(myrd::kBlocksPerLeaf),
                                 const std::uint64_t>);
    REQUIRE_EQ(myrd::kBlockSizeBytes, 65'536ULL);
    REQUIRE_EQ(myrd::kBlocksPerLeaf, 1'024ULL);
    REQUIRE_EQ(myrd::kLeafSizeBytes, 64ULL * 1024ULL * 1024ULL);
}

TEST_CASE(m2_supported_sector_size_contract_is_exact_and_noexcept) {
    static_assert(noexcept(myrd::is_supported_sector_size(512)));
    static_assert(myrd::is_supported_sector_size(512));
    static_assert(myrd::is_supported_sector_size(4'096));
    static_assert(!myrd::is_supported_sector_size(0));
    static_assert(!myrd::is_supported_sector_size(1));
    static_assert(!myrd::is_supported_sector_size(1'024));
    static_assert(!myrd::is_supported_sector_size(65'536));
}

TEST_CASE(m2_checked_add_and_multiply_reject_overflow) {
    const auto sum = myrd::checked_add_u64(17, 25);
    REQUIRE(sum.ok());
    REQUIRE_EQ(sum.value, 42ULL);

    const auto add_overflow = myrd::checked_add_u64(
        std::numeric_limits<std::uint64_t>::max(), 1);
    REQUIRE(!add_overflow.ok());
    REQUIRE_EQ(add_overflow.error, myrd::BlockMathError::Overflow);

    const auto product = myrd::checked_multiply_u64(1'024, 64 * 1'024);
    REQUIRE(product.ok());
    REQUIRE_EQ(product.value, myrd::kLeafSizeBytes);

    const auto multiply_overflow = myrd::checked_multiply_u64(
        std::numeric_limits<std::uint64_t>::max(), 2);
    REQUIRE(!multiply_overflow.ok());
    REQUIRE_EQ(multiply_overflow.error, myrd::BlockMathError::Overflow);
}

TEST_CASE(m2_checked_round_up_handles_zero_alignment_and_overflow) {
    const auto rounded = myrd::checked_round_up_u64(65'537, myrd::kBlockSizeBytes);
    REQUIRE(rounded.ok());
    REQUIRE_EQ(rounded.value, 2 * myrd::kBlockSizeBytes);

    const auto aligned = myrd::checked_round_up_u64(
        2 * myrd::kBlockSizeBytes, myrd::kBlockSizeBytes);
    REQUIRE(aligned.ok());
    REQUIRE_EQ(aligned.value, 2 * myrd::kBlockSizeBytes);

    const auto zero = myrd::checked_round_up_u64(0, myrd::kBlockSizeBytes);
    REQUIRE(zero.ok());
    REQUIRE_EQ(zero.value, 0ULL);

    const auto invalid_alignment = myrd::checked_round_up_u64(1, 0);
    REQUIRE(!invalid_alignment.ok());
    REQUIRE_EQ(invalid_alignment.error, myrd::BlockMathError::InvalidAlignment);

    const auto overflow = myrd::checked_round_up_u64(
        std::numeric_limits<std::uint64_t>::max(), myrd::kBlockSizeBytes);
    REQUIRE(!overflow.ok());
    REQUIRE_EQ(overflow.error, myrd::BlockMathError::Overflow);
}

TEST_CASE(m2_geometry_supports_64_gib_and_1_tib) {
    const auto geometry_64_gib = myrd::compute_block_geometry(64ULL * kGiB);
    REQUIRE(geometry_64_gib.ok());
    REQUIRE_EQ(geometry_64_gib.value.total_blocks, 1'048'576ULL);
    REQUIRE_EQ(geometry_64_gib.value.total_leaves, 1'024ULL);
    REQUIRE_EQ(geometry_64_gib.value.reservation_size_bytes, 64ULL * kGiB);
    REQUIRE_EQ(geometry_64_gib.value.last_block_size_bytes,
               myrd::kBlockSizeBytes);

    const auto geometry_1_tib = myrd::compute_block_geometry(kTiB);
    REQUIRE(geometry_1_tib.ok());
    REQUIRE_EQ(geometry_1_tib.value.total_blocks, 16'777'216ULL);
    REQUIRE_EQ(geometry_1_tib.value.total_leaves, 16'384ULL);
    REQUIRE_EQ(geometry_1_tib.value.reservation_size_bytes, kTiB);
}

TEST_CASE(m2_geometry_rounds_reservation_and_preserves_partial_last_block) {
    const auto geometry = myrd::compute_block_geometry(
        myrd::kBlockSizeBytes + 512);
    REQUIRE(geometry.ok());
    REQUIRE_EQ(geometry.value.logical_size_bytes,
               myrd::kBlockSizeBytes + 512);
    REQUIRE_EQ(geometry.value.reservation_size_bytes,
               2 * myrd::kBlockSizeBytes);
    REQUIRE_EQ(geometry.value.total_blocks, 2ULL);
    REQUIRE_EQ(geometry.value.total_leaves, 1ULL);
    REQUIRE_EQ(geometry.value.last_block_size_bytes, 512ULL);

    REQUIRE(!myrd::compute_block_geometry(0).ok());
    REQUIRE(!myrd::compute_block_geometry(
        std::numeric_limits<std::uint64_t>::max()).ok());
}

TEST_CASE(m2_range_split_descriptor_is_noexcept_and_allocation_free) {
    static_assert(noexcept(myrd::validate_and_split_block_range(
        65'536, 512, 0, 512, 512)));
    static_assert(std::is_trivially_copyable_v<myrd::BlockRangeSplitResult>);
    static_assert(std::is_standard_layout_v<myrd::BlockRangeSplitResult>);

    const auto descriptor = myrd::validate_and_split_block_range(
        65'536, 512, 0, 512, 512);
    static_assert(noexcept(descriptor.segment_at(0)));
    REQUIRE(descriptor.ok());
    REQUIRE_EQ(descriptor.segment_count, 1ULL);
}

TEST_CASE(m2_range_split_handles_within_two_and_three_blocks) {
    const auto within = myrd::validate_and_split_block_range(
        3 * myrd::kBlockSizeBytes, 512, 4'096, 1'024, 1'024);
    REQUIRE(within.ok());
    REQUIRE_EQ(within.segment_count, 1ULL);
    const auto within_segment = within.segment_at(0);
    REQUIRE(within_segment.ok());
    REQUIRE_EQ(within_segment.value.block_index, 0ULL);
    REQUIRE_EQ(within_segment.value.block_offset_bytes, 4'096ULL);
    REQUIRE_EQ(within_segment.value.length_bytes, 1'024ULL);
    REQUIRE_EQ(within_segment.value.buffer_offset_bytes, 0ULL);

    const auto two = myrd::validate_and_split_block_range(
        3 * myrd::kBlockSizeBytes, 512,
        myrd::kBlockSizeBytes - 512, 1'024, 1'024);
    REQUIRE(two.ok());
    REQUIRE_EQ(two.segment_count, 2ULL);
    const auto two_first = two.segment_at(0);
    const auto two_second = two.segment_at(1);
    REQUIRE(two_first.ok());
    REQUIRE(two_second.ok());
    REQUIRE_EQ(two_first.value.length_bytes, 512ULL);
    REQUIRE_EQ(two_second.value.block_index, 1ULL);
    REQUIRE_EQ(two_second.value.block_offset_bytes, 0ULL);
    REQUIRE_EQ(two_second.value.buffer_offset_bytes, 512ULL);

    const std::uint64_t three_length = 1'024 + myrd::kBlockSizeBytes + 2'048;
    const auto three = myrd::validate_and_split_block_range(
        3 * myrd::kBlockSizeBytes, 512,
        myrd::kBlockSizeBytes - 1'024, three_length, three_length);
    REQUIRE(three.ok());
    REQUIRE_EQ(three.segment_count, 3ULL);
    const auto three_first = three.segment_at(0);
    const auto three_middle = three.segment_at(1);
    const auto three_last = three.segment_at(2);
    REQUIRE(three_first.ok());
    REQUIRE(three_middle.ok());
    REQUIRE(three_last.ok());
    REQUIRE_EQ(three_first.value.length_bytes, 1'024ULL);
    REQUIRE_EQ(three_middle.value.length_bytes, myrd::kBlockSizeBytes);
    REQUIRE_EQ(three_last.value.length_bytes, 2'048ULL);
    REQUIRE_EQ(three_last.value.buffer_offset_bytes,
               1'024 + myrd::kBlockSizeBytes);
}

TEST_CASE(m2_range_split_supports_partial_logical_last_block) {
    const auto split = myrd::validate_and_split_block_range(
        myrd::kBlockSizeBytes + 512, 512,
        myrd::kBlockSizeBytes - 512, 1'024, 1'024);
    REQUIRE(split.ok());
    REQUIRE_EQ(split.segment_count, 2ULL);
    const auto last = split.segment_at(1);
    REQUIRE(last.ok());
    REQUIRE_EQ(last.value.block_index, 1ULL);
    REQUIRE_EQ(last.value.length_bytes, 512ULL);
}

TEST_CASE(m2_range_split_describes_full_1_tib_without_allocation) {
    const auto split = myrd::validate_and_split_block_range(
        kTiB, 512, 0, kTiB, kTiB);
    REQUIRE(split.ok());
    REQUIRE_EQ(split.segment_count, 16'777'216ULL);

    const auto first = split.segment_at(0);
    REQUIRE(first.ok());
    REQUIRE_EQ(first.value.block_index, 0ULL);
    REQUIRE_EQ(first.value.block_offset_bytes, 0ULL);
    REQUIRE_EQ(first.value.length_bytes, myrd::kBlockSizeBytes);
    REQUIRE_EQ(first.value.buffer_offset_bytes, 0ULL);

    const std::uint64_t middle_index = split.segment_count / 2;
    const auto middle = split.segment_at(middle_index);
    REQUIRE(middle.ok());
    REQUIRE_EQ(middle.value.block_index, middle_index);
    REQUIRE_EQ(middle.value.block_offset_bytes, 0ULL);
    REQUIRE_EQ(middle.value.length_bytes, myrd::kBlockSizeBytes);
    REQUIRE_EQ(middle.value.buffer_offset_bytes,
               middle_index * myrd::kBlockSizeBytes);

    const std::uint64_t last_index = split.segment_count - 1;
    const auto last = split.segment_at(last_index);
    REQUIRE(last.ok());
    REQUIRE_EQ(last.value.block_index, last_index);
    REQUIRE_EQ(last.value.block_offset_bytes, 0ULL);
    REQUIRE_EQ(last.value.length_bytes, myrd::kBlockSizeBytes);
    REQUIRE_EQ(last.value.buffer_offset_bytes,
               last_index * myrd::kBlockSizeBytes);

    const auto past_end = split.segment_at(split.segment_count);
    REQUIRE(!past_end.ok());
    REQUIRE_EQ(past_end.error,
               myrd::BlockMathError::SegmentIndexOutOfRange);
    REQUIRE_EQ(myrd::to_string(past_end.error),
               std::string_view("SegmentIndexOutOfRange"));
}

TEST_CASE(m2_range_validation_rejects_full_request_before_splitting) {
    const auto zero = myrd::validate_and_split_block_range(65'536, 512, 0, 0, 0);
    REQUIRE_EQ(zero.error, myrd::BlockMathError::ZeroLength);
    REQUIRE_EQ(zero.segment_count, 0ULL);

    const auto misaligned = myrd::validate_and_split_block_range(
        65'536, 512, 1, 512, 512);
    REQUIRE_EQ(misaligned.error, myrd::BlockMathError::Misaligned);
    REQUIRE_EQ(misaligned.segment_count, 0ULL);

    const auto overflow = myrd::validate_and_split_block_range(
        std::numeric_limits<std::uint64_t>::max() - 511, 512,
        std::numeric_limits<std::uint64_t>::max() - 511, 1'024, 1'024);
    REQUIRE_EQ(overflow.error, myrd::BlockMathError::Overflow);
    REQUIRE_EQ(overflow.segment_count, 0ULL);

    const auto out_of_range = myrd::validate_and_split_block_range(
        65'536, 512, 65'024, 1'024, 1'024);
    REQUIRE_EQ(out_of_range.error, myrd::BlockMathError::OutOfRange);
    REQUIRE_EQ(out_of_range.segment_count, 0ULL);

    const auto wrong_buffer = myrd::validate_and_split_block_range(
        65'536, 512, 0, 512, 256);
    REQUIRE_EQ(wrong_buffer.error, myrd::BlockMathError::BufferLengthMismatch);
    REQUIRE_EQ(wrong_buffer.segment_count, 0ULL);
}

TEST_CASE(m2_range_validation_rejects_sector_misaligned_length) {
    const auto result = myrd::validate_and_split_block_range(
        65'536, 512, 512, 768, 768);
    REQUIRE_EQ(result.error, myrd::BlockMathError::Misaligned);
    REQUIRE_EQ(result.segment_count, 0ULL);
}

TEST_CASE(m2_range_validation_rejects_unsupported_nonzero_sector_sizes) {
    const auto sector_1024 = myrd::validate_and_split_block_range(
        65'536, 1'024, 0, 1'024, 1'024);
    REQUIRE_EQ(sector_1024.error, myrd::BlockMathError::InvalidSectorSize);
    REQUIRE_EQ(sector_1024.segment_count, 0ULL);

    const auto sector_1 = myrd::validate_and_split_block_range(
        65'536, 1, 0, 512, 512);
    REQUIRE_EQ(sector_1.error, myrd::BlockMathError::InvalidSectorSize);
    REQUIRE_EQ(sector_1.segment_count, 0ULL);

    const auto sector_65536 = myrd::validate_and_split_block_range(
        65'536, 65'536, 0, 65'536, 65'536);
    REQUIRE_EQ(sector_65536.error, myrd::BlockMathError::InvalidSectorSize);
    REQUIRE_EQ(sector_65536.segment_count, 0ULL);
}

TEST_CASE(m2_range_validation_accepts_512_and_4096_sector_sizes) {
    const auto sector_512 = myrd::validate_and_split_block_range(
        65'536, 512, 0, 512, 512);
    REQUIRE(sector_512.ok());
    REQUIRE_EQ(sector_512.segment_count, 1ULL);

    const auto sector_4096 = myrd::validate_and_split_block_range(
        65'536, 4'096, 0, 4'096, 4'096);
    REQUIRE(sector_4096.ok());
    REQUIRE_EQ(sector_4096.segment_count, 1ULL);
}

TEST_CASE(m2_options_require_order_alignment_snapshot_limits_and_stripes) {
    auto options = valid_options();
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());

    options.eviction_low_watermark_bytes = 0;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::InvalidLimitOrder);
    options = valid_options();
    options.eviction_low_watermark_bytes = options.soft_limit_bytes + myrd::kBlockSizeBytes;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::InvalidLimitOrder);
    options = valid_options();
    options.soft_limit_bytes = options.hard_limit_bytes;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::InvalidLimitOrder);

    options = valid_options();
    options.soft_limit_bytes += 512;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::LimitNotBlockAligned);
    options = valid_options();
    options.max_snapshot_batch_bytes = myrd::kBlockSizeBytes - 512;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::SnapshotBytesTooSmall);
    options = valid_options();
    options.max_snapshot_batch_bytes += 512;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::SnapshotBytesNotBlockAligned);
    options = valid_options();
    options.max_snapshot_batch_records = 0;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::SnapshotRecordsZero);
    options = valid_options();
    options.lock_stripe_count = 63;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::StripeCountTooSmall);
    options = valid_options();
    options.lock_stripe_count = 96;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::StripeCountNotPowerOfTwo);

    options = valid_options();
    options.eviction_low_watermark_bytes = options.soft_limit_bytes;
    REQUIRE(myrd::validate_dynamic_block_store_options(options).ok());
}

TEST_CASE(m2_options_reject_soft_limit_greater_than_hard_limit) {
    auto options = valid_options();
    options.soft_limit_bytes =
        options.hard_limit_bytes + myrd::kBlockSizeBytes;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::InvalidLimitOrder);
}

TEST_CASE(m2_options_reject_misaligned_low_watermark) {
    auto options = valid_options();
    options.eviction_low_watermark_bytes += 512;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::LimitNotBlockAligned);
}

TEST_CASE(m2_options_reject_misaligned_hard_limit) {
    auto options = valid_options();
    options.hard_limit_bytes += 512;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_options(options).error,
               myrd::DynamicBlockStoreOptionsError::LimitNotBlockAligned);
}

TEST_CASE(m2_create_validation_identifies_backing_normalization) {
    auto args = clean_zero_args();
    const auto clean_zero = myrd::validate_dynamic_block_store_create_args(args);
    REQUIRE(clean_zero.ok());
    REQUIRE_EQ(clean_zero.backing_normalization,
               myrd::BackingNormalization::UseZeroBackingStore);
    REQUIRE(clean_zero.requires_zero_backing_store());

    args.initial_state = myrd::InitialBlockState::BackingUnresident;
    const auto missing_backing = myrd::validate_dynamic_block_store_create_args(args);
    REQUIRE(!missing_backing.ok());
    REQUIRE_EQ(missing_backing.error,
               myrd::DynamicBlockStoreCreateError::BackingStoreRequired);

    args = clean_zero_args();
    args.logical_size_bytes = 0;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_create_args(args).error,
               myrd::DynamicBlockStoreCreateError::InvalidLogicalSize);
    args = clean_zero_args();
    args.sector_size_bytes = 0;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_create_args(args).error,
               myrd::DynamicBlockStoreCreateError::InvalidSectorSize);
    args = clean_zero_args();
    args.logical_size_bytes += 1;
    REQUIRE_EQ(myrd::validate_dynamic_block_store_create_args(args).error,
               myrd::DynamicBlockStoreCreateError::LogicalSizeNotSectorAligned);
}

TEST_CASE(m2_create_validation_rejects_unsupported_nonzero_sector_size) {
    auto args = clean_zero_args();
    args.sector_size_bytes = 1'024;
    const auto result = myrd::validate_dynamic_block_store_create_args(args);
    REQUIRE_EQ(result.error,
               myrd::DynamicBlockStoreCreateError::InvalidSectorSize);
}

TEST_CASE(m2_create_validation_accepts_512_and_4096_sector_sizes) {
    auto sector_512 = clean_zero_args();
    sector_512.sector_size_bytes = 512;
    REQUIRE(myrd::validate_dynamic_block_store_create_args(sector_512).ok());

    auto sector_4096 = clean_zero_args();
    sector_4096.sector_size_bytes = 4'096;
    REQUIRE(myrd::validate_dynamic_block_store_create_args(sector_4096).ok());
}

TEST_CASE(m2_public_states_errors_and_records_have_stable_names) {
    REQUIRE_EQ(myrd::to_string(myrd::InitialBlockState::CleanZero),
               std::string_view("CleanZero"));
    REQUIRE_EQ(myrd::to_string(myrd::InitialBlockState::BackingUnresident),
               std::string_view("BackingUnresident"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::Unresident), std::string_view("Unresident"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::ResidentClean), std::string_view("ResidentClean"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::ResidentDirty), std::string_view("ResidentDirty"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::ZeroClean), std::string_view("ZeroClean"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::ZeroDirty), std::string_view("ZeroDirty"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::Loading), std::string_view("Loading"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::Committing), std::string_view("Committing"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockState::Evicting), std::string_view("Evicting"));
    REQUIRE(myrd::is_stable_block_state(myrd::BlockState::ResidentDirty));
    REQUIRE(!myrd::is_stable_block_state(myrd::BlockState::Loading));
    REQUIRE(myrd::is_transient_block_state(myrd::BlockState::Loading));
    REQUIRE(!myrd::is_transient_block_state(myrd::BlockState::ZeroClean));
    REQUIRE_EQ(myrd::to_string(myrd::DirtyRecordKind::Data), std::string_view("Data"));
    REQUIRE_EQ(myrd::to_string(myrd::DirtyRecordKind::Zero), std::string_view("Zero"));
    REQUIRE_EQ(myrd::to_string(myrd::DirtyRecordKind::Discard), std::string_view("Discard"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreErrorCode::BackingGenerationConflict),
               std::string_view("BackingGenerationConflict"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreErrorCode::FatalInternalInvariant),
               std::string_view("FatalInternalInvariant"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreOptionsError::InvalidLimitOrder),
               std::string_view("InvalidLimitOrder"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreOptionsError::LimitNotBlockAligned),
               std::string_view("LimitNotBlockAligned"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreOptionsError::SnapshotBytesTooSmall),
               std::string_view("SnapshotBytesTooSmall"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreOptionsError::SnapshotBytesNotBlockAligned),
               std::string_view("SnapshotBytesNotBlockAligned"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreOptionsError::SnapshotRecordsZero),
               std::string_view("SnapshotRecordsZero"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreOptionsError::StripeCountTooSmall),
               std::string_view("StripeCountTooSmall"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreOptionsError::StripeCountNotPowerOfTwo),
               std::string_view("StripeCountNotPowerOfTwo"));
    REQUIRE_EQ(myrd::to_string(myrd::BackingNormalization::UseZeroBackingStore),
               std::string_view("UseZeroBackingStore"));
    REQUIRE_EQ(myrd::to_string(myrd::DynamicBlockStoreCreateError::BackingStoreRequired),
               std::string_view("BackingStoreRequired"));
    REQUIRE_EQ(myrd::to_string(myrd::BlockMathError::DivergentBlocksExceedTotal),
               std::string_view("DivergentBlocksExceedTotal"));
}

TEST_CASE(m2_generation_and_version_increment_never_wrap) {
    const auto version = myrd::checked_increment_version(41);
    REQUIRE(version.ok());
    REQUIRE_EQ(version.value, 42ULL);

    const auto generation = myrd::checked_increment_backing_generation(99);
    REQUIRE(generation.ok());
    REQUIRE_EQ(generation.value, 100ULL);

    const auto version_wrap = myrd::checked_increment_version(
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(!version_wrap.ok());
    REQUIRE_EQ(version_wrap.value, std::numeric_limits<std::uint64_t>::max());
    REQUIRE_EQ(version_wrap.error,
               myrd::DynamicBlockStoreErrorCode::FatalInternalInvariant);

    const auto generation_wrap = myrd::checked_increment_backing_generation(
        std::numeric_limits<myrd::BackingGeneration>::max());
    REQUIRE(!generation_wrap.ok());
    REQUIRE_EQ(generation_wrap.value,
               std::numeric_limits<myrd::BackingGeneration>::max());
}

TEST_CASE(m2_dirty_snapshot_cursor_and_stats_are_copyable_values) {
    static_assert(std::is_same_v<myrd::BackingGeneration, std::uint64_t>);
    static_assert(std::is_copy_constructible_v<myrd::DynamicBlockStoreStats>);
    static_assert(std::is_copy_assignable_v<myrd::DynamicBlockStoreStats>);

    myrd::DirtyBlockSnapshot snapshot{};
    snapshot.block_index = 7;
    snapshot.version = 9;
    snapshot.kind = myrd::DirtyRecordKind::Data;
    snapshot.base_generation = 5;
    snapshot.data.resize(static_cast<std::size_t>(myrd::kBlockSizeBytes));
    REQUIRE_EQ(snapshot.base_generation, 5ULL);
    REQUIRE_EQ(snapshot.data.size(), static_cast<std::size_t>(myrd::kBlockSizeBytes));

    myrd::DirtyScanCursor cursor{};
    REQUIRE_EQ(cursor.next_block_index, 0ULL);

    myrd::DynamicBlockStoreStats stats{};
    stats.logical_size_bytes = 1;
    stats.reserved_bytes = 2;
    stats.committed_bytes = 3;
    stats.resident_clean_blocks = 4;
    stats.resident_dirty_blocks = 5;
    stats.zero_clean_blocks = 6;
    stats.zero_dirty_blocks = 7;
    stats.loading_blocks = 8;
    stats.evicted_blocks_total = 9;
    stats.backing_reads_total = 10;
    stats.backing_read_bytes_total = 11;
    stats.uncached_reads_total = 12;
    stats.unmap_bytes_total = 13;
    stats.zero_bytes_total = 14;
    stats.hard_limit_rejections = 15;
    stats.commit_failures = 16;
    stats.decommit_failures = 17;
    stats.snapshot_records_total = 18;
    stats.snapshot_version_conflicts = 19;
    stats.backing_generation = 20;
    stats.backing_view_installs_total = 21;
    stats.backing_generation_conflicts = 22;
    stats.stale_backing_load_retries = 23;
    const auto copy = stats;
    REQUIRE_EQ(copy.stale_backing_load_retries, 23ULL);
    REQUIRE_EQ(copy.backing_generation, 20ULL);
}

TEST_CASE(m2_implicit_clean_zero_count_never_allocates_and_rejects_underflow) {
    const auto count = myrd::implicit_clean_zero_block_count(1'000, 37);
    REQUIRE(count.ok());
    REQUIRE_EQ(count.value, 963ULL);

    const auto empty = myrd::implicit_clean_zero_block_count(0, 0);
    REQUIRE(empty.ok());
    REQUIRE_EQ(empty.value, 0ULL);

    const auto invalid = myrd::implicit_clean_zero_block_count(10, 11);
    REQUIRE(!invalid.ok());
    REQUIRE_EQ(invalid.error,
               myrd::BlockMathError::DivergentBlocksExceedTotal);
}
