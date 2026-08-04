#include "snapshot_coordinator.h"

#include <limits>
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

struct SnapshotRecordBudget final {
    explicit SnapshotRecordBudget(std::uint64_t limit) noexcept
        : max_bytes(limit) {}

    void reserve(std::size_t count) {
        if (count == 0 ||
            count > (std::numeric_limits<std::uint64_t>::max)() /
                        kSnapshotCowRecordBudgetBytes) {
            throw SnapshotBudgetExhausted{};
        }
        const auto bytes = static_cast<std::uint64_t>(count) *
                           kSnapshotCowRecordBudgetBytes;
        if (current_bytes > max_bytes ||
            bytes > max_bytes - current_bytes) {
            throw SnapshotBudgetExhausted{};
        }
        current_bytes += bytes;
        if (peak_bytes < current_bytes) {
            peak_bytes = current_bytes;
        }
    }

    void release(std::size_t count) noexcept {
        const auto bytes = static_cast<std::uint64_t>(count) *
                           kSnapshotCowRecordBudgetBytes;
        if (count == 0 || current_bytes < bytes) {
            std::terminate();
        }
        current_bytes -= bytes;
    }

    std::uint64_t max_bytes{};
    std::uint64_t current_bytes{};
    std::uint64_t peak_bytes{};
};

} // namespace

class SnapshotCoordinator::Impl final {
public:
    struct Record final {
        SnapshotPreimage preimage;
        std::uint64_t reservation_id{};
        bool committed{};
        Record* next{};
    };

    static_assert(
        sizeof(Record) <= kSnapshotCowRecordBudgetBytes,
        "snapshot record exceeds its frozen bookkeeping slab");

    Impl(
        std::uint64_t max_bytes,
        std::shared_ptr<ISnapshotCoordinatorObserver> next_observer)
        : budget(max_bytes),
          observer(std::move(next_observer)) {}

    ~Impl() noexcept {
        clear_records();
    }

    bool token_matches(const ConsistentSnapshotToken& token) const noexcept {
        return active_token.has_value() &&
               same_token(*active_token, token);
    }

    void notify(SnapshotCoordinatorEvent event) const noexcept {
        if (observer) {
            observer->on_event(event);
        }
    }

    Record* find_record(std::uint64_t block_index) const noexcept {
        auto* record = records;
        while (record != nullptr &&
               record->preimage.block_index < block_index) {
            record = record->next;
        }
        return record != nullptr &&
                       record->preimage.block_index == block_index
            ? record
            : nullptr;
    }

    Record* allocate_record(
        const SnapshotPreimage& metadata,
        std::uint64_t reservation_id) {
        budget.reserve(1);
        void* slab = nullptr;
        try {
            slab = ::operator new(
                static_cast<std::size_t>(
                    kSnapshotCowRecordBudgetBytes));
            return std::construct_at(
                static_cast<Record*>(slab),
                Record{metadata, reservation_id, false, nullptr});
        } catch (...) {
            if (slab != nullptr) {
                ::operator delete(slab);
            }
            budget.release(1);
            throw;
        }
    }

    void insert_record(Record* record) noexcept {
        auto** link = &records;
        while (*link != nullptr &&
               (*link)->preimage.block_index <
                   record->preimage.block_index) {
            link = &(*link)->next;
        }
        if (*link != nullptr &&
            (*link)->preimage.block_index ==
                record->preimage.block_index) {
            std::terminate();
        }
        record->next = *link;
        *link = record;
    }

    void erase_record(Record* record) noexcept {
        auto** link = &records;
        while (*link != nullptr && *link != record) {
            link = &(*link)->next;
        }
        if (*link != record) {
            std::terminate();
        }
        *link = record->next;
        if (record->committed) {
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
        std::destroy_at(record);
        ::operator delete(record);
        budget.release(1);
    }

    void clear_records() noexcept {
        while (records != nullptr) {
            erase_record(records);
        }
        if (committed_records != 0 || pending_reservations != 0) {
            std::terminate();
        }
        if (budget.current_bytes != 0) {
            std::terminate();
        }
    }

    mutable std::mutex mutex;
    SnapshotRecordBudget budget;
    Record* records{};
    std::shared_ptr<ISnapshotCoordinatorObserver> observer;
    std::optional<ConsistentSnapshotToken> active_token;
    std::optional<ConsistentSnapshotState> active_state;
    std::uint64_t next_snapshot_id{1};
    std::uint64_t next_reservation_id{1};
    std::uint64_t next_scan_block{};
    std::uint64_t committed_records{};
    std::uint64_t pending_reservations{};
};

SnapshotMetadataReservation::SnapshotMetadataReservation(
    SnapshotPreimageDisposition disposition,
    SnapshotCoordinator* owner,
    std::uint64_t snapshot_id,
    std::uint64_t block_index,
    std::uint64_t reservation_id) noexcept
    : disposition_(disposition),
      owner_(owner),
      snapshot_id_(snapshot_id),
      block_index_(block_index),
      reservation_id_(reservation_id) {}

SnapshotMetadataReservation::~SnapshotMetadataReservation() noexcept {
    reset();
}

SnapshotMetadataReservation::SnapshotMetadataReservation(
    SnapshotMetadataReservation&& other) noexcept
    : disposition_(other.disposition_),
      owner_(other.owner_),
      snapshot_id_(other.snapshot_id_),
      block_index_(other.block_index_),
      reservation_id_(other.reservation_id_) {
    other.owner_ = nullptr;
}

SnapshotMetadataReservation& SnapshotMetadataReservation::operator=(
    SnapshotMetadataReservation&& other) noexcept {
    if (this != &other) {
        reset();
        disposition_ = other.disposition_;
        owner_ = other.owner_;
        snapshot_id_ = other.snapshot_id_;
        block_index_ = other.block_index_;
        reservation_id_ = other.reservation_id_;
        other.owner_ = nullptr;
    }
    return *this;
}

void SnapshotMetadataReservation::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->abandon(*this);
    }
}

SnapshotCoordinator::SnapshotCoordinator(
    std::uint64_t max_bytes,
    std::shared_ptr<ISnapshotCoordinatorObserver> observer)
    : impl_(std::make_unique<Impl>(max_bytes, std::move(observer))) {}

SnapshotCoordinator::~SnapshotCoordinator() noexcept = default;

Result<ConsistentSnapshotToken> SnapshotCoordinator::activate(
    BackingGeneration source_backing_generation,
    std::uint64_t cutoff_dirty_epoch) {
    std::scoped_lock lock(impl_->mutex);
    impl_->notify(SnapshotCoordinatorEvent::ActivateLocked);
    if (impl_->active_token.has_value()) {
        return Result<ConsistentSnapshotToken>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotBusy,
                L"another consistent snapshot owns the coordinator"));
    }
    if (impl_->records != nullptr || impl_->budget.current_bytes != 0 ||
        impl_->pending_reservations != 0 ||
        impl_->committed_records != 0) {
        return Result<ConsistentSnapshotToken>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"inactive coordinator retained COW resources"));
    }
    if (impl_->next_snapshot_id == 0 ||
        impl_->next_snapshot_id ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<ConsistentSnapshotToken>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"consistent snapshot id exhausted"));
    }

    const ConsistentSnapshotToken token{
        impl_->next_snapshot_id++,
        source_backing_generation,
        cutoff_dirty_epoch,
    };
    impl_->active_token = token;
    impl_->active_state = ConsistentSnapshotState::Active;
    impl_->next_scan_block = 0;
    return Result<ConsistentSnapshotToken>::success(token);
}

Result<SnapshotMetadataReservation>
SnapshotCoordinator::reserve_metadata_before_mutation(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    const SnapshotPreimage& metadata) {
    const auto initial_live = check_operation_context(context);
    if (!initial_live.ok()) {
        return Result<SnapshotMetadataReservation>::failure(
            initial_live.error());
    }

    std::scoped_lock lock(impl_->mutex);
    if (!impl_->token_matches(token)) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                L"snapshot token does not own this coordinator"));
    }
    if (impl_->active_state != ConsistentSnapshotState::Active) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotAborted,
                L"snapshot is not active"));
    }
    if (metadata.kind == DirtyRecordKind::Data || !metadata.data.empty()) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"DATA pre-image payload preservation belongs to M3 Task04"));
    }
    if (metadata.dirty_epoch == 0 ||
        metadata.dirty_epoch > token.cutoff_dirty_epoch ||
        metadata.base_generation > token.source_backing_generation) {
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::NotMember,
                nullptr,
                0,
                0,
                0});
    }
    if (metadata.block_index < impl_->next_scan_block) {
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::Consumed,
                nullptr,
                0,
                0,
                0});
    }
    if (impl_->find_record(metadata.block_index) != nullptr) {
        return Result<SnapshotMetadataReservation>::success(
            SnapshotMetadataReservation{
                SnapshotPreimageDisposition::AlreadyPreserved,
                nullptr,
                0,
                0,
                0});
    }
    if (impl_->next_reservation_id == 0 ||
        impl_->next_reservation_id ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot reservation id exhausted"));
    }

    const auto reservation_id = impl_->next_reservation_id++;
    try {
        // The actual 256-byte slab contains the metadata record, reservation
        // bookkeeping and ordered linked-index node. DATA payload allocation
        // is deliberately absent until Task04.
        auto* record = impl_->allocate_record(metadata, reservation_id);
        impl_->insert_record(record);
        ++impl_->pending_reservations;
        impl_->notify(SnapshotCoordinatorEvent::ReservationInserted);

        const auto reserved_live = check_operation_context(context);
        if (!reserved_live.ok()) {
            impl_->erase_record(record);
            return Result<SnapshotMetadataReservation>::failure(
                reserved_live.error());
        }
    } catch (const SnapshotBudgetExhausted&) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
                L"snapshot COW metadata budget is exhausted"));
    } catch (const std::bad_alloc&) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotResourceExhausted,
                L"snapshot COW metadata allocation failed"));
    } catch (...) {
        return Result<SnapshotMetadataReservation>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"snapshot COW metadata insertion failed"));
    }

    return Result<SnapshotMetadataReservation>::success(
        SnapshotMetadataReservation{
            SnapshotPreimageDisposition::Reserved,
            this,
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

    std::scoped_lock lock(impl_->mutex);
    if (reservation.owner_ != this) {
        return Result<void>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                L"metadata reservation belongs to another coordinator"));
    }
    if (reservation.disposition_ !=
            SnapshotPreimageDisposition::Reserved ||
        !impl_->active_token.has_value() ||
        reservation.snapshot_id_ != impl_->active_token->snapshot_id) {
        reservation.owner_ = nullptr;
        return Result<void>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
                L"metadata reservation does not belong to active token"));
    }
    auto* record = impl_->find_record(reservation.block_index_);
    if (record == nullptr || record->committed ||
        record->reservation_id != reservation.reservation_id_) {
        reservation.owner_ = nullptr;
        return Result<void>::failure(
            coordinator_error(
                DynamicBlockStoreErrorCode::FatalInternalInvariant,
                L"metadata reservation record is missing or changed"));
    }

    const auto publish_live = check_operation_context(context);
    if (!publish_live.ok()) {
        impl_->erase_record(record);
        reservation.owner_ = nullptr;
        return Result<void>::failure(publish_live.error());
    }
    if (impl_->pending_reservations == 0) {
        std::terminate();
    }
    --impl_->pending_reservations;
    ++impl_->committed_records;
    record->committed = true;
    reservation.owner_ = nullptr;
    return Result<void>::success();
}

void SnapshotCoordinator::rollback_metadata_preimage(
    SnapshotMetadataReservation& reservation) noexcept {
    if (reservation.owner_ == this) {
        abandon(reservation);
    }
}

std::optional<SnapshotPreimage> SnapshotCoordinator::peek_preimage(
    std::uint64_t block_index) const {
    std::scoped_lock lock(impl_->mutex);
    const auto* record = impl_->find_record(block_index);
    if (record == nullptr ||
        !record->committed ||
        block_index < impl_->next_scan_block) {
        return std::nullopt;
    }
    return record->preimage;
}

Result<void> SnapshotCoordinator::advance_scan_frontier(
    const ConsistentSnapshotToken& token,
    std::uint64_t next_scan_block,
    std::uint64_t total_blocks) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->token_matches(token)) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    if (next_scan_block < impl_->next_scan_block ||
        next_scan_block > total_blocks) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::FatalInternalInvariant,
            L"snapshot frontier must advance monotonically within geometry"));
    }
    for (auto* record = impl_->records;
         record != nullptr &&
         record->preimage.block_index < next_scan_block;
         record = record->next) {
        if (!record->committed) {
            return Result<void>::failure(coordinator_error(
                DynamicBlockStoreErrorCode::SnapshotBusy,
                L"snapshot frontier reached a pending reservation"));
        }
    }
    while (impl_->records != nullptr &&
           impl_->records->preimage.block_index < next_scan_block) {
        impl_->erase_record(impl_->records);
    }
    impl_->next_scan_block = next_scan_block;
    if (next_scan_block == total_blocks) {
        impl_->active_state = ConsistentSnapshotState::FullyScanned;
    }
    return Result<void>::success();
}

Result<void> SnapshotCoordinator::finish(
    const ConsistentSnapshotToken& token,
    std::uint64_t total_blocks) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->token_matches(token)) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    if (impl_->active_state != ConsistentSnapshotState::FullyScanned ||
        impl_->next_scan_block != total_blocks) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotNotFullyScanned,
            L"snapshot cannot finish before its frontier reaches the end"));
    }
    impl_->clear_records();
    impl_->active_token.reset();
    impl_->active_state.reset();
    impl_->next_scan_block = 0;
    return Result<void>::success();
}

Result<void> SnapshotCoordinator::abort(
    const ConsistentSnapshotToken& token) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->token_matches(token)) {
        return Result<void>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    impl_->clear_records();
    impl_->active_token.reset();
    impl_->active_state.reset();
    impl_->next_scan_block = 0;
    return Result<void>::success();
}

Result<ConsistentSnapshotStatus> SnapshotCoordinator::status(
    const ConsistentSnapshotToken& token) const {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->token_matches(token) || !impl_->active_state.has_value()) {
        return Result<ConsistentSnapshotStatus>::failure(coordinator_error(
            DynamicBlockStoreErrorCode::SnapshotTokenInvalid,
            L"snapshot token does not own this coordinator"));
    }
    return Result<ConsistentSnapshotStatus>::success(
        ConsistentSnapshotStatus{
            *impl_->active_state,
            impl_->budget.current_bytes,
            impl_->committed_records,
            0,
        });
}

SnapshotCoordinatorSnapshot SnapshotCoordinator::snapshot() const {
    std::scoped_lock lock(impl_->mutex);
    return SnapshotCoordinatorSnapshot{
        impl_->active_token,
        impl_->active_state,
        impl_->next_scan_block,
        impl_->budget.current_bytes,
        impl_->budget.peak_bytes,
        impl_->committed_records,
        impl_->pending_reservations,
    };
}

void SnapshotCoordinator::abandon(
    SnapshotMetadataReservation& reservation) noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        if (reservation.owner_ == this) {
            auto* record = impl_->find_record(
                reservation.block_index_);
            if (record != nullptr &&
                !record->committed &&
                record->reservation_id ==
                    reservation.reservation_id_) {
                impl_->erase_record(record);
            }
            reservation.owner_ = nullptr;
        }
    } catch (...) {
        std::terminate();
    }
}

} // namespace myrd
