#include "test_framework.h"

#include "src/control/command.h"
#include "src/control/ipc_frame.h"
#include "src/control/ipc_codec.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    if (!parsed.ok()) {
        throw std::runtime_error("invalid UUID in IPC test");
    }
    return parsed.move_value();
}

template <std::size_t Size>
void require_bytes_equal(const std::vector<std::byte>& actual,
                         const std::array<std::uint8_t, Size>& expected) {
    REQUIRE_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE_EQ(static_cast<std::uint8_t>(actual[index]), expected[index]);
    }
}

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_field(std::vector<std::byte>& bytes,
                  std::uint16_t tag,
                  myrd::ipc::FieldKind kind,
                  myrd::ipc::FieldRequirement requirement,
                  std::initializer_list<std::uint8_t> value) {
    append_u16(bytes, tag);
    bytes.push_back(static_cast<std::byte>(kind));
    bytes.push_back(static_cast<std::byte>(requirement));
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    for (const auto byte : value) {
        bytes.push_back(static_cast<std::byte>(byte));
    }
}

constexpr std::array<myrd::ipc::FieldDescriptor, 2> kTestFields{{
    {1, myrd::ipc::FieldKind::bytes, myrd::ipc::FieldRequirement::required,
     myrd::ipc::FieldDisposition::consume, 0, 4, 0},
    {2, myrd::ipc::FieldKind::bytes, myrd::ipc::FieldRequirement::optional,
     myrd::ipc::FieldDisposition::ignore, 0, 2, 0},
}};

constexpr myrd::ipc::CommandSchema kTestSchema{17, 2, kTestFields};

constexpr std::array<myrd::ipc::FieldDescriptor, 2> kMinorFields{{
    {1, myrd::ipc::FieldKind::bytes, myrd::ipc::FieldRequirement::required,
     myrd::ipc::FieldDisposition::consume, 0, 4, 0},
    {2, myrd::ipc::FieldKind::bytes, myrd::ipc::FieldRequirement::optional,
     myrd::ipc::FieldDisposition::ignore, 1, 2, 0},
}};

constexpr myrd::ipc::CommandSchema kMinorSchema{18, 2, kMinorFields};

constexpr std::array<myrd::ipc::FieldDescriptor, 1> kStringFields{{
    {1, myrd::ipc::FieldKind::string_utf8,
     myrd::ipc::FieldRequirement::required,
     myrd::ipc::FieldDisposition::consume, 0, 16, 0},
}};

constexpr myrd::ipc::CommandSchema kStringSchema{19, 1, kStringFields};

} // namespace

TEST_CASE(ipc_typed_payload_accepts_known_fields_and_schema_declared_ignore) {
    std::vector<std::byte> payload;
    append_field(payload, 1, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::required, {0xaa, 0xbb});
    append_field(payload, 2, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::optional, {0xcc});
    auto decoded = myrd::ipc::decode_typed_payload(payload, kTestSchema, 0);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().fields.size(), 1U);
    REQUIRE_EQ(decoded.value().fields[0].tag, 1U);
    REQUIRE_EQ(decoded.value().fields[0].value.size(), 2U);
    REQUIRE_EQ(decoded.value().allocated_value_bytes, 2U);
}

TEST_CASE(ipc_typed_payload_rejects_unknown_fields_even_if_wire_claims_optional) {
    std::vector<std::byte> optional_unknown;
    append_field(optional_unknown, 3, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::optional, {0xaa});
    REQUIRE(!myrd::ipc::decode_typed_payload(
        optional_unknown, kTestSchema, 0).ok());

    std::vector<std::byte> required_unknown;
    append_field(required_unknown, 3, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::required, {0xaa});
    REQUIRE(!myrd::ipc::decode_typed_payload(
        required_unknown, kTestSchema, 0).ok());
}

TEST_CASE(ipc_typed_payload_accepts_new_optional_field_only_after_minor_negotiation) {
    std::vector<std::byte> payload;
    append_field(payload, 1, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::required, {0xaa});
    append_field(payload, 2, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::optional, {0xbb});
    REQUIRE(!myrd::ipc::decode_typed_payload(payload, kMinorSchema, 0).ok());
    auto decoded = myrd::ipc::decode_typed_payload(payload, kMinorSchema, 1);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().fields.size(), 1U);
    REQUIRE_EQ(decoded.value().allocated_value_bytes, 1U);
}

TEST_CASE(ipc_typed_payload_rejects_overlong_surrogate_and_out_of_range_utf8) {
    const std::array malformed{
        std::initializer_list<std::uint8_t>{0xc0, 0xaf},
        std::initializer_list<std::uint8_t>{0xed, 0xa0, 0x80},
        std::initializer_list<std::uint8_t>{0xf4, 0x90, 0x80, 0x80},
    };
    for (const auto bytes : malformed) {
        std::vector<std::byte> payload;
        append_field(payload, 1, myrd::ipc::FieldKind::string_utf8,
                     myrd::ipc::FieldRequirement::required, bytes);
        REQUIRE(!myrd::ipc::decode_typed_payload(
            payload, kStringSchema, 0).ok());
    }
}

TEST_CASE(ipc_typed_payload_rejects_duplicate_missing_and_mismatched_fields) {
    std::vector<std::byte> duplicate;
    append_field(duplicate, 1, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::required, {0xaa});
    append_field(duplicate, 1, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::required, {0xbb});
    REQUIRE(!myrd::ipc::decode_typed_payload(duplicate, kTestSchema, 0).ok());

    std::vector<std::byte> missing_required;
    append_field(missing_required, 2, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::optional, {0xaa});
    REQUIRE(!myrd::ipc::decode_typed_payload(
        missing_required, kTestSchema, 0).ok());

    std::vector<std::byte> wrong_kind;
    append_field(wrong_kind, 1, myrd::ipc::FieldKind::uint32_le,
                 myrd::ipc::FieldRequirement::required, {0, 0, 0, 0});
    REQUIRE(!myrd::ipc::decode_typed_payload(wrong_kind, kTestSchema, 0).ok());

    std::vector<std::byte> wrong_requirement;
    append_field(wrong_requirement, 1, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::optional, {0xaa});
    REQUIRE(!myrd::ipc::decode_typed_payload(
        wrong_requirement, kTestSchema, 0).ok());
}

TEST_CASE(ipc_typed_payload_rejects_limits_truncation_and_trailing_garbage) {
    std::vector<std::byte> too_large;
    append_field(too_large, 1, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::required,
                 {0, 1, 2, 3, 4});
    REQUIRE(!myrd::ipc::decode_typed_payload(too_large, kTestSchema, 0).ok());

    std::vector<std::byte> short_header{std::byte{1}, std::byte{0}};
    REQUIRE(!myrd::ipc::decode_typed_payload(
        short_header, kTestSchema, 0).ok());

    std::vector<std::byte> short_value;
    append_u16(short_value, 1);
    short_value.push_back(static_cast<std::byte>(myrd::ipc::FieldKind::bytes));
    short_value.push_back(static_cast<std::byte>(
        myrd::ipc::FieldRequirement::required));
    append_u32(short_value, 4);
    short_value.push_back(std::byte{0});
    REQUIRE(!myrd::ipc::decode_typed_payload(
        short_value, kTestSchema, 0).ok());

    std::vector<std::byte> too_many;
    append_field(too_many, 1, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::required, {0});
    append_field(too_many, 2, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::optional, {0});
    append_field(too_many, 2, myrd::ipc::FieldKind::bytes,
                 myrd::ipc::FieldRequirement::optional, {0});
    REQUIRE(!myrd::ipc::decode_typed_payload(too_many, kTestSchema, 0).ok());
}

TEST_CASE(ipc_command_codec_group_one_round_trips_typed_variants) {
    const std::array<myrd::control::Command, 5> commands{{
        myrd::control::StatusCommand{},
        myrd::control::DiskListCommand{},
        myrd::control::DiskShowCommand{L"WorkDisk"},
        myrd::control::DiskStartCommand{L"FastDisk"},
        myrd::control::DiskCheckpointCommand{L"CacheDisk"},
    }};
    constexpr std::array expected_ids{
        myrd::ipc::CommandId::status,
        myrd::ipc::CommandId::disk_list,
        myrd::ipc::CommandId::disk_show,
        myrd::ipc::CommandId::disk_start,
        myrd::ipc::CommandId::disk_checkpoint,
    };
    for (std::size_t index = 0; index < commands.size(); ++index) {
        auto encoded = myrd::ipc::encode_command_payload(commands[index]);
        REQUIRE(encoded.ok());
        REQUIRE_EQ(encoded.value().command_id, expected_ids[index]);
        auto decoded = myrd::ipc::decode_command_payload(
            encoded.value().command_id, encoded.value().payload, 0);
        REQUIRE(decoded.ok());
        REQUIRE_EQ(myrd::control::command_name(decoded.value()),
                   myrd::control::command_name(commands[index]));
    }
}

TEST_CASE(ipc_command_codec_group_one_preserves_unicode_targets) {
    const std::wstring target{static_cast<wchar_t>(0x4E2D),
                              static_cast<wchar_t>(0x6587)};
    myrd::control::Command command = myrd::control::DiskShowCommand{target};
    auto encoded = myrd::ipc::encode_command_payload(command);
    REQUIRE(encoded.ok());
    auto decoded = myrd::ipc::decode_command_payload(
        encoded.value().command_id, encoded.value().payload, 0);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(std::get<myrd::control::DiskShowCommand>(decoded.value()).target,
               target);
}

TEST_CASE(ipc_command_codec_rejects_unknown_command_and_malformed_payload) {
    const std::array<std::byte, 1> garbage{{std::byte{0}}};
    REQUIRE(!myrd::ipc::decode_command_payload(
        static_cast<myrd::ipc::CommandId>(0xffffffffU), {}, 0).ok());
    REQUIRE(!myrd::ipc::decode_command_payload(
        myrd::ipc::CommandId::status, garbage, 0).ok());
    REQUIRE(!myrd::ipc::decode_command_payload(
        myrd::ipc::CommandId::disk_show, {}, 0).ok());
}

TEST_CASE(ipc_command_codec_group_two_round_trips_targets_operations_and_driver) {
    const auto operation_id = uuid("01234567-89ab-cdef-0123-456789abcdef");
    const std::array<myrd::control::Command, 7> commands{{
        myrd::control::DiskCompactCommand{L"WorkDisk"},
        myrd::control::DiskConfigGetCommand{L"WorkDisk"},
        myrd::control::OperationListCommand{},
        myrd::control::OperationShowCommand{operation_id},
        myrd::control::OperationWaitCommand{operation_id},
        myrd::control::OperationCancelCommand{operation_id},
        myrd::control::DriverStatusCommand{},
    }};
    for (const auto& command : commands) {
        auto encoded = myrd::ipc::encode_command_payload(command);
        REQUIRE(encoded.ok());
        auto decoded = myrd::ipc::decode_command_payload(
            encoded.value().command_id, encoded.value().payload, 0);
        REQUIRE(decoded.ok());
        REQUIRE_EQ(myrd::control::command_name(decoded.value()),
                   myrd::control::command_name(command));
    }
    auto encoded = myrd::ipc::encode_command_payload(
        myrd::control::OperationShowCommand{operation_id});
    auto decoded = myrd::ipc::decode_command_payload(
        encoded.value().command_id, encoded.value().payload, 0);
    REQUIRE_EQ(std::get<myrd::control::OperationShowCommand>(decoded.value())
                   .operation_id.to_string(),
               operation_id.to_string());
}

TEST_CASE(ipc_connection_policy_allows_only_one_in_flight_request) {
    myrd::ipc::InFlightGate gate;
    REQUIRE(gate.begin().ok());
    REQUIRE(!gate.begin().ok());
    REQUIRE(gate.complete().ok());
    REQUIRE(gate.begin().ok());
    REQUIRE(gate.complete().ok());
    REQUIRE(!gate.complete().ok());
}

TEST_CASE(ipc_private_channel_requires_exact_product_version) {
    constexpr myrd::ipc::ProductVersion product{1, 0, 0};
    REQUIRE(myrd::ipc::private_product_version_matches(product, product));
    REQUIRE(!myrd::ipc::private_product_version_matches(
        product, myrd::ipc::ProductVersion{1, 0, 1}));
    REQUIRE(!myrd::ipc::private_product_version_matches(
        product, myrd::ipc::ProductVersion{1, 1, 0}));
    REQUIRE(!myrd::ipc::private_product_version_matches(
        product, myrd::ipc::ProductVersion{2, 0, 0}));
}

TEST_CASE(ipc_command_codec_group_three_round_trips_lifecycle_identity_commands) {
    const auto profile_id = uuid("01234567-89ab-cdef-0123-456789abcdef");
    const auto image_id = uuid("11111111-2222-4333-8444-555555555555");
    const std::array<myrd::control::Command, 4> commands{{
        myrd::control::DiskStopCommand{L"WorkDisk", true, true},
        myrd::control::DiskRepairProfileCommand{profile_id, true},
        myrd::control::DiskRebindCommand{
            profile_id, L"D:\\Images\\WorkDisk", image_id},
        myrd::control::DiskForgetCommand{profile_id, profile_id},
    }};
    for (const auto& command : commands) {
        auto encoded = myrd::ipc::encode_command_payload(command);
        REQUIRE(encoded.ok());
        auto decoded = myrd::ipc::decode_command_payload(
            encoded.value().command_id, encoded.value().payload, 0);
        REQUIRE(decoded.ok());
        REQUIRE_EQ(myrd::control::command_name(decoded.value()),
                   myrd::control::command_name(command));
    }
    auto encoded_stop = myrd::ipc::encode_command_payload(commands[0]);
    REQUIRE(encoded_stop.ok());
    auto decoded_stop = myrd::ipc::decode_command_payload(
        encoded_stop.value().command_id, encoded_stop.value().payload, 0);
    REQUIRE(decoded_stop.ok());
    const auto& stop_command =
        std::get<myrd::control::DiskStopCommand>(decoded_stop.value());
    REQUIRE(stop_command.force);
    REQUIRE(stop_command.accept_volatile_data_loss);
    auto encoded = myrd::ipc::encode_command_payload(commands[3]);
    auto decoded = myrd::ipc::decode_command_payload(
        encoded.value().command_id, encoded.value().payload, 0);
    REQUIRE_EQ(std::get<myrd::control::DiskForgetCommand>(decoded.value())
                   .confirmation_id.to_string(),
               profile_id.to_string());
}

TEST_CASE(ipc_command_codec_rejects_invalid_lifecycle_flags_and_identity) {
    REQUIRE(!myrd::ipc::encode_command_payload(
        myrd::control::DiskRepairProfileCommand{
            uuid("01234567-89ab-cdef-0123-456789abcdef"), false}).ok());

    auto stop = myrd::ipc::encode_command_payload(
        myrd::control::DiskStopCommand{L"WorkDisk", true, true});
    REQUIRE(stop.ok());
    auto malformed = stop.value().payload;
    malformed.back() = std::byte{2};
    REQUIRE(!myrd::ipc::decode_command_payload(
        stop.value().command_id, malformed, 0).ok());

    auto rebind = myrd::ipc::encode_command_payload(
        myrd::control::DiskRebindCommand{
            uuid("01234567-89ab-cdef-0123-456789abcdef"),
            L"D:\\Images\\WorkDisk",
            uuid("11111111-2222-4333-8444-555555555555")});
    REQUIRE(rebind.ok());
    rebind.value().payload.pop_back();
    REQUIRE(!myrd::ipc::decode_command_payload(
        rebind.value().command_id, rebind.value().payload, 0).ok());
}

TEST_CASE(ipc_command_codec_accepts_legacy_normal_stop_without_loss_acknowledgement) {
    auto encoded = myrd::ipc::encode_command_payload(
        myrd::control::DiskStopCommand{L"WorkDisk", false, false});
    REQUIRE(encoded.ok());
    REQUIRE(encoded.value().payload.size() >= 9U);

    auto legacy = encoded.value().payload;
    legacy.resize(legacy.size() - 9U);
    auto decoded = myrd::ipc::decode_command_payload(
        encoded.value().command_id, legacy, 0);

    REQUIRE(decoded.ok());
    const auto& stop =
        std::get<myrd::control::DiskStopCommand>(decoded.value());
    REQUIRE_EQ(stop.target, std::wstring(L"WorkDisk"));
    REQUIRE(!stop.force);
    REQUIRE(!stop.accept_volatile_data_loss);

    auto force = myrd::ipc::encode_command_payload(
        myrd::control::DiskStopCommand{L"WorkDisk", true, true});
    REQUIRE(force.ok());
    force.value().payload.resize(force.value().payload.size() - 9U);
    REQUIRE(!myrd::ipc::decode_command_payload(
        force.value().command_id, force.value().payload, 0).ok());
}

TEST_CASE(ipc_command_codec_group_four_round_trips_configuration_and_creation) {
    myrd::control::Command config = myrd::control::DiskConfigSetCommand{
        L"WorkDisk", {{L"checkpoint.interval", L"5m"},
                       {L"checkpoint.enabled", L"true"}}};
    myrd::control::Command diagnostics = myrd::control::DiagnosticsCollectCommand{
        std::wstring(L"WorkDisk"), std::wstring(L"C:\\Temp\\diag.zip"), true};
    myrd::control::Command import = myrd::control::DiskImportCommand{
        L"Imported", L"D:\\Images\\Imported", L"R:",
        std::optional<std::wstring>(L"S-1-5-21-1000")};
    myrd::control::Command create = myrd::control::DiskCreateCommand{
        L"FastDisk", L"D:\\Images\\FastDisk", 8ULL << 30, L"R:",
        std::nullopt, false, myrd::control::MemoryMode::fixed,
        2ULL << 30,
        myrd::product::CheckpointInterval{true, std::chrono::minutes(5)},
        myrd::control::OptionalSize{false, 0}};

    const std::array<myrd::control::Command, 4> commands{{
        config, diagnostics, import, create}};
    for (const auto& command : commands) {
        auto encoded = myrd::ipc::encode_command_payload(command);
        REQUIRE(encoded.ok());
        auto decoded = myrd::ipc::decode_command_payload(
            encoded.value().command_id, encoded.value().payload, 0);
        REQUIRE(decoded.ok());
        REQUIRE_EQ(myrd::control::command_name(decoded.value()),
                   myrd::control::command_name(command));
    }

    auto config_wire = myrd::ipc::encode_command_payload(config);
    auto config_back = myrd::ipc::decode_command_payload(
        config_wire.value().command_id, config_wire.value().payload, 0);
    const auto& assignments = std::get<myrd::control::DiskConfigSetCommand>(
        config_back.value()).assignments;
    REQUIRE_EQ(assignments.size(), 2U);
    REQUIRE_EQ(assignments[1].key, std::wstring(L"checkpoint.enabled"));

    auto create_wire = myrd::ipc::encode_command_payload(create);
    auto create_back = myrd::ipc::decode_command_payload(
        create_wire.value().command_id, create_wire.value().payload, 0);
    const auto& created = std::get<myrd::control::DiskCreateCommand>(
        create_back.value());
    REQUIRE_EQ(created.size_bytes, 8ULL << 30);
    REQUIRE_EQ(*created.memory_hard_limit_bytes, 2ULL << 30);
    REQUIRE(!created.dirty_threshold->enabled);
}

TEST_CASE(ipc_command_codec_rejects_truncated_configuration_array) {
    myrd::control::Command config = myrd::control::DiskConfigSetCommand{
        L"WorkDisk", {{L"key", L"value"}}};
    auto encoded = myrd::ipc::encode_command_payload(config);
    REQUIRE(encoded.ok());
    encoded.value().payload.pop_back();
    REQUIRE(!myrd::ipc::decode_command_payload(
        encoded.value().command_id, encoded.value().payload, 0).ok());
}

TEST_CASE(ipc_frame_rejects_every_truncated_prefix_and_trailing_bytes) {
    myrd::ipc::Frame frame{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"),
        1, {std::byte{0xaa}, std::byte{0xbb}}};
    auto encoded = myrd::ipc::encode_frame(frame);
    REQUIRE(encoded.ok());
    for (std::size_t length = 0; length < encoded.value().size(); ++length) {
        REQUIRE(!myrd::ipc::decode_frame(
            std::span(encoded.value()).first(length)).ok());
    }
    auto trailing = encoded.value();
    trailing.push_back(std::byte{0});
    REQUIRE(!myrd::ipc::decode_frame(trailing).ok());
}

TEST_CASE(ipc_frame_rejects_bad_magic_type_major_and_payload_lengths) {
    myrd::ipc::Frame frame{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"),
        1, {}};
    auto encoded = myrd::ipc::encode_frame(frame);
    REQUIRE(encoded.ok());

    auto bad_magic = encoded.value();
    bad_magic[0] = std::byte{0};
    REQUIRE(!myrd::ipc::decode_frame(bad_magic).ok());

    auto bad_type = encoded.value();
    bad_type[8] = std::byte{3};
    REQUIRE(!myrd::ipc::decode_frame(bad_type).ok());

    auto bad_major = encoded.value();
    bad_major[4] = std::byte{2};
    REQUIRE(!myrd::ipc::decode_frame(bad_major).ok());

    auto too_large = encoded.value();
    too_large[14] = std::byte{0x01};
    too_large[15] = std::byte{0x00};
    too_large[16] = std::byte{0x10};
    too_large[17] = std::byte{0x00};
    REQUIRE(!myrd::ipc::decode_frame(too_large).ok());

    auto inconsistent = encoded.value();
    inconsistent[14] = std::byte{0xff};
    inconsistent[15] = std::byte{0xff};
    inconsistent[16] = std::byte{0xff};
    inconsistent[17] = std::byte{0xff};
    REQUIRE(!myrd::ipc::decode_frame(inconsistent).ok());
}

TEST_CASE(ipc_frame_enforces_exact_one_mib_payload_limit_on_encode) {
    myrd::ipc::Frame frame{
        1, 0, myrd::ipc::MessageType::request, 0,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("00000000-0000-0000-0000-000000000000"),
        1, std::vector<std::byte>(myrd::ipc::kMaxPayloadBytes)};
    REQUIRE(myrd::ipc::encode_frame(frame).ok());
    frame.payload.push_back(std::byte{0});
    REQUIRE(!myrd::ipc::encode_frame(frame).ok());
}

TEST_CASE(ipc_protocol_negotiation_requires_major_and_uses_lower_minor) {
    auto same = myrd::ipc::negotiate_protocol({1, 5}, {1, 3});
    REQUIRE(same.ok());
    REQUIRE_EQ(same.value().major, 1U);
    REQUIRE_EQ(same.value().minor, 3U);
    auto older = myrd::ipc::negotiate_protocol({1, 0}, {1, 10});
    REQUIRE(older.ok());
    REQUIRE_EQ(older.value().minor, 0U);
    REQUIRE(!myrd::ipc::negotiate_protocol({1, 0}, {2, 0}).ok());
}

TEST_CASE(ipc_frame_empty_request_has_exact_little_endian_bytes) {
    myrd::ipc::Frame frame{
        myrd::ipc::kProtocolMajor,
        myrd::ipc::kProtocolMinor,
        myrd::ipc::MessageType::request,
        0,
        uuid("00000000-0000-0000-0000-000000000000"),
        uuid("00000000-0000-0000-0000-000000000000"),
        0,
        {}};

    auto encoded = myrd::ipc::encode_frame(frame);
    REQUIRE(encoded.ok());
    constexpr std::array<std::uint8_t, 54> expected{{
        0x4d, 0x59, 0x52, 0x44,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    }};
    require_bytes_equal(encoded.value(), expected);

    auto decoded = myrd::ipc::decode_frame(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().protocol_major, 1U);
    REQUIRE_EQ(decoded.value().protocol_minor, 0U);
    REQUIRE_EQ(decoded.value().message_type, myrd::ipc::MessageType::request);
    REQUIRE_EQ(decoded.value().flags, 0U);
    REQUIRE_EQ(decoded.value().request_id.to_string(),
               std::string("00000000-0000-0000-0000-000000000000"));
    REQUIRE_EQ(decoded.value().operation_id.to_string(),
               std::string("00000000-0000-0000-0000-000000000000"));
    REQUIRE_EQ(decoded.value().command_id, 0U);
    REQUIRE(decoded.value().payload.empty());
}

TEST_CASE(ipc_frame_multibyte_response_has_exact_little_endian_bytes) {
    myrd::ipc::Frame frame{
        1,
        0,
        myrd::ipc::MessageType::response,
        0x11223344U,
        uuid("00112233-4455-6677-8899-aabbccddeeff"),
        uuid("10213243-5465-7687-98a9-bacbdcedfe0f"),
        0xa1b2c3d4U,
        {std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}}};

    auto encoded = myrd::ipc::encode_frame(frame);
    REQUIRE(encoded.ok());
    constexpr std::array<std::uint8_t, 57> expected{{
        0x4d, 0x59, 0x52, 0x44,
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00,
        0x44, 0x33, 0x22, 0x11,
        0x03, 0x00, 0x00, 0x00,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
        0xd4, 0xc3, 0xb2, 0xa1,
        0xaa, 0xbb, 0xcc,
    }};
    require_bytes_equal(encoded.value(), expected);

    auto decoded = myrd::ipc::decode_frame(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().message_type, myrd::ipc::MessageType::response);
    REQUIRE_EQ(decoded.value().flags, 0x11223344U);
    REQUIRE_EQ(decoded.value().request_id.to_string(),
               std::string("00112233-4455-6677-8899-aabbccddeeff"));
    REQUIRE_EQ(decoded.value().operation_id.to_string(),
               std::string("10213243-5465-7687-98a9-bacbdcedfe0f"));
    REQUIRE_EQ(decoded.value().command_id, 0xa1b2c3d4U);
    REQUIRE_EQ(decoded.value().payload.size(), 3U);
    REQUIRE_EQ(static_cast<std::uint8_t>(decoded.value().payload[0]), 0xaaU);
}
