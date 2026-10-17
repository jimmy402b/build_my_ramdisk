#pragma once

// **The response half of the public protocol.**
//
// The request half already existed: a typed `control::Command` becomes a frame and
// the service decodes it (`ipc::encode_command_payload` / `decode_command_payload`).
// What did not exist was anything that carried a *result* back.  `ServiceResponse`
// is the service's own answer, and it holds `ManagedProfileSnapshot` values --
// records containing `bundle_path`, mount targets, ownership and durable journals.
// Putting that on a wire read by an unprivileged client would be the path leak
// design spec 搂10.3 forbids in as many words.
//
// So this file owns three things, and only the first is mechanical:
//
//   1. `ServiceResponseWire`, the one bounded, path-free shape a client ever sees;
//   2. the projection from the service's answer onto it, which is where the
//      decision about *what a client may know* is made and can be read;
//   3. a strict codec, in both directions, so the service and the client cannot
//      disagree about the format without a test failing.
//
// The projection is deliberately lossy and says so.  A list of disks carries no
// bundle path, no journal, no operation gate, no checkpoint correlation; it carries
// what the design's 搂10.3 data model names, plus the mount target, which is the
// thing a user recognises a disk by.  Everything else is derived from the durable
// state, which stays the authority, and is not copied to the client.
//
// The codec is strict the way the command codec is strict: unknown fields, a
// duplicate field, a truncated field, a count past its bound, an enum value out of
// range and trailing bytes are all *errors* rather than ignored.  A lenient reader
// of a response is how a client ends up acting on a field it did not understand.

#include "src/common/result.h"
#include "src/management/service_core.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd::management {

// Version of the response document, independent of the frame's protocol version so
// the two can move separately.  A reader accepts its own major and any minor up to
// its own; a different major is a different format, not a newer one.
inline constexpr std::uint16_t kServiceResponseWireMajor = 1;
inline constexpr std::uint16_t kServiceResponseWireMinor = 0;

// Bounds.  The frame layer caps a payload at 1 MiB; these are what the decoder
// enforces *before* allocating, so a hostile payload cannot turn a length field
// into an allocation the process cannot refuse.
inline constexpr std::size_t kMaximumWireProfiles = 1024;
inline constexpr std::size_t kMaximumWireWarnings = 32;
inline constexpr std::size_t kMaximumWireCommandBytes = 128;
inline constexpr std::size_t kMaximumWirePhaseBytes = 128;
inline constexpr std::size_t kMaximumWireErrorBytes = 4096;
inline constexpr std::size_t kMaximumWireMountBytes = 1024;
inline constexpr std::size_t kMaximumWireFields = 16;

// The client-visible result of one command.
//
// The fields are the design's 搂10.3 data model and nothing else.  `error` is present
// exactly when the command failed, and a client that renders `completed` over a
// populated `error` would be rendering a success that did not happen -- which is why
// `disposition` and `error` are both carried rather than one being inferred.
struct ServiceResponseWire final {
    product::Uuid request_id;
    std::string command;
    ServiceResponseDisposition disposition{ServiceResponseDisposition::completed};
    std::optional<product::Uuid> operation_id;
    std::optional<ServiceOperationProjection> operation;
    std::vector<ServiceProfileProjection> profiles;
    std::vector<control::IdempotentWarning> warnings;
    std::optional<Error> error;
    // The mount target the command concerns, when there is exactly one.  For a list
    // the per-profile target travels in the projection instead.
    std::optional<std::string> mount_target;
};

// The projection: durable snapshot in, bounded client projection out.
//
// `aggregate` is derived here through the same rule the read path uses
// (`aggregate_status(reported_profile_state(...))`), so a client cannot compute a
// verdict that disagrees with the service's.  A list that reads `runtime` alone
// sees the durable claim, which during a transition is not the whole truth.
ServiceProfileProjection project_profile(const ManagedProfileSnapshot& snapshot);
ServiceOperationProjection project_operation(const OperationRecord& record);

// `ServiceResponse` -> wire.  Cannot fail: the projection drops nothing it is
// required to keep, and `encode_service_response` is where the bounds are enforced.
ServiceResponseWire project_response(const ServiceResponse& response);

Result<std::vector<std::byte>> encode_service_response_wire(
    const ServiceResponseWire& response);

// The whole server side in one call: project, then encode.
Result<std::vector<std::byte>> encode_service_response(
    const ServiceResponse& response);

// The client side.  `negotiated_minor` is the frame's protocol minor, so a response
// written against a newer document is refused rather than half-read.
Result<ServiceResponseWire> decode_service_response(
    std::span<const std::byte> payload,
    std::uint16_t negotiated_minor);

} // namespace myrd::management
