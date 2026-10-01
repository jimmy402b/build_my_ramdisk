#pragma once

#include "src/common/result.h"
#include "src/profile/profile_store.h"
#include "src/product/uuid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::management {

inline constexpr std::array<std::byte, 8> kOperationMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'O'}, std::byte{'P'}, std::byte{'R'}, std::byte{0}};
inline constexpr std::uint16_t kOperationSchemaMajor = 1;
inline constexpr std::uint16_t kOperationSchemaMinor = 0;
inline constexpr std::uint32_t kOperationHeaderBytes = 72;
inline constexpr std::uint32_t kOperationFieldCount = 15;
inline constexpr std::uint32_t kOperationMaximumBytes = 256U * 1024U;
inline constexpr std::uint32_t kOperationMaximumTerminalResultBytes =
    192U * 1024U;
inline constexpr std::uint64_t kOperationRetentionMilliseconds =
    7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
inline constexpr std::size_t kOperationMaximumTerminalRecords = 10'000;

enum class OperationState : std::uint8_t {
    queued = 0,
    running = 1,
    reconciling = 2,
    succeeded = 3,
    failed = 4,
    cancelled = 5,
};

enum class OperationStoreError : std::uint32_t {
    invalid_size = 0x5101,
    invalid_magic,
    unsupported_schema,
    invalid_header,
    checksum_mismatch,
    invalid_fields,
    invalid_record,
    revision_conflict,
    request_id_conflict,
    operation_not_found,
    cancellation_too_late,
    operation_terminal,
    storage_corrupt,
    retention_blocked,
    io_failure,
    commit_indeterminate,
    // A caller-supplied operation identifier is already owned by another
    // record.  Appended so no existing durable error classification moves.
    operation_id_conflict,
};

struct OperationRecord final {
    std::uint64_t revision{};
    product::Uuid request_id;
    product::Uuid operation_id;
    product::Uuid profile_id;
    std::wstring caller_sid;
    std::string command_scope;
    std::array<std::byte, 32> argument_digest{};
    OperationState state{OperationState::queued};
    std::string phase;
    std::uint64_t created_unix_ms{};
    std::uint64_t updated_unix_ms{};
    std::optional<std::uint64_t> terminal_unix_ms;
    bool cancellable{true};
    bool cancel_requested{};
    std::vector<std::byte> terminal_result;
    bool operator==(const OperationRecord&) const noexcept = default;
};

Result<std::vector<std::byte>> encode_operation_record(
    const OperationRecord& record);
Result<OperationRecord> decode_operation_record(
    std::span<const std::byte> bytes);

struct OperationStorePaths final {
    std::wstring operations_root;
};

struct OperationRequest final {
    product::Uuid request_id;
    product::Uuid profile_id;
    std::wstring caller_sid;
    std::string command_scope;
    std::array<std::byte, 32> argument_digest{};
    std::uint64_t created_unix_ms{};
};

struct OperationAdmission final {
    OperationRecord record;
    bool replayed{};
};

struct OperationRetentionResult final {
    std::vector<product::Uuid> removed_operation_ids;
    std::size_t remaining_terminal_records{};
};

Result<std::vector<product::Uuid>> select_operation_retention_candidates(
    std::span<const OperationRecord> records,
    std::uint64_t now_unix_ms);

class OperationStore final {
public:
    static Result<OperationStore> open_existing(
        std::wstring_view trusted_program_data_root);
    static Result<OperationStore> from_directory_capability(
        std::unique_ptr<profile::IAtomicFileDirectory> directory,
        OperationStorePaths paths,
        std::wstring gate_key);

    OperationStore(OperationStore&&) noexcept;
    OperationStore& operator=(OperationStore&&) noexcept;
    ~OperationStore() noexcept;

    OperationStore(const OperationStore&) = delete;
    OperationStore& operator=(const OperationStore&) = delete;

    const OperationStorePaths& paths() const noexcept;
    Result<void> write_record(
        std::uint64_t expected_revision,
        const OperationRecord& proposed);
    Result<OperationRecord> load_record(
        const product::Uuid& operation_id);
    // Resolves the record a request already owns, repairing the request index
    // across restarts.  `nullopt` means the request was never admitted; the
    // lookup never creates a record.  A reused request identifier carrying
    // different arguments is a conflict, not an absent request.
    Result<std::optional<OperationRecord>> find_request(
        const OperationRequest& request);
    Result<OperationAdmission> admit(const OperationRequest& request);
    // Admission under a caller-supplied operation identifier.  The caller
    // mints the identifier because it must write the operation gate and the
    // lifecycle reservation under the same identifier before this call, so the
    // durable record has to carry it.  A replay still wins: an identifier is
    // never minted twice for one request, and a taken identifier is refused
    // rather than overwritten.
    Result<OperationAdmission> admit_with_id(
        const OperationRequest& request,
        const product::Uuid& candidate_operation_id);
    Result<std::vector<OperationRecord>> list_records();
    Result<std::vector<OperationRecord>> reconcile_after_restart(
        std::uint64_t now_unix_ms);
    Result<OperationRecord> request_cancel(
        const product::Uuid& operation_id,
        std::uint64_t now_unix_ms);
    Result<OperationRetentionResult> cleanup_terminal_records(
        std::uint64_t now_unix_ms);

private:
    struct Impl;
    explicit OperationStore(std::unique_ptr<Impl> impl) noexcept;
    Result<std::optional<OperationRecord>> find_request_locked(
        const OperationRequest& request);
    Result<OperationRecord> load_record_locked(
        const product::Uuid& operation_id);
    Result<void> write_record_locked(
        std::uint64_t expected_revision,
        const OperationRecord& proposed);
    Result<std::vector<OperationRecord>> list_records_locked();
    Result<void> cleanup_orphan_temps_locked();
    Result<void> rebuild_index_locked();
    std::unique_ptr<Impl> impl_;
};

} // namespace myrd::management
