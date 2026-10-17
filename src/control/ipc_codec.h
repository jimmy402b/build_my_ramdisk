#pragma once

#include "src/common/result.h"
#include "src/control/command.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace myrd::ipc {

struct ProtocolVersion {
    std::uint16_t major{};
    std::uint16_t minor{};
};

Result<ProtocolVersion> negotiate_protocol(ProtocolVersion local,
                                           ProtocolVersion peer);

enum class FieldKind : std::uint8_t {
    bytes = 0,
    string_utf8 = 1,
    path_local = 2,
    uint32_le = 3,
    uint64_le = 4,
    uuid_raw = 5,
    boolean = 6,
    string_pairs = 7,
};

enum class FieldRequirement : std::uint8_t {
    optional = 0,
    required = 1,
};

enum class FieldDisposition : std::uint8_t {
    consume = 0,
    ignore = 1,
};

struct FieldDescriptor {
    std::uint16_t tag{};
    FieldKind kind{};
    FieldRequirement requirement{};
    FieldDisposition disposition{};
    std::uint16_t introduced_minor{};
    std::uint32_t max_value_bytes{};
    std::uint16_t max_array_items{};
};

struct CommandSchema {
    std::uint32_t command_id{};
    std::uint16_t max_fields{};
    std::span<const FieldDescriptor> fields;
};

struct DecodedField {
    std::uint16_t tag{};
    FieldKind kind{};
    std::vector<std::byte> value;
};

struct DecodedFieldSet {
    std::vector<DecodedField> fields;
    std::size_t allocated_value_bytes{};
};

Result<DecodedFieldSet> decode_typed_payload(
    std::span<const std::byte> payload,
    const CommandSchema& schema,
    std::uint16_t negotiated_minor);

enum class CommandId : std::uint32_t {
    status = 1,
    disk_list = 2,
    disk_show = 3,
    disk_create = 4,
    disk_import = 5,
    disk_start = 6,
    disk_stop = 7,
    disk_checkpoint = 8,
    disk_compact = 9,
    disk_config_get = 10,
    disk_config_set = 11,
    disk_repair_profile = 12,
    disk_rebind = 13,
    disk_forget = 14,
    operation_list = 15,
    operation_show = 16,
    operation_wait = 17,
    operation_cancel = 18,
    diagnostics_collect = 19,
    driver_status = 20,
};

struct EncodedCommand {
    CommandId command_id{};
    std::vector<std::byte> payload;
};

Result<EncodedCommand> encode_command_payload(const control::Command& command);
Result<control::Command> decode_command_payload(
    CommandId command_id,
    std::span<const std::byte> payload,
    std::uint16_t negotiated_minor);

// The protocol's own UTF conversions, shared rather than reimplemented.
//
// The response codec (`src/management/service_response_wire.h`) carries wide
// strings -- an `Error`'s operation and message -- and needs the same strict,
// locale-independent conversion the command codec uses.  A second implementation
// would be a second place for a lone surrogate to be accepted, so this is the one
// that is exported: strict on invalid input, and never a lossy replace-and-continue.
Result<std::string> utf16_to_utf8(std::wstring_view value);
Result<std::wstring> utf8_to_utf16(std::span<const std::byte> value);

class InFlightGate final {
public:
    Result<void> begin() noexcept;
    Result<void> complete() noexcept;

private:
    std::atomic_bool active_{false};
};

struct ProductVersion {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};
    bool operator==(const ProductVersion&) const noexcept = default;
};

bool private_product_version_matches(ProductVersion local,
                                     ProductVersion peer) noexcept;

} // namespace myrd::ipc
