#pragma once

// **The wire shape moved to the client library (T3-01).**
//
// This header used to own the response format, the projection and the codec together, which
// meant the service library was also the only place a client-shaped decoder could live.  T3-01
// needs a client that links neither the service nor the storage stack, so the format and its
// codec moved to `src/client/response_view.h` (`myrd_product_client`), and what remains here is
// the server-side *projection*: durable snapshot in, bounded wire shape out.
//
// It stays in the service because the projection is where the decision about what a client may
// know is made -- it reads bundle paths, ownership and journals in order to drop them -- and
// that decision belongs next to the state it is made from.

#include "src/client/response_view.h"
#include "src/management/service_core.h"

#include <span>
#include <vector>

namespace myrd::management {

// Re-exported so every existing call site keeps its name.
using client::decode_service_response;
using client::encode_service_response_wire;
using client::kMaximumWireCommandBytes;
using client::kMaximumWireErrorBytes;
using client::kMaximumWireFields;
using client::kMaximumWireMountBytes;
using client::kMaximumWirePhaseBytes;
using client::kMaximumWireProfiles;
using client::kMaximumWireWarnings;
using client::kServiceResponseWireMajor;
using client::kServiceResponseWireMinor;
using client::ServiceResponseWire;

// The projection: durable snapshot in, bounded client projection out.
//
// `aggregate` is derived here through the same rule the read path uses
// (`aggregate_status(reported_profile_state(...))`), so a client cannot compute a
// verdict that disagrees with the service's.  A list that reads `runtime` alone
// sees the durable claim, which during a transition is not the whole truth.
product::ServiceDiskView project_profile(const ManagedProfileSnapshot& snapshot);
product::ServiceOperationView project_operation(const OperationRecord& record);

// `ServiceResponse` -> wire.  Cannot fail: the projection drops nothing it is
// required to keep, and `encode_service_response_wire` is where the bounds are enforced.
ServiceResponseWire project_response(const ServiceResponse& response);

// The whole server side in one call: project, then encode.
Result<std::vector<std::byte>> encode_service_response(
    const ServiceResponse& response);

} // namespace myrd::management
