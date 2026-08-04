/// @file persistence_types.cpp
/// @brief M3 persistence public types — anchor translation unit for build target.
///
/// The frozen constants live in the header. This translation unit exists
/// only to produce a linkable myrd_persistence.lib for build-boundary checks.

#include "src/persistence/persistence_types.h"

namespace myrd {
namespace persistence {
// Anchor: format version constants are inline constexpr in the header.
}  // namespace persistence
}  // namespace myrd
