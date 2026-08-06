#include "test_framework.h"

#include "src/common/crc32c.h"
#include "src/common/sha256.h"
#include "src/persistence/live_extent_enumerator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace myrd::persistence {
namespace {

class ZeroJournal final : public detail::ICompositeJournalLayer {
public:
    Result<detail::CompositeJournalRead> read_block(
        const OperationContext&, std::uint64_t,
        std::span<std::byte>) const override {
        return Result<detail::CompositeJournalRead>::success(
            {detail::CompositeJournalKind::Absent});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext&, std::uint64_t) const override {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
};

class ZeroBase final : public detail::ICompositeBaseLayer {
public:
    Result<detail::CompositeBaseRead> read_block(
        const OperationContext&, std::uint64_t,
        std::span<std::byte>) const override {
        return Result<detail::CompositeBaseRead>::success(
            {detail::CompositeBaseKind::Zero});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext&, std::uint64_t) const override {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
};

using BlockBytes = std::array<std::byte, kM2BlockSize>;

struct JournalEntry final {
    detail::CompositeJournalKind kind{
        detail::CompositeJournalKind::Absent};
    BlockBytes bytes{};
};

struct BaseEntry final {
    detail::CompositeBaseKind kind{detail::CompositeBaseKind::Absent};
    BlockBytes bytes{};
};

struct JournalState final {
    std::map<std::uint64_t, JournalEntry> entries;
    std::optional<Error> failure;
    std::stop_source* stop_after_read{};
    std::chrono::milliseconds read_delay{};
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> candidate_calls{};
    std::atomic<bool> layer_destroyed{};
};

struct BaseState final {
    std::map<std::uint64_t, BaseEntry> entries;
    std::optional<Error> failure;
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> candidate_calls{};
    std::atomic<bool> layer_destroyed{};
};

class MapJournal final : public detail::ICompositeJournalLayer {
public:
    explicit MapJournal(std::shared_ptr<JournalState> state)
        : state_(std::move(state)) {}

    ~MapJournal() override {
        state_->layer_destroyed.store(true, std::memory_order_release);
    }

    Result<detail::CompositeJournalRead> read_block(
        const OperationContext& context,
        const std::uint64_t block,
        const std::span<std::byte> destination) const override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<detail::CompositeJournalRead>::failure(
                live.error());
        }
        state_->calls.fetch_add(1, std::memory_order_relaxed);
        if (state_->read_delay.count() != 0) {
            std::this_thread::sleep_for(state_->read_delay);
        }
        if (state_->stop_after_read != nullptr) {
            state_->stop_after_read->request_stop();
        }
        if (state_->failure.has_value()) {
            return Result<detail::CompositeJournalRead>::failure(
                *state_->failure);
        }
        const auto found = state_->entries.find(block);
        if (found == state_->entries.end()) {
            return Result<detail::CompositeJournalRead>::success(
                {detail::CompositeJournalKind::Absent});
        }
        if (found->second.kind == detail::CompositeJournalKind::Data) {
            std::copy(found->second.bytes.begin(),
                      found->second.bytes.end(), destination.begin());
        }
        return Result<detail::CompositeJournalRead>::success(
            {found->second.kind});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        const std::uint64_t first_block) const override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                live.error());
        }
        state_->candidate_calls.fetch_add(1, std::memory_order_relaxed);
        for (auto found = state_->entries.lower_bound(first_block);
             found != state_->entries.end(); ++found) {
            if (found->second.kind !=
                detail::CompositeJournalKind::Absent) {
                return Result<std::optional<std::uint64_t>>::success(
                    found->first);
            }
        }
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }

private:
    std::shared_ptr<JournalState> state_;
};

class MapBase final : public detail::ICompositeBaseLayer {
public:
    explicit MapBase(std::shared_ptr<BaseState> state)
        : state_(std::move(state)) {}

    ~MapBase() override {
        state_->layer_destroyed.store(true, std::memory_order_release);
    }

    Result<detail::CompositeBaseRead> read_block(
        const OperationContext& context,
        const std::uint64_t block,
        const std::span<std::byte> destination) const override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<detail::CompositeBaseRead>::failure(live.error());
        }
        state_->calls.fetch_add(1, std::memory_order_relaxed);
        if (state_->failure.has_value()) {
            return Result<detail::CompositeBaseRead>::failure(
                *state_->failure);
        }
        const auto found = state_->entries.find(block);
        if (found == state_->entries.end()) {
            return Result<detail::CompositeBaseRead>::success(
                {detail::CompositeBaseKind::Absent});
        }
        if (found->second.kind == detail::CompositeBaseKind::Data) {
            std::copy(found->second.bytes.begin(),
                      found->second.bytes.end(), destination.begin());
        }
        return Result<detail::CompositeBaseRead>::success(
            {found->second.kind});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        const std::uint64_t first_block) const override {
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                live.error());
        }
        state_->candidate_calls.fetch_add(1, std::memory_order_relaxed);
        for (auto found = state_->entries.lower_bound(first_block);
             found != state_->entries.end(); ++found) {
            if (found->second.kind == detail::CompositeBaseKind::Data) {
                return Result<std::optional<std::uint64_t>>::success(
                    found->first);
            }
        }
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }

private:
    std::shared_ptr<BaseState> state_;
};

void set_base_data(BaseState& state, const std::uint64_t block,
                   const std::byte value) {
    auto& entry = state.entries[block];
    entry.kind = detail::CompositeBaseKind::Data;
    entry.bytes.fill(value);
}

void set_journal_data(JournalState& state, const std::uint64_t block,
                      const std::byte value) {
    auto& entry = state.entries[block];
    entry.kind = detail::CompositeJournalKind::Data;
    entry.bytes.fill(value);
}

CompositeBackingIdentity identity(
    const std::uint64_t virtual_bytes = 2 * kM2BlockSize,
    const std::uint64_t generation = 7,
    const std::byte image_tag = std::byte{0x34}) {
    CompositeBackingIdentity value{};
    value.image_uuid.bytes[0] = image_tag;
    value.geometry = {virtual_bytes, 512, 4096};
    value.checkpoint_generation = generation;
    value.base_generation = generation;
    value.base_windows_identity.volume_serial_number = 1;
    value.base_vhdx_identity.file_write_guid[0] = std::byte{1};
    value.base_vhdx_identity.data_write_guid[0] = std::byte{2};
    value.base_expected_length = 4096;
    value.journal_windows_identity.volume_serial_number = 2;
    value.journal_identity.journal_uuid[0] = std::byte{3};
    value.journal_epoch = 1;
    value.journal_root_generation = generation;
    value.journal_committed_length = 4096;
    value.commit.length = 4096;
    return value;
}

struct MapFixture final {
    CompositeBackingIdentity backing_identity;
    std::shared_ptr<JournalState> journal{
        std::make_shared<JournalState>()};
    std::shared_ptr<BaseState> base{std::make_shared<BaseState>()};
    std::shared_ptr<const CompositeBackingStore> store;

    explicit MapFixture(
        const std::uint64_t virtual_bytes,
        const std::uint64_t generation = 7,
        const std::byte image_tag = std::byte{0x34})
        : backing_identity(
              identity(virtual_bytes, generation, image_tag)) {}

    void build() {
        auto created = CompositeBackingStore::create_for_testing(
            backing_identity, std::make_unique<MapJournal>(journal),
            std::make_unique<MapBase>(base));
        REQUIRE(created.ok());
        store = std::shared_ptr<const CompositeBackingStore>(
            created.move_value());
    }

    Result<std::unique_ptr<LiveExtentEnumerator>> create_enumerator(
        const std::uint64_t max_extent_bytes = 2 * kM2BlockSize) const {
        return LiveExtentEnumerator::create(
            BackingView{backing_identity.checkpoint_generation, store},
            backing_identity, {max_extent_bytes});
    }
};

TEST_CASE(live_extent_enumerator_captures_an_immutable_zero_view) {
    const auto expected_identity = identity();
    auto composite = CompositeBackingStore::create_for_testing(
        expected_identity, std::make_unique<ZeroJournal>(),
        std::make_unique<ZeroBase>());
    REQUIRE(composite.ok());
    auto store = std::shared_ptr<const IBackingStore>(composite.move_value());
    auto enumerator = LiveExtentEnumerator::create(
        BackingView{7, std::move(store)}, expected_identity,
        {2 * kM2BlockSize});
    REQUIRE(enumerator.ok());
    std::uint64_t extent_count{};
    const auto result = enumerator.value()->enumerate(
        OperationContext{},
        [&extent_count](const LiveDataExtent&) {
            ++extent_count;
            return Result<void>::success();
        });
    REQUIRE(result.ok());
    REQUIRE_EQ(extent_count, 0ull);
    REQUIRE_EQ(result.value().candidate_blocks_examined, 0ull);
    REQUIRE_EQ(result.value().zero_blocks, 0ull);
}

TEST_CASE(live_extents_merge_shadowed_data_and_skip_zero_discard_holes) {
    constexpr std::uint64_t tail_bytes = 1024;
    constexpr std::uint64_t virtual_bytes =
        6 * kM2BlockSize + tail_bytes;
    MapFixture fixture(virtual_bytes);
    set_base_data(*fixture.base, 0, std::byte{0x10});
    set_base_data(*fixture.base, 1, std::byte{0x11});
    set_base_data(*fixture.base, 2, std::byte{0x12});
    set_base_data(*fixture.base, 3, std::byte{0x13});
    set_base_data(*fixture.base, 4, std::byte{0x14});
    set_base_data(*fixture.base, 5, std::byte{0x15});
    set_base_data(*fixture.base, 6, std::byte{0x16});
    set_journal_data(*fixture.journal, 1, std::byte{0x21});
    fixture.journal->entries[2].kind =
        detail::CompositeJournalKind::Zero;
    fixture.journal->entries[3].kind =
        detail::CompositeJournalKind::Discard;
    fixture.build();
    auto enumerator = fixture.create_enumerator();
    REQUIRE(enumerator.ok());

    std::vector<LiveDataExtent> extents;
    const auto result = enumerator.value()->enumerate(
        OperationContext{}, [&extents](const LiveDataExtent& extent) {
            extents.push_back(extent);
            return Result<void>::success();
        });
    REQUIRE(result.ok());
    REQUIRE_EQ(extents.size(), 3ull);
    REQUIRE_EQ(extents[0].logical_offset, 0ull);
    REQUIRE_EQ(extents[0].length, 2 * kM2BlockSize);
    REQUIRE_EQ(extents[1].logical_offset, 4 * kM2BlockSize);
    REQUIRE_EQ(extents[1].length, 2 * kM2BlockSize);
    REQUIRE_EQ(extents[2].logical_offset, 6 * kM2BlockSize);
    REQUIRE_EQ(extents[2].length, tail_bytes);
    REQUIRE_EQ(result.value().candidate_blocks_examined, 7ull);
    REQUIRE_EQ(result.value().data_blocks, 5ull);
    REQUIRE_EQ(result.value().zero_blocks, 2ull);
    REQUIRE_EQ(result.value().extents_emitted, 3ull);
    REQUIRE_EQ(result.value().bytes_hashed,
               4 * kM2BlockSize + tail_bytes);
    REQUIRE(result.value().peak_scratch_bytes < 2 * kM2BlockSize);
    REQUIRE_EQ(fixture.journal->calls.load(), 7ull);
    REQUIRE_EQ(fixture.base->calls.load(), 4ull);

    std::vector<std::byte> expected(virtual_bytes);
    const auto fill_expected = [&expected](
        const std::uint64_t block, const std::byte value,
        const std::uint64_t length = kM2BlockSize) {
        std::fill_n(
            expected.begin() +
                static_cast<std::ptrdiff_t>(block * kM2BlockSize),
            static_cast<std::ptrdiff_t>(length), value);
    };
    fill_expected(0, std::byte{0x10});
    fill_expected(1, std::byte{0x21});
    fill_expected(4, std::byte{0x14});
    fill_expected(5, std::byte{0x15});
    fill_expected(6, std::byte{0x16}, tail_bytes);
    for (const auto& extent : extents) {
        std::vector<std::byte> payload(
            static_cast<std::size_t>(extent.length));
        REQUIRE(enumerator.value()->read_verified_extent(
            OperationContext{}, extent, payload).ok());
        REQUIRE(std::equal(
            payload.begin(), payload.end(),
            expected.begin() +
                static_cast<std::ptrdiff_t>(extent.logical_offset)));
    }
}

class TestRandom final {
public:
    explicit TestRandom(const std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        state_ ^= state_ >> 12U;
        state_ ^= state_ << 25U;
        state_ ^= state_ >> 27U;
        return state_ * 2685821657736338717ULL;
    }

private:
    std::uint64_t state_;
};

TEST_CASE(live_extents_random_sparse_full_map_oracle_is_bounded) {
    constexpr std::uint64_t block_count = 64;
    constexpr std::uint64_t tail_bytes = 4096;
    constexpr std::uint64_t virtual_bytes =
        (block_count - 1) * kM2BlockSize + tail_bytes;
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        TestRandom random(0x34000000ULL + seed);
        MapFixture fixture(virtual_bytes, 20 + seed,
                           static_cast<std::byte>(seed));
        std::vector<std::byte> expected(virtual_bytes);
        std::uint64_t expected_data_blocks{};
        std::uint64_t expected_candidate_blocks{};
        std::uint64_t expected_base_calls{};
        for (std::uint64_t block = 0; block < block_count; ++block) {
            const auto valid_bytes = block + 1 == block_count
                ? tail_bytes
                : kM2BlockSize;
            std::byte resolved{};
            bool base_candidate = false;
            const auto base_kind = random.next() % 3;
            if (base_kind == 1) {
                const auto value = static_cast<std::byte>(
                    1U + random.next() % 251U);
                set_base_data(*fixture.base, block, value);
                resolved = value;
                base_candidate = true;
            } else if (base_kind == 2) {
                fixture.base->entries[block].kind =
                    detail::CompositeBaseKind::Zero;
            }

            const auto journal_kind = random.next() % 4;
            if (journal_kind == 0) {
                if (base_candidate) {
                    ++expected_candidate_blocks;
                    ++expected_base_calls;
                }
            } else if (journal_kind == 1) {
                ++expected_candidate_blocks;
                const auto value = seed % 5 == 0 && block % 7 == 0
                    ? std::byte{0}
                    : static_cast<std::byte>(1U + random.next() % 251U);
                set_journal_data(*fixture.journal, block, value);
                resolved = value;
            } else if (journal_kind == 2) {
                ++expected_candidate_blocks;
                fixture.journal->entries[block].kind =
                    detail::CompositeJournalKind::Zero;
                resolved = std::byte{0};
            } else {
                ++expected_candidate_blocks;
                fixture.journal->entries[block].kind =
                    detail::CompositeJournalKind::Discard;
                resolved = std::byte{0};
            }
            if (resolved != std::byte{0}) ++expected_data_blocks;
            std::fill_n(
                expected.begin() + static_cast<std::ptrdiff_t>(
                    block * kM2BlockSize),
                static_cast<std::ptrdiff_t>(valid_bytes), resolved);
        }
        fixture.build();
        auto enumerator = fixture.create_enumerator(4 * kM2BlockSize);
        REQUIRE(enumerator.ok());
        std::vector<LiveDataExtent> extents;
        const auto metrics = enumerator.value()->enumerate(
            OperationContext{}, [&extents](const LiveDataExtent& extent) {
                extents.push_back(extent);
                return Result<void>::success();
            });
        REQUIRE(metrics.ok());
        REQUIRE_EQ(metrics.value().candidate_blocks_examined,
                   expected_candidate_blocks);
        REQUIRE_EQ(metrics.value().data_blocks, expected_data_blocks);
        REQUIRE_EQ(metrics.value().zero_blocks,
                   expected_candidate_blocks - expected_data_blocks);
        REQUIRE_EQ(metrics.value().extents_emitted, extents.size());
        REQUIRE(metrics.value().peak_scratch_bytes < 2 * kM2BlockSize);
        REQUIRE_EQ(fixture.journal->calls.load(),
                   expected_candidate_blocks);
        REQUIRE_EQ(fixture.base->calls.load(), expected_base_calls);

        std::vector<LiveDataExtent> oracle_extents;
        std::uint64_t oracle_offset{};
        std::uint64_t oracle_length{};
        const auto flush_oracle = [&] {
            if (oracle_length == 0) return;
            const auto payload = std::span<const std::byte>{expected}.subspan(
                static_cast<std::size_t>(oracle_offset),
                static_cast<std::size_t>(oracle_length));
            auto digest = sha256_digest(payload);
            REQUIRE(digest.ok());
            oracle_extents.push_back({
                fixture.backing_identity.image_uuid,
                fixture.backing_identity.checkpoint_generation,
                fixture.backing_identity.checkpoint_generation,
                oracle_offset,
                oracle_length,
                crc32c(payload),
                digest.value()});
            oracle_length = 0;
        };
        for (std::uint64_t block = 0; block < block_count; ++block) {
            const auto offset = block * kM2BlockSize;
            const auto valid_bytes = (std::min)(
                kM2BlockSize, virtual_bytes - offset);
            const auto block_payload =
                std::span<const std::byte>{expected}.subspan(
                    static_cast<std::size_t>(offset),
                    static_cast<std::size_t>(valid_bytes));
            const bool nonzero = std::any_of(
                block_payload.begin(), block_payload.end(),
                [](const std::byte value) {
                    return value != std::byte{0};
                });
            if (!nonzero) {
                flush_oracle();
                continue;
            }
            if (oracle_length != 0 &&
                valid_bytes > 4 * kM2BlockSize - oracle_length) {
                flush_oracle();
            }
            if (oracle_length == 0) oracle_offset = offset;
            oracle_length += valid_bytes;
        }
        flush_oracle();
        REQUIRE(extents == oracle_extents);

        std::vector<std::byte> reconstructed(virtual_bytes);
        std::uint64_t previous_end{};
        for (const auto& extent : extents) {
            REQUIRE(extent.logical_offset >= previous_end);
            REQUIRE(extent.length <= 4 * kM2BlockSize);
            REQUIRE(extent.logical_offset + extent.length <= virtual_bytes);
            std::vector<std::byte> payload(
                static_cast<std::size_t>(extent.length));
            REQUIRE(enumerator.value()->read_verified_extent(
                OperationContext{}, extent, payload).ok());
            REQUIRE(std::any_of(
                payload.begin(), payload.end(), [](const std::byte value) {
                    return value != std::byte{0};
                }));
            std::copy(payload.begin(), payload.end(),
                      reconstructed.begin() + static_cast<std::ptrdiff_t>(
                          extent.logical_offset));
            previous_end = extent.logical_offset + extent.length;
        }
        REQUIRE(reconstructed == expected);
    }
}

TEST_CASE(live_extent_verified_read_rejects_identity_and_content_drift) {
    MapFixture fixture(2 * kM2BlockSize);
    set_base_data(*fixture.base, 0, std::byte{0x41});
    set_base_data(*fixture.base, 1, std::byte{0x42});
    fixture.build();
    auto enumerator = fixture.create_enumerator();
    REQUIRE(enumerator.ok());
    std::vector<LiveDataExtent> extents;
    REQUIRE(enumerator.value()->enumerate(
        OperationContext{}, [&extents](const LiveDataExtent& extent) {
            extents.push_back(extent);
            return Result<void>::success();
        }).ok());
    REQUIRE_EQ(extents.size(), 1ull);
    std::vector<std::byte> payload(2 * kM2BlockSize);

    auto wrong_identity = extents.front();
    ++wrong_identity.checkpoint_generation;
    REQUIRE(!enumerator.value()->read_verified_extent(
        OperationContext{}, wrong_identity, payload).ok());
    REQUIRE_EQ(fixture.base->calls.load(), 2ull);

    fixture.base->entries[1].bytes[17] ^= std::byte{1};
    const auto corrupted = enumerator.value()->read_verified_extent(
        OperationContext{}, extents.front(), payload);
    REQUIRE(!corrupted.ok());
    REQUIRE_EQ(corrupted.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(corrupted.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::BackingDataCorrupt));
}

TEST_CASE(live_extent_enumerator_owns_one_view_and_never_follows_later_generation) {
    MapFixture old_fixture(kM2BlockSize, 7, std::byte{0x71});
    set_base_data(*old_fixture.base, 0, std::byte{0x17});
    old_fixture.build();
    auto created = old_fixture.create_enumerator(kM2BlockSize);
    REQUIRE(created.ok());
    auto frozen = created.move_value();
    old_fixture.store.reset();
    REQUIRE(!old_fixture.journal->layer_destroyed.load());
    REQUIRE(!old_fixture.base->layer_destroyed.load());

    MapFixture later_fixture(kM2BlockSize, 8, std::byte{0x72});
    set_base_data(*later_fixture.base, 0, std::byte{0x28});
    later_fixture.build();
    BackingView current{8, later_fixture.store};
    REQUIRE_EQ(current.generation, 8ull);

    std::vector<LiveDataExtent> extents;
    REQUIRE(frozen->enumerate(
        OperationContext{}, [&extents](const LiveDataExtent& extent) {
            extents.push_back(extent);
            return Result<void>::success();
        }).ok());
    REQUIRE_EQ(extents.size(), 1ull);
    REQUIRE_EQ(extents.front().checkpoint_generation, 7ull);
    std::vector<std::byte> payload(kM2BlockSize);
    REQUIRE(frozen->read_verified_extent(
        OperationContext{}, extents.front(), payload).ok());
    REQUIRE(std::all_of(
        payload.begin(), payload.end(), [](const std::byte value) {
            return value == std::byte{0x17};
        }));

    frozen.reset();
    REQUIRE(old_fixture.journal->layer_destroyed.load());
    REQUIRE(old_fixture.base->layer_destroyed.load());
}

TEST_CASE(live_extent_enumerator_skips_explicit_all_zero_data) {
    MapFixture fixture(2 * kM2BlockSize);
    set_base_data(*fixture.base, 0, std::byte{0});
    set_journal_data(*fixture.journal, 1, std::byte{0});
    fixture.build();
    auto enumerator = fixture.create_enumerator();
    REQUIRE(enumerator.ok());

    std::uint64_t consumed{};
    const auto result = enumerator.value()->enumerate(
        OperationContext{}, [&consumed](const LiveDataExtent&) {
            ++consumed;
            return Result<void>::success();
        });
    REQUIRE(result.ok());
    REQUIRE_EQ(consumed, 0ull);
    REQUIRE_EQ(result.value().data_blocks, 0ull);
    REQUIRE_EQ(result.value().zero_blocks, 2ull);
    REQUIRE_EQ(fixture.journal->calls.load(), 2ull);
    REQUIRE_EQ(fixture.base->calls.load(), 1ull);
}

TEST_CASE(live_extent_enumerator_propagates_cancel_deadline_and_layer_error) {
    for (const bool cancel : {true, false}) {
        MapFixture fixture(2 * kM2BlockSize);
        set_base_data(*fixture.base, 0, std::byte{0x61});
        set_base_data(*fixture.base, 1, std::byte{0x62});
        fixture.build();
        auto enumerator = fixture.create_enumerator();
        REQUIRE(enumerator.ok());

        std::stop_source stop;
        if (cancel) stop.request_stop();
        const OperationContext context{
            stop.get_token(),
            cancel ? std::chrono::steady_clock::time_point::max()
                   : std::chrono::steady_clock::now()};
        const auto result = enumerator.value()->enumerate(
            context, [](const LiveDataExtent&) {
                return Result<void>::success();
            });
        REQUIRE(!result.ok());
        REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
        REQUIRE_EQ(
            result.error().code,
            static_cast<std::uint32_t>(
                cancel ? OperationErrorCode::cancelled
                       : OperationErrorCode::timed_out));
        REQUIRE_EQ(fixture.journal->calls.load(), 0ull);
        REQUIRE_EQ(fixture.base->calls.load(), 0ull);
    }

    MapFixture failed_fixture(kM2BlockSize);
    set_journal_data(*failed_fixture.journal, 0, std::byte{0x44});
    failed_fixture.journal->failure = Error{
        ErrorDomain::persistence, 0xB34, L"MapJournal::read_block",
        L"injected layer failure"};
    failed_fixture.build();
    auto failed_enumerator = failed_fixture.create_enumerator();
    REQUIRE(failed_enumerator.ok());
    const auto failed = failed_enumerator.value()->enumerate(
        OperationContext{}, [](const LiveDataExtent&) {
            return Result<void>::success();
        });
    REQUIRE(!failed.ok());
    REQUIRE_EQ(failed.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(failed.error().code, 0xB34u);
    REQUIRE_EQ(failed_fixture.journal->calls.load(), 1ull);
    REQUIRE_EQ(failed_fixture.base->calls.load(), 0ull);
}

TEST_CASE(live_extent_enumerator_stops_between_blocks_when_cancelled) {
    MapFixture fixture(3 * kM2BlockSize);
    set_base_data(*fixture.base, 0, std::byte{0x71});
    set_base_data(*fixture.base, 1, std::byte{0x72});
    set_base_data(*fixture.base, 2, std::byte{0x73});
    std::stop_source stop;
    fixture.journal->stop_after_read = &stop;
    fixture.build();
    auto enumerator = fixture.create_enumerator();
    REQUIRE(enumerator.ok());

    const auto result = enumerator.value()->enumerate(
        OperationContext{stop.get_token()}, [](const LiveDataExtent&) {
            return Result<void>::success();
        });
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
                                        OperationErrorCode::cancelled));
    REQUIRE_EQ(fixture.journal->calls.load(), 1ull);
    REQUIRE_EQ(fixture.base->calls.load(), 0ull);
}

TEST_CASE(live_extent_enumerator_stops_after_final_data_read_when_cancelled) {
    MapFixture fixture(kM2BlockSize);
    set_journal_data(*fixture.journal, 0, std::byte{0x6A});
    std::stop_source stop;
    fixture.journal->stop_after_read = &stop;
    fixture.build();
    auto enumerator = fixture.create_enumerator(kM2BlockSize);
    REQUIRE(enumerator.ok());

    std::uint64_t consumer_calls{};
    const auto result = enumerator.value()->enumerate(
        OperationContext{stop.get_token()},
        [&consumer_calls](const LiveDataExtent&) {
            ++consumer_calls;
            return Result<void>::success();
        });
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
    REQUIRE_EQ(result.error().code, static_cast<std::uint32_t>(
                                        OperationErrorCode::cancelled));
    REQUIRE_EQ(consumer_calls, 0ull);

    stop = std::stop_source{};
    fixture.journal->stop_after_read = &stop;
    LiveDataExtent descriptor{
        fixture.backing_identity.image_uuid,
        fixture.backing_identity.checkpoint_generation,
        fixture.backing_identity.checkpoint_generation,
        0,
        kM2BlockSize,
        0,
        {}};
    std::vector<std::byte> destination(kM2BlockSize);
    const auto read = enumerator.value()->read_verified_extent(
        OperationContext{stop.get_token()}, descriptor, destination);
    REQUIRE(!read.ok());
    REQUIRE_EQ(read.error().domain, ErrorDomain::storage);
    REQUIRE_EQ(read.error().code, static_cast<std::uint32_t>(
                                      OperationErrorCode::cancelled));
}

TEST_CASE(live_extent_enumerator_stops_after_final_data_read_on_deadline) {
    MapFixture fixture(kM2BlockSize);
    set_journal_data(*fixture.journal, 0, std::byte{0x6B});
    fixture.build();
    auto enumerator = fixture.create_enumerator(kM2BlockSize);
    REQUIRE(enumerator.ok());
    std::vector<LiveDataExtent> descriptors;
    REQUIRE(enumerator.value()->enumerate(
        OperationContext{}, [&descriptors](const LiveDataExtent& extent) {
            descriptors.push_back(extent);
            return Result<void>::success();
        }).ok());
    REQUIRE_EQ(descriptors.size(), 1ull);

    fixture.journal->read_delay = std::chrono::milliseconds(10);
    std::uint64_t consumer_calls{};
    const auto enumerate_deadline = enumerator.value()->enumerate(
        OperationContext{
            {}, std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(1)},
        [&consumer_calls](const LiveDataExtent&) {
            ++consumer_calls;
            return Result<void>::success();
        });
    REQUIRE(!enumerate_deadline.ok());
    REQUIRE_EQ(enumerate_deadline.error().domain, ErrorDomain::storage);
    REQUIRE_EQ(enumerate_deadline.error().code,
               static_cast<std::uint32_t>(
                   OperationErrorCode::timed_out));
    REQUIRE_EQ(consumer_calls, 0ull);

    std::vector<std::byte> destination(kM2BlockSize);
    const auto read_deadline = enumerator.value()->read_verified_extent(
        OperationContext{
            {}, std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(1)},
        descriptors.front(), destination);
    REQUIRE(!read_deadline.ok());
    REQUIRE_EQ(read_deadline.error().domain, ErrorDomain::storage);
    REQUIRE_EQ(read_deadline.error().code,
               static_cast<std::uint32_t>(
                   OperationErrorCode::timed_out));
}

TEST_CASE(live_extent_enumerator_does_not_scan_sparse_virtual_capacity) {
    constexpr std::uint64_t virtual_bytes = 1ull << 30;
    constexpr std::uint64_t block_count = virtual_bytes / kM2BlockSize;
    MapFixture fixture(virtual_bytes);
    set_base_data(*fixture.base, 0, std::byte{0x11});
    set_journal_data(*fixture.journal, block_count - 1,
                     std::byte{0x22});
    fixture.build();
    auto enumerator = fixture.create_enumerator(kM2BlockSize);
    REQUIRE(enumerator.ok());

    std::vector<LiveDataExtent> extents;
    const auto metrics = enumerator.value()->enumerate(
        OperationContext{}, [&extents](const LiveDataExtent& extent) {
            extents.push_back(extent);
            return Result<void>::success();
        });
    REQUIRE(metrics.ok());
    REQUIRE_EQ(metrics.value().candidate_blocks_examined, 2ull);
    REQUIRE_EQ(fixture.journal->calls.load(), 2ull);
    REQUIRE_EQ(fixture.base->calls.load(), 1ull);
    REQUIRE_EQ(extents.size(), 2ull);
    REQUIRE_EQ(extents.front().logical_offset, 0ull);
    REQUIRE_EQ(extents.back().logical_offset,
               virtual_bytes - kM2BlockSize);
    REQUIRE(fixture.journal->candidate_calls.load() <= 3ull);
    REQUIRE(fixture.base->candidate_calls.load() <= 3ull);
}

TEST_CASE(live_extent_candidate_merge_keeps_far_bat_lookahead_cached) {
    constexpr std::uint64_t virtual_bytes = 1ull << 30;
    constexpr std::uint64_t block_count = virtual_bytes / kM2BlockSize;
    MapFixture fixture(virtual_bytes);
    for (std::uint64_t index = 0; index < 64; ++index) {
        set_journal_data(*fixture.journal, index * 2,
                         static_cast<std::byte>(index + 1));
    }
    set_base_data(*fixture.base, block_count - 1, std::byte{0x7F});
    fixture.build();
    auto enumerator = fixture.create_enumerator(kM2BlockSize);
    REQUIRE(enumerator.ok());

    std::uint64_t extents{};
    const auto metrics = enumerator.value()->enumerate(
        OperationContext{}, [&extents](const LiveDataExtent&) {
            ++extents;
            return Result<void>::success();
        });
    REQUIRE(metrics.ok());
    REQUIRE_EQ(metrics.value().candidate_blocks_examined, 65ull);
    REQUIRE_EQ(extents, 65ull);
    REQUIRE_EQ(fixture.base->candidate_calls.load(), 1ull);
    REQUIRE_EQ(fixture.base->calls.load(), 1ull);
    REQUIRE_EQ(fixture.journal->calls.load(), 65ull);
}

TEST_CASE(live_extent_enumerator_propagates_consumer_failure_and_exception) {
    MapFixture fixture(3 * kM2BlockSize);
    set_base_data(*fixture.base, 0, std::byte{0x31});
    fixture.base->entries[1].kind = detail::CompositeBaseKind::Zero;
    set_base_data(*fixture.base, 2, std::byte{0x33});
    fixture.build();
    auto enumerator = fixture.create_enumerator();
    REQUIRE(enumerator.ok());

    std::uint64_t calls{};
    const auto consumer_failure = enumerator.value()->enumerate(
        OperationContext{}, [&calls](const LiveDataExtent&) {
            ++calls;
            return Result<void>::failure({
                ErrorDomain::persistence, 0xC34,
                L"live_extent_test_consumer", L"injected refusal"});
        });
    REQUIRE(!consumer_failure.ok());
    REQUIRE_EQ(calls, 1ull);
    REQUIRE_EQ(consumer_failure.error().code, 0xC34u);

    const auto consumer_exception = enumerator.value()->enumerate(
        OperationContext{}, [](const LiveDataExtent&) -> Result<void> {
            throw 34;
        });
    REQUIRE(!consumer_exception.ok());
    REQUIRE_EQ(consumer_exception.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(consumer_exception.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::FatalInternalInvariant));
}

TEST_CASE(live_extent_enumerator_rejects_invalid_creation_and_read_inputs) {
    MapFixture fixture(kM2BlockSize);
    set_base_data(*fixture.base, 0, std::byte{0x55});
    fixture.build();

    REQUIRE(!LiveExtentEnumerator::create(
                 BackingView{}, fixture.backing_identity,
                 {kM2BlockSize}).ok());
    REQUIRE(!LiveExtentEnumerator::create(
                 BackingView{fixture.backing_identity.checkpoint_generation,
                             fixture.store},
                 fixture.backing_identity, {kM2BlockSize - 1}).ok());
    auto wrong_identity = fixture.backing_identity;
    wrong_identity.image_uuid.bytes[1] = std::byte{0x99};
    REQUIRE(!LiveExtentEnumerator::create(
                 BackingView{fixture.backing_identity.checkpoint_generation,
                             fixture.store},
                 wrong_identity, {kM2BlockSize}).ok());

    auto enumerator = fixture.create_enumerator(kM2BlockSize);
    REQUIRE(enumerator.ok());
    REQUIRE(!enumerator.value()->enumerate(OperationContext{}, {}).ok());
    std::vector<LiveDataExtent> extents;
    REQUIRE(enumerator.value()->enumerate(
        OperationContext{}, [&extents](const LiveDataExtent& extent) {
            extents.push_back(extent);
            return Result<void>::success();
        }).ok());
    REQUIRE_EQ(extents.size(), 1ull);
    std::vector<std::byte> short_destination(kM2BlockSize - 1);
    REQUIRE(!enumerator.value()->read_verified_extent(
                 OperationContext{}, extents.front(),
                 short_destination).ok());
    auto misaligned = extents.front();
    ++misaligned.logical_offset;
    std::vector<std::byte> full_destination(kM2BlockSize);
    REQUIRE(!enumerator.value()->read_verified_extent(
                 OperationContext{}, misaligned,
                 full_destination).ok());
}

}  // namespace
}  // namespace myrd::persistence
