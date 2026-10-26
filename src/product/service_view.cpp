// The neutral view vocabulary (T3-01).  This translation unit exists so the header is
// parsed by the build even before a consumer exists, and so the numbers on the wire are
// asserted here rather than discovered by a client that decoded them wrongly.

#include "src/product/service_view.h"

namespace myrd::product {
namespace {

// The wire values are part of the format.  If one of these changes, a client built against
// the old numbers decodes a different state -- so each is written down.
static_assert(static_cast<std::uint8_t>(ServiceRuntime::running) == 3);
static_assert(static_cast<std::uint8_t>(ServicePersistence::dirty) == 2);
static_assert(static_cast<std::uint8_t>(ServiceHealth::needs_attention) == 3);
static_assert(static_cast<std::uint8_t>(ServiceAggregate::running) == 2);
static_assert(static_cast<std::uint8_t>(ServiceOperationState::succeeded) == 4);
static_assert(static_cast<std::uint8_t>(ServiceDisposition::completed) == 1);

// `unknown` is 0 and every known enumerator is above it, which is what makes
// `known_*()` a range test rather than a table.
static_assert(static_cast<std::uint8_t>(ServiceRuntime::unknown) == 0);
static_assert(static_cast<std::uint8_t>(ServiceAggregate::unknown) == 0);

} // namespace
} // namespace myrd::product
