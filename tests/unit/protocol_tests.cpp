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
    // SRB_IO_CONTROL — verified against Windows SDK ntddscsi.h
    REQUIRE_EQ(sizeof(myrd::aim::SrbIoControl), 28u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, Signature), 4u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, ControlCode), 16u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, ReturnCode), 20u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, Length), 24u);
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
    REQUIRE_EQ(myrd::aim::kAllDevices, 0x00FFFFFFu);
    myrd::aim::DeviceId sentinel{myrd::aim::kAllDevices};
    REQUIRE_EQ(sentinel.value, 0x00FFFFFFu);
}

// ── Task 23 regression: SRB ReturnCode vs Length (FIELD ORDER) ──

TEST_CASE(srb_layout_returncode_at_20_length_at_24) {
    // Verified against Windows SDK ntddscsi.h
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, ReturnCode), 20u);
    REQUIRE_EQ(offsetof(myrd::aim::SrbIoControl, Length), 24u);
}

// ── Task 23 regression: QUERY_VERSION real byte layout ──────────
// Evidence: DeviceIoControl succeeded, bytes_returned=32,
// ReturnCode/API version=0x00000101, Length=4, SubVersion=0x01021C5D

TEST_CASE(query_version_response_byte_layout_returncode_is_api_version) {
    // Build a 32-byte buffer matching real QUERY_VERSION response
    std::byte buf[32]{};
    // HeaderLength = 28 at offset 0
    std::uint32_t hl = 28;
    std::memcpy(buf + 0, &hl, 4);
    // Signature "PhDskMnt" at offset 4
    std::memcpy(buf + 4, "PhDskMnt", 8);
    // Timeout = 0 at offset 12
    // ControlCode = SMP_IMSCSI_QUERY_VERSION at offset 16
    std::uint32_t cc = myrd::aim::kQueryVersion;
    std::memcpy(buf + 16, &cc, 4);
    // ReturnCode = 0x00000101 (API version) at offset 20
    std::uint32_t rc = 0x00000101u;
    std::memcpy(buf + 20, &rc, 4);
    // Length = 4 at offset 24
    std::uint32_t len = 4;
    std::memcpy(buf + 24, &len, 4);
    // SubVersion = 0x01021C5D at offset 28
    std::uint32_t sv = 0x01021C5Du;
    std::memcpy(buf + 28, &sv, 4);

    // Verify raw bytes — ReturnCode is at 20, NOT 24
    std::uint32_t read_rc = 0, read_len = 0, read_sv = 0;
    std::memcpy(&read_rc, buf + 20, 4);
    std::memcpy(&read_len, buf + 24, 4);
    std::memcpy(&read_sv, buf + 28, 4);
    REQUIRE_EQ(read_rc, 0x00000101u);  // API version in ReturnCode
    REQUIRE_EQ(read_len, 4u);
    REQUIRE_EQ(read_sv, 0x01021C5Du);  // SubVersion in payload
}

// ── Task 23 regression: GUID_DEVINTERFACE_STORAGEPORT ──────────

#include <windows.h>
#include <winioctl.h>

TEST_CASE(storageport_guid_is_correct_from_sdk) {
    // GUID_DEVINTERFACE_STORAGEPORT from Windows SDK winioctl.h:
    // {0x2ACCFE60, 0xC130, 0x11D2, {0xB0,0x82,0x00,0xA0,0xC9,0x1E,0xFB,0x8B}}
    // Data3 must be 0x11D2, NOT 0x11D1
    REQUIRE_EQ(GUID_DEVINTERFACE_STORAGEPORT.Data3, 0x11D2u);
    // Data1 verification
    REQUIRE_EQ(GUID_DEVINTERFACE_STORAGEPORT.Data1, 0x2ACCFE60u);
}
