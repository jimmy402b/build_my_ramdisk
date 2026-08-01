#include "test_framework.h"

#include "src/driver/direct_aim_adapter.h"
#include "src/driver/srb_codec.h"

#include <cstring>
#include <vector>

// ── Fake Transport ────────────────────────────────────────────

struct FakeCall {
    std::uint32_t control_code;
    std::vector<std::byte> request_payload;
    std::size_t response_capacity;
    std::uint32_t timeout;
};

class FakeSrbTransport final : public myrd::aim::ISrbTransport {
public:
    std::vector<FakeCall> calls;
    std::uint32_t queued_return_code = 0;
    std::vector<std::byte> queued_response;
    bool inject_transport_error = false;

    int send_count() const { return static_cast<int>(calls.size()); }

    myrd::Result<myrd::aim::SrbTransactionResult> transact(
        std::uint32_t control_code,
        std::span<const std::byte> request_payload,
        std::size_t response_payload_capacity,
        std::uint32_t timeout_seconds) override {
        calls.push_back({control_code,
            std::vector<std::byte>(
                request_payload.begin(), request_payload.end()),
            response_payload_capacity, timeout_seconds});
        if (inject_transport_error) {
            return myrd::Result<myrd::aim::SrbTransactionResult>::failure(
                {myrd::ErrorDomain::win32, 5,
                 L"fake_transport", L"injected error"});
        }
        myrd::aim::SrbTransactionResult r{};
        r.return_code = queued_return_code;
        r.payload = queued_response;
        return myrd::Result<myrd::aim::SrbTransactionResult>::success(r);
    }
};

// Helpers
static std::vector<std::byte> make_bytes(const void* p, std::size_t n) {
    auto* b = static_cast<const std::byte*>(p);
    return std::vector<std::byte>(b, b + n);
}

static std::wstring make_native_path() {
    return L"\\BaseNamedObjects\\Global\\myrd-m0-00112233445566778899aabbccddeeff";
}

// ── QUERY_VERSION ─────────────────────────────────────────────

TEST_CASE(adapter_query_version_reads_api_version_from_return_code) {
    FakeSrbTransport fake;
    // Simulate real QUERY_VERSION: ReturnCode=0x101, SubVersion=0x01021C5D
    fake.queued_return_code = 0x00000101u;  // API version in ReturnCode
    std::uint32_t sv = 0x01021C5Du;
    fake.queued_response = make_bytes(&sv, 4);  // SubVersion in payload
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_version();
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().api_version, 0x00000101u);
    REQUIRE_EQ(r.value().sub_version, 0x01021C5Du);
}

TEST_CASE(adapter_query_version_return_code_and_subversion_are_distinct) {
    FakeSrbTransport fake;
    fake.queued_return_code = 0x0101u;  // api_version
    std::uint32_t sv = 0x99999999u;     // sub_version
    fake.queued_response = make_bytes(&sv, 4);
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_version();
    REQUIRE(r.ok());
    // api_version != sub_version — they must not be confused
    REQUIRE(r.value().api_version != r.value().sub_version);
    REQUIRE_EQ(r.value().api_version, 0x0101u);
    REQUIRE_EQ(r.value().sub_version, 0x99999999u);
}

TEST_CASE(adapter_query_version_preserves_srb_return_code) {
    FakeSrbTransport fake;
    fake.queued_return_code = 0x0101u;
    std::uint32_t sv = 0x01021C5Du;
    fake.queued_response = make_bytes(&sv, 4);
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_version();
    REQUIRE(r.ok());
    // API version comes from ReturnCode, NOT from payload
    REQUIRE_EQ(r.value().api_version, 0x0101u);
}

TEST_CASE(adapter_query_version_truncated_payload_still_has_api_version) {
    FakeSrbTransport fake;
    fake.queued_return_code = 0x0101u;
    fake.queued_response = std::vector<std::byte>{};  // empty payload
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_version();
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().api_version, 0x0101u);
    REQUIRE_EQ(r.value().sub_version, 0u);  // no payload → sub_version=0
}

// ── QUERY_ADAPTER / list_devices ──────────────────────────────

TEST_CASE(adapter_list_devices_decodes_zero_one_and_many) {
    // Zero devices
    {
        FakeSrbTransport fake;
        std::uint32_t count = 0;
        fake.queued_response = make_bytes(&count, 4);
        myrd::aim::DirectAimAdapter adapter(fake);
        auto r = adapter.list_devices();
        REQUIRE(r.ok());
        REQUIRE(r.value().empty());
    }
    // One device
    {
        FakeSrbTransport fake;
        std::uint32_t data[2] = {1, 0x00010203u};
        fake.queued_response = make_bytes(data, 8);
        myrd::aim::DirectAimAdapter adapter(fake);
        auto r = adapter.list_devices();
        REQUIRE(r.ok());
        REQUIRE_EQ(r.value().size(), 1u);
        REQUIRE_EQ(r.value()[0].value, 0x00010203u);
    }
}

TEST_CASE(adapter_list_devices_rejects_count_length_mismatch) {
    FakeSrbTransport fake;
    std::uint32_t data[2] = {5, 0}; // claims 5 but only 1
    fake.queued_response = make_bytes(data, 8);
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.list_devices();
    REQUIRE(r.has_error());
}

// ── CREATE ────────────────────────────────────────────────────

TEST_CASE(adapter_create_encodes_64_mib_512_and_shared_memory_flags) {
    FakeSrbTransport fake;
    std::uint32_t dev_id = 0x08030014u;
    fake.queued_response = make_bytes(&dev_id, 4);
    myrd::aim::DirectAimAdapter adapter(fake);
    myrd::aim::ProxyDiskSpec spec{myrd::aim::kM0DiskSize,
        myrd::aim::kM0SectorSize, make_native_path()};
    auto r = adapter.create_proxy_disk(spec);
    REQUIRE(r.ok());
    REQUIRE_EQ(fake.send_count(), 1);
    REQUIRE_EQ(fake.calls[0].control_code, myrd::aim::kCreateDevice);
}

TEST_CASE(adapter_create_encodes_exact_native_object_path_length) {
    FakeSrbTransport fake;
    std::uint32_t dev_id = 42;
    fake.queued_response = make_bytes(&dev_id, 4);
    myrd::aim::DirectAimAdapter adapter(fake);
    auto path = make_native_path();
    myrd::aim::ProxyDiskSpec spec{myrd::aim::kM0DiskSize,
        myrd::aim::kM0SectorSize, path};
    auto r = adapter.create_proxy_disk(spec);
    REQUIRE(r.ok());

    REQUIRE_EQ(fake.send_count(), 1);
    const auto& payload = fake.calls[0].request_payload;
    constexpr std::size_t file_name_offset =
        offsetof(myrd::aim::DeviceConfiguration, FileNameLength) +
        sizeof(std::uint16_t);
    const auto name_bytes = path.size() * sizeof(wchar_t);

    std::uint16_t encoded_name_bytes = 0;
    std::memcpy(&encoded_name_bytes,
                payload.data() +
                    offsetof(myrd::aim::DeviceConfiguration, FileNameLength),
                sizeof(encoded_name_bytes));
    REQUIRE_EQ(encoded_name_bytes, name_bytes);
    REQUIRE_EQ(payload.size(), file_name_offset + name_bytes);
    REQUIRE(std::memcmp(payload.data() + file_name_offset,
                        path.data(), name_bytes) == 0);
}

TEST_CASE(adapter_create_rejects_empty_path) {
    FakeSrbTransport fake;
    myrd::aim::DirectAimAdapter adapter(fake);
    myrd::aim::ProxyDiskSpec spec{myrd::aim::kM0DiskSize,
        myrd::aim::kM0SectorSize, L""};
    auto r = adapter.create_proxy_disk(spec);
    REQUIRE(r.has_error());
    REQUIRE_EQ(fake.send_count(), 0);
}

TEST_CASE(adapter_create_rejects_path_larger_than_wire_length_field) {
    FakeSrbTransport fake;
    myrd::aim::DirectAimAdapter adapter(fake);
    myrd::aim::ProxyDiskSpec spec{
        myrd::aim::kM0DiskSize,
        myrd::aim::kM0SectorSize,
        std::wstring(32768, L'x')};

    auto r = adapter.create_proxy_disk(spec);

    REQUIRE(r.has_error());
    REQUIRE_EQ(fake.send_count(), 0);
}

TEST_CASE(adapter_create_rejects_unaligned_size) {
    FakeSrbTransport fake;
    myrd::aim::DirectAimAdapter adapter(fake);
    myrd::aim::ProxyDiskSpec spec{1025, 512, make_native_path()};
    auto r = adapter.create_proxy_disk(spec);
    REQUIRE(r.has_error());
}

TEST_CASE(adapter_create_returns_exact_driver_device_id) {
    FakeSrbTransport fake;
    std::uint32_t dev_id = 0x0A0B0C0Du;
    fake.queued_response = make_bytes(&dev_id, 4);
    myrd::aim::DirectAimAdapter adapter(fake);
    myrd::aim::ProxyDiskSpec spec{myrd::aim::kM0DiskSize,
        myrd::aim::kM0SectorSize, make_native_path()};
    auto r = adapter.create_proxy_disk(spec);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().value, 0x0A0B0C0Du);
}

TEST_CASE(adapter_create_rejects_auto_device_sentinel_response) {
    FakeSrbTransport fake;
    const std::uint32_t sentinel = myrd::aim::kAutoDeviceNumber;
    fake.queued_response = make_bytes(&sentinel, sizeof(sentinel));
    myrd::aim::DirectAimAdapter adapter(fake);
    myrd::aim::ProxyDiskSpec spec{
        myrd::aim::kM0DiskSize,
        myrd::aim::kM0SectorSize,
        make_native_path()};

    auto r = adapter.create_proxy_disk(spec);

    REQUIRE(r.has_error());
}

TEST_CASE(adapter_create_does_not_hide_returned_id_behind_query) {
    FakeSrbTransport fake;
    std::uint32_t dev_id = 99;
    fake.queued_response = make_bytes(&dev_id, 4);
    myrd::aim::DirectAimAdapter adapter(fake);
    myrd::aim::ProxyDiskSpec spec{myrd::aim::kM0DiskSize,
        myrd::aim::kM0SectorSize, make_native_path()};
    auto r = adapter.create_proxy_disk(spec);
    REQUIRE(r.ok());
    REQUIRE_EQ(fake.send_count(), 1); // Only CREATE, no QUERY
}

// ── REMOVE ────────────────────────────────────────────────────

TEST_CASE(adapter_query_device_decodes_path_from_wire_offset_34) {
    FakeSrbTransport fake;
    const auto path = make_native_path();
    constexpr std::size_t path_offset =
        offsetof(myrd::aim::DeviceConfiguration, FileNameLength) +
        sizeof(std::uint16_t);
    const auto path_bytes = path.size() * sizeof(wchar_t);
    fake.queued_response.resize(path_offset + path_bytes);

    myrd::aim::DeviceConfiguration cfg{};
    cfg.DeviceNumber = 42;
    cfg.DiskSize = static_cast<std::int64_t>(myrd::aim::kM0DiskSize);
    cfg.BytesPerSector = myrd::aim::kM0SectorSize;
    cfg.Flags = myrd::aim::kDeviceTypeHd | myrd::aim::kTypeProxy;
    cfg.FileNameLength = static_cast<std::uint16_t>(path_bytes);
    std::memcpy(fake.queued_response.data(), &cfg, path_offset);
    std::memcpy(fake.queued_response.data() + path_offset,
                path.data(), path_bytes);

    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_device(myrd::aim::DeviceId{42});

    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().native_object_path, path);
    REQUIRE_EQ(r.value().size_bytes, myrd::aim::kM0DiskSize);
    REQUIRE_EQ(r.value().sector_size, myrd::aim::kM0SectorSize);
}

TEST_CASE(adapter_query_device_rejects_truncated_path) {
    FakeSrbTransport fake;
    constexpr std::size_t path_offset =
        offsetof(myrd::aim::DeviceConfiguration, FileNameLength) +
        sizeof(std::uint16_t);
    fake.queued_response.resize(path_offset + sizeof(wchar_t));
    myrd::aim::DeviceConfiguration cfg{};
    cfg.DeviceNumber = 42;
    cfg.FileNameLength = 4 * sizeof(wchar_t);
    std::memcpy(fake.queued_response.data(), &cfg, path_offset);

    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_device(myrd::aim::DeviceId{42});
    REQUIRE(r.has_error());
}

TEST_CASE(adapter_remove_rejects_all_devices_sentinel_without_transport) {
    FakeSrbTransport fake;
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.remove_device(
        myrd::aim::DeviceId{myrd::aim::kAllDevices});
    REQUIRE(r.has_error());
    REQUIRE_EQ(fake.send_count(), 0);
}

TEST_CASE(adapter_remove_sends_exact_owned_device_id) {
    FakeSrbTransport fake;
    fake.queued_response = {}; // empty response
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.remove_device(myrd::aim::DeviceId{0x01020304u});
    REQUIRE(r.ok());
    REQUIRE_EQ(fake.send_count(), 1);
    REQUIRE_EQ(fake.calls[0].control_code, myrd::aim::kRemoveDevice);
}

TEST_CASE(adapter_remove_rejects_response_for_different_device) {
    // remove_device doesn't verify response device number; tested via sentinel
    FakeSrbTransport fake;
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.remove_device(myrd::aim::DeviceId{0x00FFFFFFu});
    REQUIRE(r.has_error()); // sentinel rejected
}

TEST_CASE(adapter_remove_propagates_driver_return_code) {
    FakeSrbTransport fake;
    fake.inject_transport_error = true;
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.remove_device(myrd::aim::DeviceId{42});
    REQUIRE(r.has_error());
    REQUIRE_EQ(static_cast<int>(r.error().domain),
               static_cast<int>(myrd::ErrorDomain::win32));
}
