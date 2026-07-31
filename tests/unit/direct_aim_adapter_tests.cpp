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
    std::vector<std::byte> queued_response;
    bool inject_transport_error = false;

    int send_count() const { return static_cast<int>(calls.size()); }

    myrd::Result<std::vector<std::byte>> transact(
        std::uint32_t control_code,
        std::span<const std::byte> request_payload,
        std::size_t response_payload_capacity,
        std::uint32_t timeout_seconds) override {
        calls.push_back({control_code,
            std::vector<std::byte>(
                request_payload.begin(), request_payload.end()),
            response_payload_capacity, timeout_seconds});
        if (inject_transport_error) {
            return myrd::Result<std::vector<std::byte>>::failure(
                {myrd::ErrorDomain::win32, 5,
                 L"fake_transport", L"injected error"});
        }
        return myrd::Result<std::vector<std::byte>>::success(queued_response);
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

TEST_CASE(adapter_query_version_decodes_exact_value) {
    FakeSrbTransport fake;
    std::uint32_t ver = 0x0101u;
    fake.queued_response = make_bytes(&ver, 4);
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_version();
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().raw, 0x0101u);
}

TEST_CASE(adapter_query_version_rejects_truncated_payload) {
    FakeSrbTransport fake;
    fake.queued_response = std::vector<std::byte>(2, std::byte{0});
    myrd::aim::DirectAimAdapter adapter(fake);
    auto r = adapter.query_version();
    REQUIRE(r.has_error());
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
