#pragma once

#include "src/persistence/persistence_types.h"

#include <cstdint>

namespace myrd::product {

inline constexpr std::uint32_t kProductVersionMajor = 1;
inline constexpr std::uint32_t kProductVersionMinor = 0;
inline constexpr std::uint32_t kProductVersionPatch = 0;
inline constexpr const char* kProductVersion = "1.0.0";

inline constexpr std::uint32_t kCliSchemaVersion = 1;
inline constexpr std::uint32_t kPublicIpcVersionMajor = 1;
inline constexpr std::uint32_t kPublicIpcVersionMinor = 0;
inline constexpr std::uint32_t kPrivateIpcVersionMajor = 1;
inline constexpr std::uint32_t kPrivateIpcVersionMinor = 0;
inline constexpr std::uint32_t kProfileSchemaVersion = 1;
inline constexpr std::uint32_t kSkillVersion = 1;

inline constexpr std::uint32_t kBundleFormatMajor = myrd::persistence::kM3FormatMajor;
inline constexpr std::uint32_t kBundleFormatMinor = myrd::persistence::kM3FormatMinor;

} // namespace myrd::product
