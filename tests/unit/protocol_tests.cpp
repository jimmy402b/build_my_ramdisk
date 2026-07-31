#include "test_framework.h"

#include "src/driver/aim_protocol.h"
#include "src/proxy/imdproxy_protocol.h"

#include <cstring>
#include <vector>

// ── AIM control codes ──────────────────────────────────────────

TEST_CASE(aim_control_codes_match_audited_contract) {
    REQUIRE_EQ(myrd::aim::kSmpImScsi,     0x83730000u);
    REQUIRE_EQ(myrd::aim::kQueryVersion,   0x83730800u);
    REQUIRE_EQ(myrd::aim::kCreateDevice,   0x83730801u);
    REQUIRE_EQ(myrd::aim::kQueryDevice,    0x83730802u);
    REQUIRE_EQ(myrd::aim::kQueryAdapter,   0x83730803u);
    REQUIRE_EQ(myrd::aim::kRemoveDevice,   0x83730806u);
    REQUIRE_EQ(myrd::aim::kDeviceTypeHd,   0x00000010u);
    REQUIRE_EQ(myrd::aim::kTypeProxy,       0x00000300u);
    REQUIRE_EQ(myrd::aim::kProxyTypeShm,    0x00003000u);
    REQUIRE_EQ(myrd::aim::kAllDevices,      0x00FFFFFFu);
    REQUIRE_EQ(myrd::aim::kAutoDeviceNumber, 0x00FFFFFFu);
    REQUIRE_EQ(myrd::aim::kM0CreateFlags,
        myrd::aim::kDeviceTypeHd | myrd::aim::kTypeProxy | myrd::aim::kProxyTypeShm);
}

// ── AIM structure layouts ──────────────────────────────────────

TEST_CASE(aim_create_layout_matches_audited_contract) {
    // SRB_IO_CONTROL
    REQUIRE_EQ(sizeof(myrd::aim::SrbIoControl), 28u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, Signature), 4u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, ControlCode), 16u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, ReturnCode), 24u);
    // DeviceConfiguration — key field offsets verified
    REQUIRE_EQ(offsetof(myrd::aim::DeviceConfiguration, DiskSize), 4u);
    REQUIRE_EQ(offsetof(myrd::aim::DeviceConfiguration, BytesPerSector), 12u);
    REQUIRE_EQ(offsetof(myrd::aim::DeviceConfiguration, Flags), 28u);
    REQUIRE_EQ(offsetof(myrd::aim::DeviceConfiguration, FileNameLength), 32u);
    // SrbCreateData — key offsets
    REQUIRE_EQ(offsetof(myrd::aim::SrbCreateData, Fields), 28u);
}

TEST_CASE(aim_query_layouts_match_audited_contract) {
    REQUIRE_EQ(sizeof(myrd::aim::SrbQueryAdapter), 32u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbQueryAdapter, NumberOfDevices), 28u);
    REQUIRE_EQ(sizeof(myrd::aim::SrbQueryVersion), 32u);
}

TEST_CASE(aim_remove_layout_matches_audited_contract) {
    REQUIRE_EQ(sizeof(myrd::aim::SrbRemoveDevice), 32u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbRemoveDevice, DeviceNumber), 28u);
}

// ── IMDPROXY request codes ─────────────────────────────────────

TEST_CASE(imdproxy_request_codes_match_audited_contract) {
    REQUIRE_EQ(myrd::proxy::kReqInfo,  1u);
    REQUIRE_EQ(myrd::proxy::kReqRead,  2u);
    REQUIRE_EQ(myrd::proxy::kReqWrite, 3u);
    REQUIRE_EQ(myrd::proxy::kReqClose, 5u);
}

// ── IMDPROXY structure layouts ─────────────────────────────────

TEST_CASE(imdproxy_layouts_match_audited_contract) {
    REQUIRE_EQ(sizeof(myrd::proxy::InfoResponse), 24u);
    REQUIRE_EQ(offsetof(myrd::proxy::InfoResponse, flags), 16u);
    REQUIRE_EQ(sizeof(myrd::proxy::ReadRequest), 24u);
    REQUIRE_EQ(sizeof(myrd::proxy::ReadResponse), 16u);
    REQUIRE_EQ(sizeof(myrd::proxy::WriteRequest), 24u);
    REQUIRE_EQ(sizeof(myrd::proxy::WriteResponse), 16u);
    REQUIRE_EQ(sizeof(myrd::proxy::CloseRequest), 8u);
}

// ── DeviceId decode ────────────────────────────────────────────

TEST_CASE(decode_device_address_non_symmetric) {
    // Create a device id where bytes are distinct to catch swapping
    myrd::aim::DeviceId id{0x00140308u};  // PathId=8, TargetId=3, Lun=20
    auto addr = myrd::aim::decode_device_address(id);
    REQUIRE_EQ(static_cast<int>(addr.path_id), 0x08);
    REQUIRE_EQ(static_cast<int>(addr.target_id), 0x03);
    REQUIRE_EQ(static_cast<int>(addr.lun), 0x14);
}

// ── Wire helpers ───────────────────────────────────────────────

TEST_CASE(read_wire_rejects_truncated_buffer) {
    std::byte buf[3]{};
    auto r = myrd::proxy::read_wire<std::uint32_t>(
        std::span<const std::byte>(buf, 3), 0);
    REQUIRE(r.has_error());
    REQUIRE_EQ(static_cast<int>(r.error().domain),
               static_cast<int>(myrd::ErrorDomain::protocol));
}

TEST_CASE(read_wire_reads_at_offset) {
    std::byte buf[8]{};
    buf[4] = std::byte{0x42};
    auto r = myrd::proxy::read_wire<std::uint32_t>(
        std::span<const std::byte>(buf, sizeof(buf)), 4);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(), 0x42u);
}

TEST_CASE(write_wire_rejects_buffer_too_small) {
    std::byte buf[3]{};
    auto r = myrd::proxy::write_wire<std::uint32_t>(
        std::span<std::byte>(buf, 3), 0, 0xDEADu);
    REQUIRE(r.has_error());
}

TEST_CASE(write_wire_writes_and_read_wire_roundtrips) {
    std::byte buf[8]{};
    std::uint64_t val = 0x1122334455667788ull;
    auto w = myrd::proxy::write_wire(
        std::span<std::byte>(buf, sizeof(buf)), 0, val);
    REQUIRE(w.ok());
    auto r = myrd::proxy::read_wire<std::uint64_t>(
        std::span<const std::byte>(buf, sizeof(buf)), 0);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(), val);
}

// ── Sentinel constant is identifiable ──────────────────────────

TEST_CASE(all_devices_sentinel_is_distinct) {
    // The sentinel must be representable and distinguishable from a
    // valid owned DeviceId (which would normally be a small integer).
    REQUIRE_EQ(myrd::aim::kAllDevices, 0x00FFFFFFu);
    myrd::aim::DeviceId sentinel{myrd::aim::kAllDevices};
    REQUIRE_EQ(sentinel.value, 0x00FFFFFFu);
}
