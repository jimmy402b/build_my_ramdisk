#pragma once

/// @file persistence_types.h
/// @brief M3 persistence engine public constants, types and version contract.
///
/// This header belongs to the myrd_persistence library and must not depend
/// on driver, proxy, M1, M2 or platform lifecycle types.

#include <cstddef>
#include <cstdint>

namespace myrd {
namespace persistence {

// ---- Format version --------------------------------------------------------
inline constexpr std::uint32_t kFormatMajor = 1;
inline constexpr std::uint32_t kFormatMinor = 0;

// ---- Fixed layout constants (frozen by SPEC) -------------------------------
inline constexpr std::size_t kManifestSlotBytes   = 4096;
inline constexpr std::size_t kManifestFileBytes    = 8192;
inline constexpr std::size_t kJournalAlignment     = 4096;
inline constexpr std::size_t kIndexNodeBytes       = 4096;
inline constexpr std::size_t kMaxRelativeNameBytes = 255;
inline constexpr std::uint32_t kVhdxDataBlockBytes = 2U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxIndexDepth      = 8;

// ---- Generation / epoch helpers --------------------------------------------
// generation and epoch never wrap; checked-increment callers (Task 13) return
// a fatal error on overflow.

inline constexpr std::uint64_t kInvalidGeneration = UINT64_MAX;

}  // namespace persistence
}  // namespace myrd
