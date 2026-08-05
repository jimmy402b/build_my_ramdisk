#include "test_framework.h"

#include "consistent_snapshot_model.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

std::vector<std::byte> block_bytes(std::uint8_t value) {
    return std::vector<std::byte>(
        static_cast<std::size_t>(
            myrd::test::consistent_model::kBlockBytes),
        std::byte{value});
}

} // namespace

TEST_CASE(m2_1_task08_model_restates_geometry_and_starts_clean) {
    myrd::test::consistent_model::Model model(
        2 * myrd::test::consistent_model::kBlockBytes,
        7);

    REQUIRE_EQ(model.logical_size_bytes(),
               2 * myrd::test::consistent_model::kBlockBytes);
    REQUIRE_EQ(model.block_count(), 2ull);
    REQUIRE_EQ(model.backing_generation(), 7ull);
    REQUIRE_EQ(model.dirty_block_count(), 0ull);
    REQUIRE(!model.active_cut().has_value());
}

TEST_CASE(m2_1_task08_begin_copies_one_immutable_whole_disk_cut) {
    using myrd::test::consistent_model::Kind;
    using myrd::test::consistent_model::Model;
    constexpr auto block = myrd::test::consistent_model::kBlockBytes;
    Model model(4 * block, 7);

    const auto old_data = block_bytes(0x31);
    REQUIRE(model.write(0, old_data));
    REQUIRE(model.zero(block, block, Kind::Zero));
    REQUIRE(model.zero(2 * block, block, Kind::Discard));

    const auto begun = model.begin_cut();
    REQUIRE(begun.has_value());
    REQUIRE_EQ(model.active_cut(), begun);

    const auto replacement = block_bytes(0x42);
    REQUIRE(model.write(0, replacement));
    REQUIRE(model.write(3 * block, block_bytes(0x53)));

    const auto* cut = model.cut_records(*begun);
    REQUIRE(cut != nullptr);
    REQUIRE_EQ(cut->size(), 3ull);
    REQUIRE_EQ((*cut)[0].block_index, 0ull);
    REQUIRE_EQ((*cut)[0].version, 1ull);
    REQUIRE_EQ((*cut)[0].kind, Kind::Data);
    REQUIRE_EQ((*cut)[0].base_generation, 7ull);
    REQUIRE_EQ((*cut)[0].data, old_data);
    REQUIRE_EQ((*cut)[1].block_index, 1ull);
    REQUIRE_EQ((*cut)[1].kind, Kind::Zero);
    REQUIRE((*cut)[1].data.empty());
    REQUIRE_EQ((*cut)[2].block_index, 2ull);
    REQUIRE_EQ((*cut)[2].kind, Kind::Discard);
    REQUIRE((*cut)[2].data.empty());
    REQUIRE_EQ(model.dirty_block_count(), 4ull);
}
