// The neutral view vocabulary (T3-01).  This translation unit exists so the header is
// parsed by the build even before a consumer exists, and so the numbers on the wire are
// asserted here rather than discovered by a client that decoded them wrongly.

#include "src/product/service_view.h"

namespace myrd::product {
namespace {

// The wire values are part of the format.  If one of these changes, a client built against
// the old numbers decodes a different state -- so each is written down.
static_assert(static_cast<std::uint8_t>(ServiceRuntime::running) == 2);
static_assert(static_cast<std::uint8_t>(ServicePersistence::dirty) == 1);
static_assert(static_cast<std::uint8_t>(ServiceHealth::needs_attention) == 2);
static_assert(static_cast<std::uint8_t>(ServiceAggregate::running) == 1);
static_assert(static_cast<std::uint8_t>(ServiceOperationState::succeeded) == 3);
static_assert(static_cast<std::uint8_t>(ServiceDisposition::completed) == 0);

} // namespace
} // namespace myrd::product
