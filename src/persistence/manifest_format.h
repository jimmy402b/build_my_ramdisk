#pragma once

#include "src/common/result.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/persistence_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>

namespace myrd::persistence {

using ManifestSlotBytes = std::array<std::byte, kManifestSlotBytes>;

inline constexpr std::array<std::byte, 8> kManifestMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'R'}, std::byte{'D'}, std::byte{'M'}, std::byte{0}};

// v1 reserves the upper half for incompatible (mandatory-reader) features.
// Unknown lower-half bits are optional and are preserved by decode/encode.
inline constexpr std::uint64_t kManifestMandatoryFeatureMask =
    0xFFFFFFFF00000000ULL;

struct ManifestSlot final {
    std::uint8_t slot_index{0};
    ImageUuid image_uuid{};
    ManifestGenerationDescriptor generation{};
    ImageGeometry geometry{};
    std::string base_relative_name;
    VhdxFileIdentity base_identity{};
    std::uint64_t base_expected_length{0};
    std::string journal_relative_name;
    JournalFileIdentity journal_identity{};
    std::uint64_t feature_flags{0};
    std::uint64_t created_time_100ns{0};
    std::uint64_t committed_time_100ns{0};

    bool operator==(const ManifestSlot& other) const noexcept {
        return slot_index == other.slot_index &&
               image_uuid == other.image_uuid &&
               generation == other.generation &&
               geometry.virtual_bytes == other.geometry.virtual_bytes &&
               geometry.logical_sector_size ==
                   other.geometry.logical_sector_size &&
               geometry.physical_sector_size ==
                   other.geometry.physical_sector_size &&
               base_relative_name == other.base_relative_name &&
               base_identity == other.base_identity &&
               base_expected_length == other.base_expected_length &&
               journal_relative_name == other.journal_relative_name &&
               journal_identity == other.journal_identity &&
               feature_flags == other.feature_flags &&
               created_time_100ns == other.created_time_100ns &&
               committed_time_100ns == other.committed_time_100ns;
    }
};

enum class ManifestSlotInspectionStatus : std::uint8_t {
    DecodedV1,
    Unsupported,
};

// Structural inspection verifies exact size, magic, declared slot length and
// CRC before classifying support. For supported v1 bytes it decodes fields but
// deliberately leaves semantic and path checks to the recovery evidence
// pipeline. The legacy decode_manifest_slot API below remains fully strict.
struct ManifestSlotInspection final {
    ManifestSlotInspectionStatus status{
        ManifestSlotInspectionStatus::DecodedV1};
    // Preserved separately because ManifestSlot freezes the product block
    // size rather than exposing it as mutable state. Recovery classifies a
    // wrong on-disk value at SlotSemantics, after CRC and support.
    std::uint32_t m2_block_size{};
    std::optional<ManifestSlot> slot{};
};

[[nodiscard]] Result<ManifestSlotInspection> inspect_manifest_slot(
    std::span<const std::byte> bytes);

[[nodiscard]] Result<void> validate_manifest_slot_semantics(
    const ManifestSlot& slot);

[[nodiscard]] Result<void> validate_manifest_slot_paths(
    const ManifestSlot& slot);

[[nodiscard]] Result<ManifestSlotBytes> encode_manifest_slot(
    const ManifestSlot& slot);

[[nodiscard]] Result<ManifestSlot> decode_manifest_slot(
    std::span<const std::byte> bytes);

}  // namespace myrd::persistence
