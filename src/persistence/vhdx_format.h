#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/file_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace myrd::persistence {

struct VhdxParseEvidence;

// Immutable, validated description of the M3 v1 VHDX subset.  Construction
// is intentionally limited to parse_vhdx_layout(); later readers can retain
// the exact file object that was structurally validated.
class VhdxLayout final {
public:
    VhdxLayout(const VhdxLayout&) = default;
    VhdxLayout(VhdxLayout&&) noexcept = default;
    VhdxLayout& operator=(const VhdxLayout&) = default;
    VhdxLayout& operator=(VhdxLayout&&) noexcept = default;

    [[nodiscard]] std::uint64_t virtual_size() const noexcept;
    [[nodiscard]] std::uint32_t logical_sector_size() const noexcept;
    [[nodiscard]] std::uint32_t physical_sector_size() const noexcept;
    [[nodiscard]] std::uint32_t block_size() const noexcept;
    [[nodiscard]] std::uint64_t payload_block_count() const noexcept;
    [[nodiscard]] std::uint64_t chunk_ratio() const noexcept;
    [[nodiscard]] std::uint64_t bat_entry_count() const noexcept;
    [[nodiscard]] std::uint64_t bat_offset() const noexcept;
    [[nodiscard]] std::uint64_t bat_length() const noexcept;
    [[nodiscard]] std::uint64_t metadata_offset() const noexcept;
    [[nodiscard]] std::uint64_t metadata_length() const noexcept;
    [[nodiscard]] std::uint64_t log_offset() const noexcept;
    [[nodiscard]] std::uint64_t log_length() const noexcept;
    [[nodiscard]] std::uint64_t file_length() const noexcept;
    [[nodiscard]] const VhdxFileIdentity& file_identity() const noexcept;
    [[nodiscard]] const std::array<std::byte, 16>&
    virtual_disk_id() const noexcept;

private:
    friend Result<VhdxLayout> parse_vhdx_layout(
        const OperationContext&, std::shared_ptr<IPersistenceFile>);
    friend Result<VhdxLayout> parse_vhdx_layout(
        const OperationContext&, std::shared_ptr<IPersistenceFile>,
        VhdxParseEvidence*);

    VhdxLayout() = default;

    VhdxFileIdentity file_identity_{};
    std::array<std::byte, 16> virtual_disk_id_{};
    std::uint64_t virtual_size_{};
    std::uint64_t payload_block_count_{};
    std::uint64_t chunk_ratio_{};
    std::uint64_t bat_entry_count_{};
    std::uint64_t bat_offset_{};
    std::uint64_t bat_length_{};
    std::uint64_t metadata_offset_{};
    std::uint64_t metadata_length_{};
    std::uint64_t log_offset_{};
    std::uint64_t log_length_{};
    std::uint64_t file_length_{};
    std::uint32_t logical_sector_size_{};
    std::uint32_t physical_sector_size_{};
    std::uint32_t block_size_{};
};

enum class VhdxParseStage : std::uint8_t {
    Identity,
    Profile,
    Layout,
    Complete,
};

// Optional staged evidence for callers that must preserve a stable
// Identity -> ExpectedLength -> Profile -> Layout diagnostic order without
// reparsing or reopening the validated file.
struct VhdxParseEvidence final {
    VhdxParseStage stage{VhdxParseStage::Identity};
    std::optional<VhdxFileIdentity> identity{};
    std::optional<std::uint64_t> file_length{};
    std::uint64_t structural_read_calls{};
    std::uint64_t structural_read_bytes{};
};

[[nodiscard]] Result<VhdxLayout> parse_vhdx_layout(
    const OperationContext& context,
    std::shared_ptr<IPersistenceFile> file);

[[nodiscard]] Result<VhdxLayout> parse_vhdx_layout(
    const OperationContext& context,
    std::shared_ptr<IPersistenceFile> file,
    VhdxParseEvidence* evidence);

// Minimal identity probe for classifying a structurally valid but unsupported
// profile. It validates identifier, header checksums/sequence/reserved bytes
// and non-nil identity GUIDs, but deliberately does not accept a VHDX layout.
[[nodiscard]] Result<VhdxFileIdentity> probe_vhdx_file_identity(
    const OperationContext& context,
    std::shared_ptr<IPersistenceFile> file);

}  // namespace myrd::persistence
