#include "snapshot_coordinator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace myrd {
namespace {

Error coordinator_error(
    DynamicBlockStoreErrorCode code,
    std::wstring message) {
    return Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(code),
        L"SnapshotCoordinator",
        std::move(message),
    };
}

bool same_token(
    const ConsistentSnapshotToken& left,
    const ConsistentSnapshotToken& right) noexcept {
    return left.snapshot_id == right.snapshot_id &&
           left.source_backing_generation ==
               right.source_backing_generation &&
           left.cutoff_dirty_epoch == right.cutoff_dirty_epoch;
}

struct SnapshotBudgetExhausted final {};

constexpr std::size_t kSnapshotMapInfrastructureSlabs = 8;

std::atomic<std::uint64_t> global_snapshot_id{1};

std::uint64_t acquire_snapshot_id() noexcept {
    auto current = global_snapshot_id.load(std::memory_order_relaxed);
    while (current != 0 &&
           current != (std::numeric_limits<std::uint64_t>::max)()) {
        if (global_snapshot_id.compare_exchange_weak(
                current,
                current + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0;
}

struct SnapshotRecordBudget final {
    explicit SnapshotRecordBudget(std::uint64_t limit) noexcept
        : max_bytes(limit) {}

    ~SnapshotRecordBudget() noexcept {
        if (current_bytes != 0 || live_allocations != 0 ||
            std::any_of(
                map_infrastructure_in_use.begin(),
                map_infrastructure_in_use.end(),
                [](bool in_use) { return in_use; })) {
            std::terminate();
        }
    }

    void reserve_bytes(
        std::uint64_t bytes,
        std::uint64_t allocations) {
        if (bytes == 0 || allocations == 0) {
            throw SnapshotBudgetExhausted{};
        }
        if (current_bytes > max_bytes ||
            bytes > max_bytes - current_bytes ||
            live_allocations >
                (std::numeric_limits<std::uint64_t>::max)() - allocations) {
            throw SnapshotBudgetExhausted{};
        }
        current_bytes += bytes;
        live_allocations += allocations;
        if (peak_bytes < current_bytes) {
            peak_bytes = current_bytes;
        }
    }

    void release_bytes(
        std::uint64_t bytes,
        std::uint64_t allocations) noexcept {
        if (bytes == 0 || allocations == 0 || current_bytes < bytes ||
            live_allocations < allocations) {
            std::terminate();
        }
        current_bytes -= bytes;
        live_allocations -= allocations;
    }

    void reserve_records(std::size_t count) {
        if (count == 0 ||
            count > (std::numeric_limits<std::uint64_t>::max)() /
                        kSnapshotCowRecordBudgetBytes) {
            throw SnapshotBudgetExhausted{};
        }
        reserve_bytes(
            static_cast<std::uint64_t>(count) *
                kSnapshotCowRecordBudgetBytes,
            static_cast<std::uint64_t>(count));
    }

    void release_records(std::size_t count) noexcept {
        if (count == 0 ||
            count > (std::numeric_limits<std::uint64_t>::max)() /
                        kSnapshotCowRecordBudgetBytes) {
            std::terminate();
        }
        release_bytes(
            static_cast<std::uint64_t>(count) *
                kSnapshotCowRecordBudgetBytes,
            static_cast<std::uint64_t>(count));
    }

    void* try_allocate_map_infrastructure(std::size_t count) noexcept {
        if (count != 1 || !map_infrastructure_available) {
            return nullptr;
        }
        for (std::size_t index = 0;
             index < map_infrastructure_in_use.size();
             ++index) {
            if (!map_infrastructure_in_use[index]) {
                map_infrastructure_in_use[index] = true;
                return map_infrastructure_slabs[index].data();
            }
        }
        return nullptr;
    }

    void seal_map_infrastructure() noexcept {
        map_infrastructure_available = false;
    }

    bool owns_map_infrastructure(const void* pointer) const noexcept {
        return std::any_of(
            map_infrastructure_slabs.begin(),
            map_infrastructure_slabs.end(),
            [pointer](const auto& slab) { return pointer == slab.data(); });
    }

    void release_map_infrastructure(const void* pointer) noexcept {
        for (std::size_t index = 0;
             index < map_infrastructure_slabs.size();
             ++index) {
            if (pointer == map_infrastructure_slabs[index].data()) {
                if (!map_infrastructure_in_use[index]) {
                    std::terminate();
                }
                map_infrastructure_in_use[index] = false;
                return;
            }
        }
        std::terminate();
    }

    std::uint64_t max_bytes{};
    std::uint64_t current_bytes{};
    std::uint64_t peak_bytes{};
    std::uint64_t live_allocations{};
    alignas(std::max_align_t)
        std::array<
            std::array<std::byte, kSnapshotCowRecordBudgetBytes>,
            kSnapshotMapInfrastructureSlabs>
            map_infrastructure_slabs{};
    std::array<bool, kSnapshotMapInfrastructureSlabs>
        map_infrastructure_in_use{};
    bool map_infrastructure_available{true};
};

template <class T>
class SnapshotSlabAllocator final {
public:
    using value_type = T;

    SnapshotSlabAllocator() noexcept = default;
    explicit SnapshotSlabAllocator(
        SnapshotRecordBudget* budget) noexcept
        : budget_(budget) {}

    template <class U>
    SnapshotSlabAllocator(
        const SnapshotSlabAllocator<U>& other) noexcept
        : budget_(other.budget()) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        static_assert(
            sizeof(T) <= kSnapshotCowRecordBudgetBytes,
            "rebound snapshot map node exceeds its frozen 256-byte slab");
        static_assert(
            alignof(T) <= alignof(std::max_align_t),
            "snapshot map node requires unsupported over-alignment");
        if (budget_ == nullptr || count == 0 ||
            count > (std::numeric_limits<std::size_t>::max)() /
                        static_cast<std::size_t>(
                            kSnapshotCowRecordBudgetBytes)) {
            throw std::bad_alloc{};
        }
        if (void* infrastructure =
                budget_->try_allocate_map_infrastructure(count);
            infrastructure != nullptr) {
            return static_cast<T*>(infrastructure);
        }
        budget_->reserve_records(count);
        try {
            return static_cast<T*>(::operator new(
                count * static_cast<std::size_t>(
                            kSnapshotCowRecordBudgetBytes)));
        } catch (...) {
            budget_->release_records(count);
            throw;
        }
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        if (budget_ == nullptr || pointer == nullptr || count == 0) {
            std::terminate();
        }
        if (budget_->owns_map_infrastructure(pointer)) {
            if (count != 1) {
                std::terminate();
            }
            budget_->release_map_infrastructure(pointer);
            return;
        }
        ::operator delete(pointer);
        budget_->release_records(count);
    }

    SnapshotRecordBudget* budget() const noexcept { return budget_; }

    template <class U>
    bool operator==(const SnapshotSlabAllocator<U>& other) const noexcept {
        return budget_ == other.budget();
    }

    template <class U>
    bool operator!=(const SnapshotSlabAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    template <class>
    friend class SnapshotSlabAllocator;

    SnapshotRecordBudget* budget_{};
};

class SnapshotPayloadBuffer final {
public:
    explicit SnapshotPayloadBuffer(
        SnapshotRecordBudget* budget) noexcept
        : budget_(budget) {}

    ~SnapshotPayloadBuffer() noexcept { reset(); }

    SnapshotPayloadBuffer(const SnapshotPayloadBuffer&) = delete;
    SnapshotPayloadBuffer& operator=(const SnapshotPayloadBuffer&) = delete;

    SnapshotPayloadBuffer(SnapshotPayloadBuffer&& other) noexcept
        : budget_(other.budget_),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}

    SnapshotPayloadBuffer& operator=(SnapshotPayloadBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            budget_ = other.budget_;
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    void assign(std::span<const std::byte> source) {
        if (budget_ == nullptr || data_ != nullptr || source.empty() ||
            source.size() > (std::numeric_limits<std::uint64_t>::max)()) {
            throw std::bad_alloc{};
        }
        const auto bytes = static_cast<std::uint64_t>(source.size());
        budget_->reserve_bytes(bytes, 1);
        try {
            data_ = static_cast<std::byte*>(
                ::operator new(static_cast<std::size_t>(bytes)));
        } catch (...) {
            budget_->release_bytes(bytes, 1);
            throw;
        }
        std::copy(source.begin(), source.end(), data_);
        size_ = source.size();
    }

    [[nodiscard]] const std::byte* begin() const noexcept { return data_; }
    [[nodiscard]] const std::byte* end() const noexcept {
        return data_ == nullptr ? nullptr : data_ + size_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
    void reset() noexcept {
        if (data_ == nullptr) {
            if (size_ != 0) {
                std::terminate();
            }
            return;
        }
        if (budget_ == nullptr || size_ == 0) {
            std::terminate();
        }
        ::operator delete(data_);
        budget_->release_bytes(static_cast<std::uint64_t>(size_), 1);
        data_ = nullptr;
        size_ = 0;
    }

    SnapshotRecordBudget* budget_{};
    std::byte* data_{};
    std::size_t size_{};
};

} // namespace

class SnapshotCoordinatorState final {
public:
    using Payload = SnapshotPayloadBuffer;

    struct Record final {
        SnapshotPreimage preimage;
        Payload payload;
        std::uint64_t reservation_id{};
        bool committed{};

        Record(
            const SnapshotPreimage& metadata,
            std::uint64_t next_reservation_id,
            SnapshotRecordBudget* budget)
            : preimage{
                  metadata.block_index,
                  metadata.version,
                  metadata.kind,
                  metadata.base_generation,
                  metadata.dirty_epoch,
                  {}},
              payload(budget),
              reservation_id(next_reservation_id) {}
    };

    struct CountingLess final {
        std::uint64_t* probes{};

        bool operator()(
            std::uint64_t left,
            std::uint64_t right) const noexcept {
            if (probes == nullptr ||
                *probes == (std::numeric_limits<std::uint64_t>::max)()) {
                std::terminate();
            }
            ++*probes;
            return left < right;
        }
    };

    using RecordEntry = std::pair<const std::uint64_t, Record>;
    using RecordAllocator = SnapshotSlabAllocator<RecordEntry>;
    using RecordMap = std::map<
        std::uint64_t,
        Record,
        CountingLess,
        RecordAllocator>;

    SnapshotCoordinatorState(
        std::uint64_t max_bytes,
        std::shared_ptr<ISnapshotCoordinatorNonReentrantBarrier>
            next_barrier)
        : budget(max_bytes),
          records(CountingLess{&index_probes}, RecordAllocator{&budget}),
          non_reentrant_barrier(std::move(next_barrier)) {
        // Standard-library implementations may allocate one or more sentinel
        // and container-proxy nodes while constructing std::map (MSVC Debug
        // uses more than Release). They are fixed coordinator infrastructure,
        // not COW records. Seal the bounded embedded pool after construction so
        // every later allocation is charged to the snapshot budget.
        budget.seal_map_infrastructure();
    }

    ~SnapshotCoordinatorState() noexcept {
        clear_records();
    }

    void abandon(SnapshotMetadataReservation& reservation) noexcept;

    bool token_matches(const ConsistentSnapshotToken& token) const noexcept {
        return active_token.has_value() &&
               same_token(*active_token, token);
    }

    void pause_for_test(
        SnapshotCoordinatorBarrierEvent event) const noexcept {
        if (non_reentrant_barrier) {
            non_reentrant_barrier->pause_while_coordinator_locked(event);
        }
    }

    Record* find_record(std::uint64_t block_index) noexcept {
        const auto found = records.find(block_index);
        return found == records.end() ? nullptr : &found->second;
    }

    const Record* find_record(std::uint64_t block_index) const noexcept {
        const auto found = records.find(block_index);
        return found == records.end() ? nullptr : &found->second;
    }

    Record* insert_record(
        const SnapshotPreimage& metadata,
        std::uint64_t reservation_id) {
        auto [inserted, is_new] = records.try_emplace(
            metadata.block_index,
            metadata,
            reservation_id,
            &budget);
        if (!is_new) {
            std::terminate();
        }
        return &inserted->second;
    }

    void erase_record(Record* record) noexcept {
        if (record == nullptr) {
            std::terminate();
        }
        const auto found = records.find(record->preimage.block_index);
        if (found == records.end() || &found->second != record) {
            std::terminate();
        }
        erase_record(found);
    }

    void erase_record(RecordMap::iterator record) noexcept {
        if (record->second.committed) {
            if (committed_records == 0) {
                std::terminate();
            }
            --committed_records;
        } else {
            if (pending_reservations == 0) {
                std::terminate();
            }
            --pending_reservations;
        }
        records.erase(record);
    }

    void clear_records() noexcept {
        while (!records.empty()) {
            erase_record(records.begin());
        }
        if (committed_records != 0 || pending_reservations != 0) {
            std::terminate();
        }
        if (budget.current_bytes != 0 || budget.live_allocations != 0) {
            std::terminate();
        }
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    SnapshotRecordBudget budget;
    std::uint64_t index_probes{};
    RecordMap records;
    std::shared_ptr<ISnapshotCoordinatorNonReentrantBarrier>
        non_reentrant_barrier;
    std::optional<ConsistentSnapshotToken> active_token;
    std::optional<ConsistentSnapshotState> active_state;
    std::optional<DynamicBlockStoreErrorCode> abort_reason;
    std::uint64_t next_reservation_id{1};
    std::uint64_t next_scan_block{};
    std::uint64_t committed_records{};
    std::uint64_t pending_reservations{};
    std::atomic<bool> preimage_lookup_entered_for_test{};
};

SnapshotMetadataReservation::SnapshotMetadataReservation(
    SnapshotPreimageDisposition disposition,
    std::weak_ptr<SnapshotCoordinatorState> owner_state,
    std::uint64_t snapshot_id,
    std::uint64_t block_index,
    std::uint64_t reservation_id) noexcept
    : disposition_(disposition),
      owner_state_(std::move(owner_state)),
      snapshot_id_(snapshot_id),
      block_index_(block_index),
      reservation_id_(reservation_id) {}

SnapshotMetadataReservation::~SnapshotMetadataReservation() noexcept {
    reset();
}

SnapshotMetadataReservation::SnapshotMetadataReservation(
    SnapshotMetadataReservation&& other) noexcept
    : disposition_(other.disposition_),
      owner_state_(std::move(other.owner_state_)),
      snapshot_id_(other.snapshot_id_),
      block_index_(other.block_index_),
      reservation_id_(other.reservation_id_) {
}

SnapshotMetadataReservation& SnapshotMetadataReservation::operator=(
    SnapshotMetadataReservation&& other) noexcept {
    if (this != &other) {
        reset();
        disposition_ = other.disposition_;
        owner_state_ = std::move(other.owner_state_);
        snapshot_id_ = other.snapshot_id_;
        block_index_ = other.block_index_;
        reservation_id_ = other.reservation_id_;
    }
    return *this;
}

void SnapshotMetadataReservation::reset() noexcept {
    if (auto owner_state = owner_state_.lock()) {
        owner_state->abandon(*this);
    }
    owner_state_.reset();
}

SnapshotCoordinator::SnapshotCoordinator(
    std::uint64_t max_bytes,
    std::shared_ptr<ISnapshotCoordinatorNonReentrantBarrier>
        non_reentrant_barrier)
    : state_(std::make_shared<SnapshotCoordinatorState>(
          max_bytes, std::move(non_reentrant_barrier))) {}

SnapshotCoordinator::~SnapshotCoordinator() noexcept = default;

Result<ConsistentSnapshotToken> SnapshotCoordinator::activate(
    BackingGeneration source_backing_generation,
    std::uint64_t cutoff_dirty_epoch) {
    std::scoped_lock lock(state_->mutex);
    state_->pause_for_test(
        SnapshotCoordinatorBarrierEvent::ActivateLocked);
    if (state_->active_token.has_value()) {
        return Result<ConsistentSnapshotToken>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotBusy,
                L"another consistent snapshot owns the coordinator"));
    }
    if (!state_->records.empty() || state_->budget.current_bytes != 0 ||
        state_->pending_reservations != 0 ||
        state_->committed_records != 0) {
        return Result<ConsistentSnapshotToken>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"inactive coordinator retained COW resources"));
    }
    const auto snapshot_id = acquire_snapshot_id();
    if (snapshot_id == 0) {
        return Result<ConsistentSnapshotToken>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"consistent snapshot id exhausted"));
    }

    const ConsistentSnapshotToken token{
        snapshot_id,
        source_backing_generation,
        cutoff_dirty_epoch,
    };
    state_->active_token = token;
    state_->active_state = ConsistentSnapshotState::Active;
    state_->abort_reason.reset();
    state_->next_scan_block = 0;
    state_->condition.notify_all();
    return Result<ConsistentSnapshotToken>::success(token);
}

Result<SnapshotMetadataReservation>
SnapshotCoordinator::reserve_metadata_before_mutation(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    const SnapshotPreimage& metadata) {
    return reserve_before_mutation(
        context, token, metadata, {}, false);
}

Result<SnapshotMetadataReservation>
SnapshotCoordinator::reserve_data_before_mutation(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    const SnapshotPreimage& metadata,
    std::span<const std::byte> payload) {
    return reserve_before_mutation(
        context, token, metadata, payload, true);
}

Result<SnapshotMetadataReservation>
SnapshotCoordinator::reserve_before_mutation(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    const SnapshotPreimage& metadata,
    std::span<const std::byte> payload,
    bool data_preimage) {
    const auto initial_live = check_operation_context(context);
    if (!initial_live.ok()) {
        return Result<SnapshotMetadataReservation>::failure(
            initial_live.error());
    }

    std::scoped_lock lock(state_->mutex);
    if (!state_->token_matches(token)) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                L"snapshot token does not own this coordinator"));
    }
    if (data_preimage &&
        (metadata.kind != DirtyRecordKind::Data ||
         payload.size() != kBlockSizeBytes)) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"DATA pre-image must own exactly one complete block"));
    }
    if (!data_preimage &&
        metadata.kind != DirtyRecordKind::Zero &&
        metadata.kind != DirtyRecordKind::Discard) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                metadata.kind == DirtyRecordKind::Data
                    ? L"DATA pre-image requires the payload reservation path"
                    : L"snapshot pre-image has an unknown dirty record kind"));
    }
    if (!metadata.data.empty() || (!data_preimage && !payload.empty())) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"metadata-only pre-image unexpectedly owns payload bytes"));
    }
    if (metadata.dirty_epoch == 0) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot member dirty epoch must be nonzero"));
    }
    if (metadata.base_generation > token.source_backing_generation) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot member base generation exceeds the cut"));
    }
    if (metadata.dirty_epoch > token.cutoff_dirty_epoch) {
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::NotMember,
                {},
                0,
                0,
                0});
    }
    if (state_->active_state ==
        ConsistentSnapshotState::FullyScanned) {
        if (metadata.block_index >= state_->next_scan_block) {
            return Result<SnapshotMetadataReservation>::failure(
                coordinator_error(
                    DynamicBlockStoreErrorCode::FatalInternalInvariant,
                    L"fully scanned snapshot has an invalid frontier"));
        }
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::Consumed,
                {},
                0,
                0,
                0});
    }
    if (state_->active_state != ConsistentSnapshotState::Active) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotAborted,
                L"snapshot is not active"));
    }
    if (metadata.block_index < state_->next_scan_block) {
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::Consumed,
                {},
                0,
                0,
                0});
    }
    if (const auto* existing = state_->find_record(metadata.block_index);
        existing != nullptr) {
        if (!existing->committed) {
            return Result<SnapshotMetadataReservation>::failure(
                coordinator_error(
                    DynamicBlockStoreErrorCode::SnapshotBusy,
                    L"snapshot block already has a pending reservation"));
        }
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::AlreadyPreserved,
                {},
                0,
                0,
                0});
    }
    if (state_->next_reservation_id == 0 ||
        state_->next_reservation_id ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot reservation id exhausted"));
    }

    const auto reservation_id = state_->next_reservation_id++;
    SnapshotCoordinatorState::Record* inserted_record = nullptr;
    try {
        // The 256-byte slab contains the metadata record, reservation
        // bookkeeping and ordered tree node. DATA additionally owns one
        // separately budgeted 64 KiB payload allocation.
        inserted_record = state_->insert_record(metadata, reservation_id);
        ++state_->pending_reservations;
        if (data_preimage) {
            inserted_record->payload.assign(payload);
        }
        state_->pause_for_test(
            SnapshotCoordinatorBarrierEvent::ReservationInserted);

        const auto reserved_live = check_operation_context(context);
        if (!reserved_live.ok()) {
            state_->erase_record(inserted_record);
            return Result<SnapshotMetadataReservation>::failure(
                reserved_live.error());
        }
    } catch (const SnapshotBudgetExhausted&) {
        if (inserted_record != nullptr) {
            state_->erase_record(inserted_record);
        }
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
                L"snapshot COW metadata budget is exhausted"));
    } catch (const std::bad_alloc&) {
        if (inserted_record != nullptr) {
            state_->erase_record(inserted_record);
        }
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
                L"snapshot COW metadata allocation failed"));
    } catch (...) {
        if (inserted_record != nullptr) {
            state_->erase_record(inserted_record);
        }
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot COW metadata insertion failed"));
    }

    return Result<SnapshotMetadataReservation>::success(
        SnapshotMetadataReservation{
            SnapshotPreimageDisposition::Reserved,
            state_,
            token.snapshot_id,
            metadata.block_index,
            reservation_id});
}

Result<void> SnapshotCoordinator::publish_metadata_preimage(
    const OperationContext& context,
    SnapshotMetadataReservation& reservation) {
    const auto initial_live = check_operation_context(context);
    if (!initial_live.ok()) {
        rollback_metadata_preimage(reservation);
        return Result<void>::failure(initial_live.error());
    }

    std::scoped_lock lock(state_->mutex);
    auto reservation_state = reservation.owner_state_.lock();
    if (reservation_state.get() != state_.get()) {
        return Result<void>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                L"metadata reservation belongs to another coordinator"));
    }
    if (reservation.disposition_ !=
            SnapshotPreimageDisposition::Reserved ||
        !state_->active_token.has_value() ||
        reservation.snapshot_id_ != state_->active_token->snapshot_id) {
        reservation.owner_state_.reset();
        return Result<void>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                L"metadata reservation does not belong to active token"));
    }
    auto* record = state_->find_record(reservation.block_index_);
    if (record == nullptr || record->committed ||
        record->reservation_id != reservation.reservation_id_) {
        reservation.owner_state_.reset();
        return Result<void>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"metadata reservation record is missing or changed"));
    }

    const auto publish_live = check_operation_context(context);
    if (!publish_live.ok()) {
        state_->erase_record(record);
        reservation.owner_state_.reset();
        return Result<void>::failure(publish_live.error());
    }
    if (state_->pending_reservations == 0) {
        std::terminate();
    }
    --state_->pending_reservations;
    ++state_->committed_records;
    record->committed = true;
    state_->pause_for_test(
        SnapshotCoordinatorBarrierEvent::PreimagePublished);
    reservation.owner_state_.reset();
    state_->condition.notify_all();
    return Result<void>::success();
}

void SnapshotCoordinator::rollback_metadata_preimage(
    SnapshotMetadataReservation& reservation) noexcept {
    auto reservation_state = reservation.owner_state_.lock();
    if (reservation_state.get() == state_.get()) {
        state_->abandon(reservation);
    }
}

std::optional<SnapshotPreimage> SnapshotCoordinator::peek_preimage(
    std::uint64_t block_index) const {
    state_->preimage_lookup_entered_for_test.store(
        true, std::memory_order_release);
    state_->preimage_lookup_entered_for_test.notify_all();
    std::scoped_lock lock(state_->mutex);
    const auto* record = state_->find_record(block_index);
    if (record == nullptr ||
        !record->committed ||
        block_index < state_->next_scan_block) {
        return std::nullopt;
    }
    auto result = record->preimage;
    if (!record->payload.empty()) {
        result.data.assign(record->payload.begin(), record->payload.end());
    }
    return result;
}

std::optional<SnapshotPreimage>
SnapshotCoordinator::peek_preimage_metadata(
    std::uint64_t block_index) const {
    std::scoped_lock lock(state_->mutex);
    const auto* record = state_->find_record(block_index);
    if (record == nullptr ||
        !record->committed ||
        block_index < state_->next_scan_block) {
        return std::nullopt;
    }
    return record->preimage;
}

Result<void> SnapshotCoordinator::advance_scan_frontier(
    const ConsistentSnapshotToken& token,
    std::uint64_t expected_scan_block,
    std::uint64_t next_scan_block,
    std::uint64_t total_blocks) {
    std::scoped_lock lock(state_->mutex);
    if (!state_->token_matches(token)) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    if (state_->next_scan_block != expected_scan_block) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotBusy,
            L"snapshot frontier changed while a batch was staged"));
    }
    if (next_scan_block < state_->next_scan_block ||
        next_scan_block > total_blocks) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"snapshot frontier must advance monotonically within geometry"));
    }
    for (auto record = state_->records.begin();
         record != state_->records.end() &&
         record->first < next_scan_block;
         ++record) {
        if (!record->second.committed) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotBusy,
                L"snapshot frontier reached a pending reservation"));
        }
    }
    while (!state_->records.empty() &&
           state_->records.begin()->first < next_scan_block) {
        state_->erase_record(state_->records.begin());
    }
    state_->next_scan_block = next_scan_block;
    if (next_scan_block == total_blocks) {
        state_->active_state = ConsistentSnapshotState::FullyScanned;
    }
    state_->condition.notify_all();
    return Result<void>::success();
}

Result<void> SnapshotCoordinator::finish(
    const ConsistentSnapshotToken& token,
    std::uint64_t total_blocks) {
    std::scoped_lock lock(state_->mutex);
    if (!state_->token_matches(token)) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    if (state_->active_state != ConsistentSnapshotState::FullyScanned ||
        state_->next_scan_block != total_blocks) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotNotFullyScanned,
            L"snapshot cannot finish before its frontier reaches the end"));
    }
    state_->clear_records();
    state_->active_token.reset();
    state_->active_state.reset();
    state_->abort_reason.reset();
    state_->next_scan_block = 0;
    state_->condition.notify_all();
    return Result<void>::success();
}

Result<void> SnapshotCoordinator::abort(
    const ConsistentSnapshotToken& token) {
    std::scoped_lock lock(state_->mutex);
    if (!state_->token_matches(token)) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    state_->active_state = ConsistentSnapshotState::Aborted;
    state_->abort_reason = DynamicBlockStoreErrorCode::SnapshotAborted;
    state_->condition.notify_all();
    state_->clear_records();
    state_->active_token.reset();
    state_->active_state.reset();
    state_->abort_reason.reset();
    state_->next_scan_block = 0;
    state_->condition.notify_all();
    return Result<void>::success();
}

Result<ConsistentSnapshotStatus> SnapshotCoordinator::status(
    const ConsistentSnapshotToken& token) const {
    std::scoped_lock lock(state_->mutex);
    if (!state_->token_matches(token) || !state_->active_state.has_value()) {
        return Result<ConsistentSnapshotStatus>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    return Result<ConsistentSnapshotStatus>::success(
        ConsistentSnapshotStatus{
            *state_->active_state,
            state_->budget.current_bytes,
            state_->committed_records,
            0,
        });
}

SnapshotCoordinatorSnapshot SnapshotCoordinator::snapshot() const {
    std::scoped_lock lock(state_->mutex);
    return SnapshotCoordinatorSnapshot{
        state_->active_token,
        state_->active_state,
        state_->next_scan_block,
        state_->budget.current_bytes,
        state_->budget.peak_bytes,
        state_->committed_records,
        state_->pending_reservations,
    };
}

Result<void> SnapshotCoordinator::validate_invariants_for_test() const {
    std::scoped_lock lock(state_->mutex);
    const auto invariant_failure = [](std::wstring message) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            std::move(message)));
    };

    if (state_->active_token.has_value() !=
        state_->active_state.has_value()) {
        return invariant_failure(
            L"snapshot token/state presence diverged");
    }
    if (state_->active_state.has_value()) {
        switch (*state_->active_state) {
        case ConsistentSnapshotState::Active:
        case ConsistentSnapshotState::FullyScanned:
        case ConsistentSnapshotState::Aborted:
            break;
        default:
            return invariant_failure(L"snapshot state is unknown");
        }
        if (state_->active_token->snapshot_id == 0) {
            return invariant_failure(L"active snapshot id is zero");
        }
        const bool is_aborted =
            *state_->active_state == ConsistentSnapshotState::Aborted;
        if (is_aborted != state_->abort_reason.has_value()) {
            return invariant_failure(
                L"snapshot abort state/reason presence diverged");
        }
    } else if (!state_->records.empty() ||
               state_->next_scan_block != 0 ||
               state_->abort_reason.has_value()) {
        return invariant_failure(
            L"inactive snapshot retained coordinator state");
    }

    std::uint64_t committed = 0;
    std::uint64_t pending = 0;
    std::optional<std::uint64_t> previous_block;
    for (const auto& [block_index, record] : state_->records) {
        if (previous_block.has_value() &&
            block_index <= *previous_block) {
            return invariant_failure(
                L"snapshot map keys are not strictly ordered");
        }
        previous_block = block_index;
        if (record.preimage.block_index != block_index ||
            block_index < state_->next_scan_block) {
            return invariant_failure(
                L"snapshot record key/frontier invariant failed");
        }
        const bool data_record =
            record.preimage.kind == DirtyRecordKind::Data;
        const bool metadata_record =
            record.preimage.kind == DirtyRecordKind::Zero ||
            record.preimage.kind == DirtyRecordKind::Discard;
        if ((!data_record && !metadata_record) ||
            !record.preimage.data.empty() ||
            (data_record && record.payload.size() != kBlockSizeBytes) ||
            (metadata_record && !record.payload.empty())) {
            return invariant_failure(
                L"snapshot record kind and payload diverged");
        }
        if (record.reservation_id == 0) {
            return invariant_failure(
                L"snapshot reservation id is zero");
        }
        if (state_->active_token.has_value() &&
            (record.preimage.dirty_epoch == 0 ||
             record.preimage.dirty_epoch >
                 state_->active_token->cutoff_dirty_epoch ||
             record.preimage.base_generation >
                 state_->active_token->source_backing_generation)) {
            return invariant_failure(
                L"snapshot map retained a non-member record");
        }
        if (record.committed) {
            ++committed;
        } else {
            ++pending;
        }
    }
    if (committed != state_->committed_records ||
        pending != state_->pending_reservations) {
        return invariant_failure(
            L"snapshot pending/committed counters diverged");
    }
    if (state_->records.size() >
        (std::numeric_limits<std::uint64_t>::max)() /
            kSnapshotCowRecordBudgetBytes) {
        return invariant_failure(L"snapshot map size overflowed budget math");
    }
    const auto metadata_bytes =
        static_cast<std::uint64_t>(state_->records.size()) *
        kSnapshotCowRecordBudgetBytes;
    std::uint64_t data_records = 0;
    for (const auto& [block_index, record] : state_->records) {
        (void)block_index;
        if (record.preimage.kind == DirtyRecordKind::Data) {
            ++data_records;
        }
    }
    if (data_records >
        (std::numeric_limits<std::uint64_t>::max)() / kBlockSizeBytes) {
        return invariant_failure(
            L"snapshot DATA payload count overflowed budget math");
    }
    const auto payload_bytes = data_records * kBlockSizeBytes;
    if (metadata_bytes >
        (std::numeric_limits<std::uint64_t>::max)() - payload_bytes) {
        return invariant_failure(
            L"snapshot total COW bytes overflowed budget math");
    }
    const auto expected_bytes = metadata_bytes + payload_bytes;
    const auto expected_allocations =
        static_cast<std::uint64_t>(state_->records.size()) + data_records;
    if (state_->budget.live_allocations != expected_allocations ||
        state_->budget.current_bytes != expected_bytes ||
        state_->budget.current_bytes > state_->budget.max_bytes ||
        state_->budget.peak_bytes < state_->budget.current_bytes ||
        state_->budget.peak_bytes > state_->budget.max_bytes) {
        return invariant_failure(
            L"snapshot allocator slabs and byte accounting diverged");
    }
    return Result<void>::success();
}

std::uint64_t SnapshotCoordinator::index_probe_count_for_test() const {
    std::scoped_lock lock(state_->mutex);
    return state_->index_probes;
}

bool SnapshotCoordinator::preimage_lookup_entered_for_test() const noexcept {
    return state_->preimage_lookup_entered_for_test.load(
        std::memory_order_acquire);
}

void SnapshotCoordinatorState::abandon(
    SnapshotMetadataReservation& reservation) noexcept {
    try {
        std::scoped_lock lock(mutex);
        auto reservation_state = reservation.owner_state_.lock();
        if (reservation_state.get() == this) {
            auto* record = find_record(
                reservation.block_index_);
            if (record != nullptr &&
                !record->committed &&
                record->reservation_id ==
                    reservation.reservation_id_) {
                erase_record(record);
                condition.notify_all();
            }
            reservation.owner_state_.reset();
        }
    } catch (...) {
        std::terminate();
    }
}

} // namespace myrd
