#include "snapshot_coordinator.h"
#include "src/common/scope_exit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
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

    void allocate(std::size_t size) {
        if (budget_ == nullptr || data_ != nullptr || size == 0 ||
            size > (std::numeric_limits<std::uint64_t>::max)()) {
            throw std::bad_alloc{};
        }
        const auto bytes = static_cast<std::uint64_t>(size);
        budget_->reserve_bytes(bytes, 1);
        try {
            data_ = static_cast<std::byte*>(
                ::operator new(static_cast<std::size_t>(bytes)));
        } catch (...) {
            budget_->release_bytes(bytes, 1);
            throw;
        }
        size_ = size;
    }

    [[nodiscard]] std::byte* data() noexcept { return data_; }
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
        bool fill_active{};
        bool payload_filled{};
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

    struct CapacityWaiter final {
        std::uint64_t ticket{};
        std::uint64_t required_bytes{};
        std::uint64_t block_index{};
    };

    struct CapacityGrant final {
        std::uint64_t ticket{};
        std::uint64_t block_index{};
    };

    SnapshotCoordinatorState(
        std::uint64_t max_bytes,
        std::shared_ptr<ISnapshotCoordinatorNonReentrantBarrier>
            next_barrier,
        std::shared_ptr<ISnapshotPressureClock> next_pressure_clock)
        : budget(max_bytes),
          records(CountingLess{&index_probes}, RecordAllocator{&budget}),
          non_reentrant_barrier(std::move(next_barrier)),
          pressure_clock(std::move(next_pressure_clock)) {
        // Standard-library implementations may allocate one or more sentinel
        // and container-proxy nodes while constructing std::map (MSVC Debug
        // uses more than Release). They are fixed coordinator infrastructure,
        // not COW records. Seal the bounded embedded pool after construction so
        // every later allocation is charged to the snapshot budget.
        budget.seal_map_infrastructure();
    }

    ~SnapshotCoordinatorState() noexcept {
        if (publication_pins != 0 || publication_end_waiters != 0) {
            std::terminate();
        }
        clear_records();
    }

    void abandon(SnapshotMetadataReservation& reservation) noexcept;
    void release_publication_pin(SnapshotPublicationPin& pin) noexcept;

    std::chrono::steady_clock::time_point now() const noexcept {
        return pressure_clock ? pressure_clock->now()
                              : std::chrono::steady_clock::now();
    }

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
        for (const auto& [block_index, record] : records) {
            (void)block_index;
            if (record.fill_active) {
                std::terminate();
            }
        }
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

    void clear_committed_records() noexcept {
        for (auto record = records.begin(); record != records.end();) {
            if (record->second.committed) {
                auto current = record++;
                erase_record(current);
            } else {
                ++record;
            }
        }
    }

    bool has_active_fill() const noexcept {
        return std::any_of(
            records.begin(), records.end(), [](const auto& entry) {
                return entry.second.fill_active;
            });
    }

    void finalize_explicit_abort_if_drained() noexcept {
        if (!abort_finalize_requested || closing ||
            active_state != ConsistentSnapshotState::Aborted ||
            pending_reservations != 0 || has_active_fill() ||
            !capacity_waiters.empty() || capacity_grant.has_value()) {
            return;
        }
        clear_records();
        active_token.reset();
        active_state.reset();
        abort_reason.reset();
        next_scan_block = 0;
        abort_finalize_requested = false;
        condition.notify_all();
    }

    mutable std::mutex mutex;
    std::condition_variable_any condition;
    SnapshotRecordBudget budget;
    std::uint64_t index_probes{};
    RecordMap records;
    std::shared_ptr<ISnapshotCoordinatorNonReentrantBarrier>
        non_reentrant_barrier;
    std::shared_ptr<ISnapshotPressureClock> pressure_clock;
    std::optional<ConsistentSnapshotToken> active_token;
    std::optional<ConsistentSnapshotState> active_state;
    std::optional<DynamicBlockStoreErrorCode> abort_reason;
    std::uint64_t next_reservation_id{1};
    std::uint64_t next_scan_block{};
    std::uint64_t committed_records{};
    std::uint64_t pending_reservations{};
    std::deque<CapacityWaiter> capacity_waiters;
    std::optional<CapacityGrant> capacity_grant;
    std::uint64_t next_capacity_ticket{1};
    std::uint64_t writer_stalls_total{};
    std::uint64_t auto_aborts_total{};
    std::uint64_t publication_pins{};
    std::uint64_t publication_end_waiters{};
    bool abort_finalize_requested{};
    bool closing{};
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

SnapshotPublicationPin::SnapshotPublicationPin(
    std::shared_ptr<SnapshotCoordinatorState> owner_state,
    const std::uint64_t snapshot_id) noexcept
    : owner_state_(std::move(owner_state)), snapshot_id_(snapshot_id) {}

SnapshotPublicationPin::~SnapshotPublicationPin() noexcept {
    reset();
}

SnapshotPublicationPin::SnapshotPublicationPin(
    SnapshotPublicationPin&& other) noexcept
    : owner_state_(std::move(other.owner_state_)),
      snapshot_id_(std::exchange(other.snapshot_id_, 0)) {}

SnapshotPublicationPin& SnapshotPublicationPin::operator=(
    SnapshotPublicationPin&& other) noexcept {
    if (this != &other) {
        reset();
        owner_state_ = std::move(other.owner_state_);
        snapshot_id_ = std::exchange(other.snapshot_id_, 0);
    }
    return *this;
}

void SnapshotPublicationPin::reset() noexcept {
    // Keep the state alive until release_publication_pin() has returned even
    // when the coordinator was already destroyed and this pin owns the last
    // shared reference.
    auto owner_state = owner_state_;
    if (owner_state) {
        owner_state->release_publication_pin(*this);
    }
    owner_state_.reset();
    snapshot_id_ = 0;
}

void SnapshotCoordinatorState::release_publication_pin(
    SnapshotPublicationPin& pin) noexcept {
    try {
        std::scoped_lock lock(mutex);
        if (pin.snapshot_id_ == 0 || publication_pins == 0 ||
            !active_token.has_value() ||
            active_token->snapshot_id != pin.snapshot_id_) {
            std::terminate();
        }
        --publication_pins;
        pin.owner_state_.reset();
        pin.snapshot_id_ = 0;
        condition.notify_all();
    } catch (...) {
        std::terminate();
    }
}

SnapshotCoordinator::SnapshotCoordinator(
    std::uint64_t max_bytes,
    std::shared_ptr<ISnapshotCoordinatorNonReentrantBarrier>
        non_reentrant_barrier,
    std::shared_ptr<ISnapshotPressureClock> pressure_clock)
    : state_(std::make_shared<SnapshotCoordinatorState>(
          max_bytes,
          std::move(non_reentrant_barrier),
          std::move(pressure_clock))) {}

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
    const SnapshotPreimage& metadata,
    std::uint64_t capacity_grant_ticket) {
    return reserve_before_mutation(
        context,
        token,
        metadata,
        {},
        false,
        capacity_grant_ticket);
}

Result<SnapshotMetadataReservation>
SnapshotCoordinator::reserve_data_before_mutation(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    const SnapshotPreimage& metadata,
    std::span<const std::byte> payload,
    std::uint64_t capacity_grant_ticket) {
    return reserve_before_mutation(
        context,
        token,
        metadata,
        payload,
        true,
        capacity_grant_ticket);
}

Result<SnapshotMetadataReservation>
SnapshotCoordinator::reserve_before_mutation(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    const SnapshotPreimage& metadata,
    std::span<const std::byte> payload,
    bool data_preimage,
    std::uint64_t capacity_grant_ticket) {
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
    bool grant_released = false;
    if (capacity_grant_ticket != 0) {
        if (!state_->capacity_grant.has_value() ||
            state_->capacity_grant->ticket != capacity_grant_ticket ||
            state_->capacity_grant->block_index != metadata.block_index) {
            if (state_->active_state !=
                ConsistentSnapshotState::Active) {
                return Result<SnapshotMetadataReservation>::failure(
                    coordinator_error(
                        DynamicBlockStoreErrorCode::SnapshotAborted,
                        L"snapshot ended before its capacity grant was consumed"));
            }
            return Result<SnapshotMetadataReservation>::failure(
                coordinator_error(
                    DynamicBlockStoreErrorCode::FatalInternalInvariant,
                    L"snapshot capacity grant ticket is missing or changed"));
        }
        state_->capacity_grant.reset();
        grant_released = true;
    } else if (state_->capacity_grant.has_value()) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
                L"snapshot COW capacity is granted to an earlier waiter"));
    }
    ScopeExit notify_grant_release([&]() noexcept {
        if (grant_released) {
            state_->condition.notify_all();
        }
    });
    if (!grant_released && !state_->capacity_waiters.empty()) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
                L"snapshot COW capacity is reserved for an earlier waiter"));
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
    if (metadata.dirty_epoch > token.cutoff_dirty_epoch) {
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::NotMember,
                {},
                0,
                0,
                0});
    }
    if (metadata.base_generation > token.source_backing_generation) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot member base generation exceeds the cut"));
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
            inserted_record->payload.allocate(payload.size());
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

Result<void> SnapshotCoordinator::fill_data_preimage(
    const OperationContext& context,
    SnapshotMetadataReservation& reservation,
    std::span<const std::byte> payload,
    ISnapshotPressureTestObserver* test_observer) {
    const auto initial_live = check_operation_context(context);
    if (!initial_live.ok()) {
        return initial_live;
    }
    if (payload.size() != kBlockSizeBytes) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"DATA pre-image fill must contain exactly one block"));
    }

    std::byte* destination = nullptr;
    {
        std::scoped_lock lock(state_->mutex);
        auto reservation_state = reservation.owner_state_.lock();
        if (reservation_state.get() == state_.get() &&
            (!state_->active_token.has_value() ||
             reservation.snapshot_id_ != state_->active_token->snapshot_id ||
             state_->active_state != ConsistentSnapshotState::Active)) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotAborted,
                L"snapshot ended before DATA pre-image fill"));
        }
        auto* record = state_->find_record(reservation.block_index_);
        if (reservation_state.get() != state_.get() ||
            reservation.disposition_ !=
                SnapshotPreimageDisposition::Reserved ||
            record == nullptr || record->committed || record->fill_active ||
            record->payload_filled ||
            record->reservation_id != reservation.reservation_id_ ||
            record->preimage.kind != DirtyRecordKind::Data ||
            record->payload.size() != kBlockSizeBytes) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"DATA pre-image fill reservation is missing or changed"));
        }
        record->fill_active = true;
        destination = record->payload.data();
    }

    if (test_observer != nullptr) {
        test_observer->on_snapshot_pressure_event(
            SnapshotPressureTestEvent::PreparingFillActive,
            reservation.block_index_);
    }

    bool fill_active = true;
    ScopeExit release_fill([&]() noexcept {
        if (!fill_active) {
            return;
        }
        try {
            std::scoped_lock lock(state_->mutex);
            auto* record = state_->find_record(reservation.block_index_);
            if (record != nullptr &&
                record->reservation_id == reservation.reservation_id_ &&
                record->fill_active) {
                record->fill_active = false;
                state_->condition.notify_all();
            }
        } catch (...) {
            std::terminate();
        }
    });

    std::copy(payload.begin(), payload.end(), destination);
    const auto copied_live = check_operation_context(context);
    if (!copied_live.ok()) {
        return copied_live;
    }

    {
        std::scoped_lock lock(state_->mutex);
        auto* record = state_->find_record(reservation.block_index_);
        if ((!state_->active_token.has_value() ||
             reservation.snapshot_id_ != state_->active_token->snapshot_id ||
             state_->active_state != ConsistentSnapshotState::Active) &&
            record == nullptr) {
            fill_active = false;
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotAborted,
                L"snapshot ended during DATA pre-image fill"));
        }
        if (record == nullptr || record->committed || !record->fill_active ||
            record->reservation_id != reservation.reservation_id_) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"DATA pre-image fill owner changed during copy"));
        }
        record->fill_active = false;
        fill_active = false;
        if (!state_->active_token.has_value() ||
            reservation.snapshot_id_ != state_->active_token->snapshot_id ||
            state_->active_state != ConsistentSnapshotState::Active) {
            state_->condition.notify_all();
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotAborted,
                L"snapshot ended during DATA pre-image fill"));
        }
        record->payload_filled = true;
        state_->condition.notify_all();
    }
    return Result<void>::success();
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
        record->fill_active ||
        record->reservation_id != reservation.reservation_id_ ||
        (record->preimage.kind == DirtyRecordKind::Data &&
         !record->payload_filled)) {
        reservation.owner_state_.reset();
        return Result<void>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"metadata reservation record is missing or changed"));
    }
    if (state_->active_state != ConsistentSnapshotState::Active) {
        state_->erase_record(record);
        reservation.owner_state_.reset();
        state_->finalize_explicit_abort_if_drained();
        state_->condition.notify_all();
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotAborted,
            L"snapshot ended before pre-image publication"));
    }

    state_->pause_for_test(
        SnapshotCoordinatorBarrierEvent::
            PreimagePublishBeforeFinalContextCheck);
    const auto publish_live = check_operation_context(context);
    if (!publish_live.ok()) {
        state_->erase_record(record);
        reservation.owner_state_.reset();
        state_->condition.notify_all();
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

Result<void> SnapshotCoordinator::wait_for_preparing_resolution(
    const OperationContext& context,
    std::uint64_t snapshot_id,
    std::uint64_t block_index,
    std::uint64_t reservation_id,
    ISnapshotPressureTestObserver* test_observer) {
    if (snapshot_id == 0 || reservation_id == 0) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"snapshot Preparing marker identity is zero"));
    }
    const auto initial_live = check_operation_context(context);
    if (!initial_live.ok()) {
        return initial_live;
    }

    std::unique_lock lock(state_->mutex);
    const auto resolved = [&]() noexcept {
        const auto* record = state_->find_record(block_index);
        return record == nullptr || record->committed ||
               record->reservation_id != reservation_id ||
               state_->closing || context.stop_token.stop_requested();
    };
    if (!resolved() && test_observer != nullptr) {
        lock.unlock();
        test_observer->on_snapshot_pressure_event(
            SnapshotPressureTestEvent::PreparingWaitReady,
            block_index);
        lock.lock();
    }
    if (context.deadline ==
        std::chrono::steady_clock::time_point::max()) {
        (void)state_->condition.wait(
            lock, context.stop_token, resolved);
    } else {
        (void)state_->condition.wait_until(
            lock, context.stop_token, context.deadline, resolved);
    }
    const auto live = check_operation_context(context);
    if (!live.ok()) {
        return live;
    }
    if (state_->closing) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotAborted,
            L"snapshot ended while waiting for Preparing marker"));
    }
    const auto* record = state_->find_record(block_index);
    if (record != nullptr && !record->committed &&
        record->reservation_id == reservation_id) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"Preparing wait ended before reservation resolution"));
    }
    return Result<void>::success();
}

Result<void> SnapshotCoordinator::wait_for_capacity_change(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    std::uint64_t required_bytes,
    std::uint64_t block_index,
    std::chrono::milliseconds stall_limit,
    ISnapshotPressureTestObserver* test_observer,
    std::uint64_t& capacity_grant_ticket) {
    capacity_grant_ticket = 0;
    if (required_bytes == 0 || required_bytes > state_->budget.max_bytes ||
        stall_limit.count() <= 0) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
            L"snapshot COW request cannot fit within the configured budget"));
    }

    std::unique_lock lock(state_->mutex);
    const auto capacity_available = [&]() noexcept {
        return state_->budget.current_bytes <= state_->budget.max_bytes &&
               required_bytes <=
                   state_->budget.max_bytes - state_->budget.current_bytes;
    };
    if (!state_->token_matches(token) ||
        state_->active_state != ConsistentSnapshotState::Active) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotAborted,
            L"snapshot ended before the capacity waiter registered"));
    }
    if (state_->next_capacity_ticket == 0 ||
        state_->next_capacity_ticket ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"snapshot capacity waiter ticket exhausted"));
    }
    const auto ticket = state_->next_capacity_ticket++;
    try {
        state_->capacity_waiters.push_back(
            SnapshotCoordinatorState::CapacityWaiter{
                ticket, required_bytes, block_index});
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
            L"snapshot capacity waiter registration allocation failed"));
    } catch (...) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"snapshot capacity waiter registration failed"));
    }
    bool registered = true;
    const auto unregister = [&]() noexcept {
        if (!registered) {
            return;
        }
        const auto found = std::find_if(
            state_->capacity_waiters.begin(),
            state_->capacity_waiters.end(),
            [ticket](const auto& waiter) {
                return waiter.ticket == ticket;
            });
        if (found == state_->capacity_waiters.end()) {
            std::terminate();
        }
        const bool removed_head = found == state_->capacity_waiters.begin();
        state_->capacity_waiters.erase(found);
        registered = false;
        if (removed_head && !state_->capacity_grant.has_value()) {
            state_->condition.notify_all();
        }
    };

    lock.unlock();
    if (test_observer != nullptr) {
        test_observer->on_snapshot_pressure_event(
            SnapshotPressureTestEvent::CapacityWaitReady,
            block_index);
    }
    lock.lock();

    const auto now = state_->now();
    const auto stall_deadline = now + stall_limit;
    const auto wait_deadline =
        (std::min)(context.deadline, stall_deadline);
    const auto wait_predicate = [&]() noexcept {
        const bool is_head = !state_->capacity_waiters.empty() &&
            state_->capacity_waiters.front().ticket == ticket;
        return (is_head && !state_->capacity_grant.has_value() &&
                capacity_available()) ||
               !state_->token_matches(token) ||
               state_->active_state != ConsistentSnapshotState::Active ||
               state_->closing || context.stop_token.stop_requested();
    };
    lock.unlock();
    if (test_observer != nullptr) {
        test_observer->on_snapshot_pressure_event(
            SnapshotPressureTestEvent::CapacityWaitSleeping,
            block_index);
    }
    lock.lock();
    if (state_->pressure_clock) {
        const auto fake_clock_wait_predicate = [&]() noexcept {
            return wait_predicate() || state_->now() >= wait_deadline;
        };
        (void)state_->condition.wait(
            lock, context.stop_token, fake_clock_wait_predicate);
    } else if (wait_deadline ==
               std::chrono::steady_clock::time_point::max()) {
        (void)state_->condition.wait(
            lock, context.stop_token, wait_predicate);
    } else {
        (void)state_->condition.wait_until(
            lock, context.stop_token, wait_deadline, wait_predicate);
    }

    const auto live = state_->pressure_clock
        ? (context.stop_token.stop_requested()
               ? Result<void>::failure(Error{
                     ErrorDomain::storage,
                     static_cast<std::uint32_t>(
                         OperationErrorCode::cancelled),
                     L"SnapshotCoordinator::wait_for_capacity_change",
                     L"operation cancelled"})
               : (state_->now() >= context.deadline
                      ? Result<void>::failure(Error{
                            ErrorDomain::storage,
                            static_cast<std::uint32_t>(
                                OperationErrorCode::timed_out),
                            L"SnapshotCoordinator::wait_for_capacity_change",
                            L"operation deadline expired"})
                      : Result<void>::success()))
        : check_operation_context(context);
    if (!live.ok()) {
        unregister();
        state_->finalize_explicit_abort_if_drained();
        return live;
    }
    if (!state_->token_matches(token) ||
        state_->active_state != ConsistentSnapshotState::Active ||
        state_->closing) {
        unregister();
        state_->finalize_explicit_abort_if_drained();
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotAborted,
            L"snapshot ended while waiting for COW capacity"));
    }
    const auto capacity_usable = [&]() noexcept {
        return capacity_available() && !state_->capacity_grant.has_value();
    };
    if (!capacity_usable() &&
        state_->now() >= stall_deadline) {
        state_->active_state = ConsistentSnapshotState::Aborted;
        state_->abort_reason =
            DynamicBlockStoreErrorCode::SnapshotResourceExhausted;
        if (state_->writer_stalls_total ==
                (std::numeric_limits<std::uint64_t>::max)() ||
            state_->auto_aborts_total ==
                (std::numeric_limits<std::uint64_t>::max)()) {
            std::terminate();
        }
        ++state_->writer_stalls_total;
        ++state_->auto_aborts_total;
        state_->capacity_grant.reset();
        unregister();
        state_->clear_committed_records();
        state_->condition.notify_all();
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotAborted,
            L"snapshot COW capacity made no progress before stall limit"));
    }
    if (!capacity_usable()) {
        unregister();
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"capacity wait ended while its predicate remained false"));
    }
    const bool is_head = !state_->capacity_waiters.empty() &&
        state_->capacity_waiters.front().ticket == ticket;
    if (!is_head || state_->capacity_grant.has_value()) {
        unregister();
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"capacity waiter woke without owning the FIFO grant"));
    }
    state_->capacity_grant =
        SnapshotCoordinatorState::CapacityGrant{ticket, block_index};
    unregister();
    capacity_grant_ticket = ticket;
    lock.unlock();
    if (test_observer != nullptr) {
        test_observer->on_snapshot_pressure_event(
            SnapshotPressureTestEvent::CapacityGrantReady,
            block_index);
    }
    return Result<void>::success();
}

void SnapshotCoordinator::release_capacity_grant(
    std::uint64_t capacity_grant_ticket) noexcept {
    try {
        if (capacity_grant_ticket == 0) {
            return;
        }
        std::scoped_lock lock(state_->mutex);
        if (state_->capacity_grant.has_value() &&
            state_->capacity_grant->ticket == capacity_grant_ticket) {
            state_->capacity_grant.reset();
            state_->finalize_explicit_abort_if_drained();
            state_->condition.notify_all();
        }
    } catch (...) {
        std::terminate();
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
    if (state_->active_state == ConsistentSnapshotState::Aborted) {
        return Result<void>::failure(coordinator_error(
            state_->abort_reason.value_or(
                DynamicBlockStoreErrorCode::SnapshotAborted),
            L"snapshot was aborted while a batch was staged"));
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

Result<SnapshotPublicationPin>
SnapshotCoordinator::acquire_publication_pin(
    const ConsistentSnapshotToken& token) {
    std::scoped_lock lock(state_->mutex);
    if (!state_->token_matches(token)) {
        return Result<SnapshotPublicationPin>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    if (state_->active_state != ConsistentSnapshotState::FullyScanned ||
        state_->closing || state_->publication_end_waiters != 0) {
        return Result<SnapshotPublicationPin>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotBusy,
            L"snapshot publication requires a fully scanned active token"));
    }
    if (state_->publication_pins ==
        (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<SnapshotPublicationPin>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"snapshot publication pin count overflowed"));
    }
    ++state_->publication_pins;
    return Result<SnapshotPublicationPin>::success(
        SnapshotPublicationPin{state_, token.snapshot_id});
}

Result<void> SnapshotCoordinator::finish(
    const ConsistentSnapshotToken& token,
    std::uint64_t total_blocks) {
    std::unique_lock lock(state_->mutex);
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
    if (state_->publication_pins != 0) {
        if (state_->publication_end_waiters ==
            (std::numeric_limits<std::uint64_t>::max)()) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot publication waiter count overflowed"));
        }
        ++state_->publication_end_waiters;
        ScopeExit remove_waiter([state = state_.get()]() noexcept {
            --state->publication_end_waiters;
            state->condition.notify_all();
        });
        state_->condition.wait(lock, [state = state_.get()] {
            return state->publication_pins == 0;
        });
        if (!state_->token_matches(token) || state_->closing ||
            state_->active_state !=
                ConsistentSnapshotState::FullyScanned) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotAborted,
                L"snapshot ended while waiting for publication pins"));
        }
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
    std::unique_lock lock(state_->mutex);
    if (!state_->token_matches(token)) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    if (state_->publication_pins != 0) {
        if (state_->publication_end_waiters ==
            (std::numeric_limits<std::uint64_t>::max)()) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot publication waiter count overflowed"));
        }
        ++state_->publication_end_waiters;
        ScopeExit remove_waiter([state = state_.get()]() noexcept {
            --state->publication_end_waiters;
            state->condition.notify_all();
        });
        state_->condition.wait(lock, [state = state_.get()] {
            return state->publication_pins == 0;
        });
        if (!state_->token_matches(token)) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                L"snapshot ended while waiting for publication pins"));
        }
    }
    state_->active_state = ConsistentSnapshotState::Aborted;
    state_->abort_reason = DynamicBlockStoreErrorCode::SnapshotAborted;
    state_->abort_finalize_requested = true;
    state_->capacity_grant.reset();
    state_->clear_committed_records();
    state_->condition.notify_all();
    state_->finalize_explicit_abort_if_drained();
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
            static_cast<std::uint64_t>(state_->records.size()),
            static_cast<std::uint64_t>(state_->capacity_waiters.size()),
        });
}

SnapshotCoordinatorSnapshot SnapshotCoordinator::snapshot() const {
    std::scoped_lock lock(state_->mutex);
    return SnapshotCoordinatorSnapshot{
        state_->active_token,
        state_->active_state,
        state_->abort_reason,
        state_->next_scan_block,
        state_->budget.current_bytes,
        state_->budget.peak_bytes,
        static_cast<std::uint64_t>(state_->records.size()),
        state_->pending_reservations,
        static_cast<std::uint64_t>(state_->capacity_waiters.size()),
        state_->writer_stalls_total,
        state_->auto_aborts_total,
        state_->publication_pins,
        state_->publication_end_waiters,
    };
}

bool SnapshotCoordinator::owns_pending_reservation(
    std::uint64_t snapshot_id,
    std::uint64_t block_index,
    std::uint64_t reservation_id) const {
    std::scoped_lock lock(state_->mutex);
    if (!state_->active_token.has_value() || snapshot_id == 0 ||
        reservation_id == 0 ||
        state_->active_token->snapshot_id != snapshot_id) {
        return false;
    }
    const auto* record = state_->find_record(block_index);
    return record != nullptr && !record->committed &&
           record->reservation_id == reservation_id;
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
               state_->abort_reason.has_value() ||
               state_->abort_finalize_requested ||
               !state_->capacity_waiters.empty() ||
               state_->capacity_grant.has_value() ||
               state_->publication_pins != 0 ||
               state_->publication_end_waiters != 0) {
        return invariant_failure(
            L"inactive snapshot retained coordinator state");
    }
    if (state_->publication_pins != 0 &&
        state_->active_state != ConsistentSnapshotState::FullyScanned &&
        !(state_->closing && state_->active_state ==
              ConsistentSnapshotState::Aborted)) {
        return invariant_failure(
            L"snapshot publication pin lacks a fully scanned owner");
    }

    std::uint64_t previous_ticket = 0;
    for (const auto& waiter : state_->capacity_waiters) {
        if (waiter.ticket == 0 || waiter.ticket <= previous_ticket ||
            waiter.required_bytes == 0 ||
            waiter.required_bytes > state_->budget.max_bytes) {
            return invariant_failure(
                L"snapshot capacity FIFO metadata is invalid");
        }
        previous_ticket = waiter.ticket;
    }
    if (state_->capacity_grant.has_value()) {
        if (state_->capacity_grant->ticket == 0 ||
            state_->closing ||
            state_->active_state != ConsistentSnapshotState::Active) {
            return invariant_failure(
                L"snapshot capacity grant outlived its active owner");
        }
        const auto duplicate = std::find_if(
            state_->capacity_waiters.begin(),
            state_->capacity_waiters.end(),
            [state = state_.get()](const auto& waiter) {
                return waiter.ticket == state->capacity_grant->ticket;
            });
        if (duplicate != state_->capacity_waiters.end()) {
            return invariant_failure(
                L"snapshot capacity grant remained in the FIFO queue");
        }
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
        if (metadata_record &&
            (record.fill_active || record.payload_filled)) {
            return invariant_failure(
                L"snapshot metadata record retained DATA fill state");
        }
        if (record.fill_active &&
            (record.committed || record.payload_filled || !data_record)) {
            return invariant_failure(
                L"snapshot active DATA fill state is invalid");
        }
        if (record.committed && data_record &&
            (!record.payload_filled || record.fill_active)) {
            return invariant_failure(
                L"snapshot committed DATA record is not fully populated");
        }
        if (record.payload_filled && !data_record) {
            return invariant_failure(
                L"snapshot payload fill flag belongs to metadata");
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

void SnapshotCoordinator::request_close() noexcept {
    try {
        std::scoped_lock lock(state_->mutex);
        state_->closing = true;
        state_->capacity_grant.reset();
        if (state_->active_token.has_value()) {
            state_->active_state = ConsistentSnapshotState::Aborted;
            state_->abort_reason =
                DynamicBlockStoreErrorCode::SnapshotAborted;
        }
        state_->condition.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void SnapshotCoordinator::finalize_close() noexcept {
    try {
        std::scoped_lock lock(state_->mutex);
        if (!state_->closing || !state_->capacity_waiters.empty() ||
            state_->publication_pins != 0 ||
            state_->publication_end_waiters != 0) {
            std::terminate();
        }
        state_->clear_records();
        state_->capacity_grant.reset();
        state_->active_token.reset();
        state_->active_state.reset();
        state_->abort_reason.reset();
        state_->next_scan_block = 0;
        state_->abort_finalize_requested = false;
        state_->condition.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void SnapshotCoordinator::notify_capacity_waiters_for_test() noexcept {
    state_->condition.notify_all();
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
                finalize_explicit_abort_if_drained();
                condition.notify_all();
            }
            reservation.owner_state_.reset();
        }
    } catch (...) {
        std::terminate();
    }
}

} // namespace myrd
