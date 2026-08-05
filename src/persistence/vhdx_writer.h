#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/file_io.h"
#include "src/persistence/vhdx_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#if defined(MYRD_ENABLE_TEST_SEAMS)
#include <functional>
#include <filesystem>
#endif
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::persistence {

class BundleWriterLock;
class ValidatedPersistenceFile;
#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace detail {
class INativeRenameBackend;
class INativeSparseBackend;
}
#endif

struct VhdxWriterGeometry final {
    std::uint64_t virtual_size{};
    std::uint32_t logical_sector_size{};
};

struct VhdxWriterIdentity final {
    VhdxFileIdentity file_identity{};
    std::array<std::byte, 16> virtual_disk_id{};
};

struct VhdxWriterLimits final {
    std::uint64_t max_candidate_payloads{};
    std::uint64_t max_allocated_payloads{};
    std::uint64_t scratch_bytes{};
};

class IVhdxPayloadSource {
public:
    virtual ~IVhdxPayloadSource() = default;

    virtual Result<void> read_payload(
        const OperationContext& context,
        std::uint64_t payload_index,
        std::span<std::byte> destination) const = 0;
};

struct VhdxPlannedPayload final {
    std::uint64_t payload_index{};
    std::uint64_t file_offset{};
    Sha256Digest digest{};
};

class VhdxWritePlan final {
public:
    VhdxWritePlan(const VhdxWritePlan&) = default;
    VhdxWritePlan(VhdxWritePlan&&) noexcept = default;
    VhdxWritePlan& operator=(const VhdxWritePlan&) = default;
    VhdxWritePlan& operator=(VhdxWritePlan&&) noexcept = default;

    [[nodiscard]] std::uint64_t virtual_size() const noexcept;
    [[nodiscard]] std::uint32_t logical_sector_size() const noexcept;
    [[nodiscard]] std::uint64_t payload_block_count() const noexcept;
    [[nodiscard]] std::uint64_t chunk_ratio() const noexcept;
    [[nodiscard]] std::uint64_t bat_entry_count() const noexcept;
    [[nodiscard]] std::uint64_t bat_offset() const noexcept;
    [[nodiscard]] std::uint64_t bat_length() const noexcept;
    [[nodiscard]] std::uint64_t metadata_offset() const noexcept;
    [[nodiscard]] std::uint64_t log_offset() const noexcept;
    [[nodiscard]] std::uint64_t payload_offset() const noexcept;
    [[nodiscard]] std::uint64_t file_length() const noexcept;
    [[nodiscard]] const VhdxWriterIdentity& identity() const noexcept;
    [[nodiscard]] const std::vector<VhdxPlannedPayload>& payloads() const noexcept;

private:
    struct PlannedCandidate final {
        std::uint64_t payload_index{};
        std::uint64_t file_offset{};
        Sha256Digest digest{};
        bool allocated{};
    };

    VhdxWriterGeometry geometry_{};
    VhdxWriterIdentity identity_{};
    std::uint64_t payload_block_count_{};
    std::uint64_t chunk_ratio_{};
    std::uint64_t bat_entry_count_{};
    std::uint64_t bat_offset_{};
    std::uint64_t bat_length_{};
    std::uint64_t metadata_offset_{};
    std::uint64_t log_offset_{};
    std::uint64_t payload_offset_{};
    std::uint64_t file_length_{};
    std::vector<VhdxPlannedPayload> payloads_;
    std::vector<PlannedCandidate> candidates_;

    VhdxWritePlan() = default;

    friend Result<VhdxWritePlan> plan_sparse_vhdx(
        const OperationContext&,
        VhdxWriterGeometry,
        VhdxWriterIdentity,
        std::span<const std::uint64_t>,
        const IVhdxPayloadSource&,
        VhdxWriterLimits);
    friend Result<class VhdxWrittenFile> write_sparse_vhdx_file(
        const OperationContext&,
        const VhdxWritePlan&,
        const IVhdxPayloadSource&,
        std::unique_ptr<IPersistenceFile>);
};

[[nodiscard]] Result<VhdxWritePlan> plan_sparse_vhdx(
    const OperationContext& context,
    VhdxWriterGeometry geometry,
    VhdxWriterIdentity identity,
    std::span<const std::uint64_t> candidate_payloads,
    const IVhdxPayloadSource& source,
    VhdxWriterLimits limits);

class VhdxWrittenFile final {
public:
    VhdxWrittenFile(VhdxWrittenFile&&) noexcept = default;
    VhdxWrittenFile& operator=(VhdxWrittenFile&&) noexcept = default;
    VhdxWrittenFile(const VhdxWrittenFile&) = delete;
    VhdxWrittenFile& operator=(const VhdxWrittenFile&) = delete;

    [[nodiscard]] const VhdxLayout& layout() const noexcept;
    [[nodiscard]] std::unique_ptr<IPersistenceFile> release_file() noexcept;

private:
    std::unique_ptr<IPersistenceFile> file_;
    VhdxLayout layout_;

    VhdxWrittenFile(
        std::unique_ptr<IPersistenceFile> file,
        VhdxLayout layout) noexcept;

    friend Result<VhdxWrittenFile> write_sparse_vhdx_file(
        const OperationContext&,
        const VhdxWritePlan&,
        const IVhdxPayloadSource&,
        std::unique_ptr<IPersistenceFile>);
};

[[nodiscard]] Result<VhdxWrittenFile> write_sparse_vhdx_file(
    const OperationContext& context,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source,
    std::unique_ptr<IPersistenceFile> temporary_file);

class VhdxPublishedBase final {
public:
    VhdxPublishedBase(VhdxPublishedBase&&) noexcept = default;
    VhdxPublishedBase& operator=(VhdxPublishedBase&&) noexcept = default;
    VhdxPublishedBase(const VhdxPublishedBase&) = delete;
    VhdxPublishedBase& operator=(const VhdxPublishedBase&) = delete;

    [[nodiscard]] const std::string& relative_name() const noexcept;
    [[nodiscard]] const WindowsFileIdentity& windows_identity() const noexcept;
    [[nodiscard]] const VhdxLayout& layout() const noexcept;

private:
    std::string relative_name_;
    WindowsFileIdentity windows_identity_{};
    VhdxLayout layout_;

    VhdxPublishedBase(
        std::string relative_name,
        WindowsFileIdentity windows_identity,
        VhdxLayout layout) noexcept;

    friend Result<VhdxPublishedBase> publish_sparse_vhdx_base(
        const OperationContext&,
        BundleWriterLock&,
        std::string_view,
        std::string_view,
        const VhdxWritePlan&,
        const IVhdxPayloadSource&);
};

[[nodiscard]] Result<VhdxPublishedBase> publish_sparse_vhdx_base(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    std::string_view temporary_name,
    std::string_view final_name,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source);

enum class VhdxPublicationPhase : std::uint8_t {
    BeforeCreate,
    BeforeSparseMark,
    BeforeClose,
    BeforeReopen,
    BeforeRename,
};

enum class VhdxFileWritePhase : std::uint8_t {
    BeforeExtend,
    BeforeStructure,
    BeforeMetadata,
    BeforeBat,
    BeforePayload,
    BeforeFlush,
    BeforeReadback,
};

#if defined(MYRD_ENABLE_TEST_SEAMS)
using VhdxPublicationHook =
    std::function<Result<void>(VhdxPublicationPhase)>;
using VhdxFileWriteHook =
    std::function<Result<void>(VhdxFileWritePhase)>;

struct VhdxPublicationTestOptions final {
    VhdxPublicationHook phase_hook{};
    VhdxFileWriteHook file_write_hook{};
    detail::INativeSparseBackend* sparse_backend{};
    detail::INativeRenameBackend* rename_backend{};
    std::function<Result<void>(ValidatedPersistenceFile&)>
        close_operation{};
    std::function<Result<void>(const std::filesystem::path&)>
        before_reopen_operation{};
};

// Deterministic failure seam compiled only for tests. The hook runs directly
// before the named product publication operation and cannot bypass validation.
[[nodiscard]] Result<VhdxPublishedBase>
publish_sparse_vhdx_base_for_testing(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    std::string_view temporary_name,
    std::string_view final_name,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source,
    VhdxPublicationHook hook);

[[nodiscard]] Result<VhdxPublishedBase>
publish_sparse_vhdx_base_for_testing(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    std::string_view temporary_name,
    std::string_view final_name,
    const VhdxWritePlan& plan,
    const IVhdxPayloadSource& source,
    VhdxPublicationTestOptions options);
#endif

}  // namespace myrd::persistence
