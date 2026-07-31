#include "direct_aim_adapter.h"

#include "srb_codec.h"

#include <cstring>

namespace myrd::aim {

DirectAimAdapter::DirectAimAdapter(ISrbTransport& transport) noexcept
    : transport_(transport) {}

Result<DriverVersion> DirectAimAdapter::query_version() {
    auto frame = build_srb_frame(kQueryVersion, {}, 4, 30);
    if (!frame) return Result<DriverVersion>::failure(frame.error());

    auto resp = transport_.transact(
        kQueryVersion, {}, 4, 30);
    if (!resp) return Result<DriverVersion>::failure(resp.error());

    if (resp.value().size() < 4) {
        return Result<DriverVersion>::failure(
            {ErrorDomain::protocol, 0,
             L"query_version", L"response payload too short"});
    }
    std::uint32_t raw{};
    std::memcpy(&raw, resp.value().data(), 4);
    return Result<DriverVersion>::success(DriverVersion{raw});
}

Result<std::vector<DeviceId>> DirectAimAdapter::list_devices() {
    std::uint32_t num_devices = 0;
    auto resp = transport_.transact(
        kQueryAdapter, {}, 1024, 30);
    if (!resp) return Result<std::vector<DeviceId>>::failure(resp.error());

    if (resp.value().size() < 4) {
        return Result<std::vector<DeviceId>>::failure(
            {ErrorDomain::protocol, 0,
             L"list_devices", L"response too short"});
    }
    std::memcpy(&num_devices, resp.value().data(), 4);

    if (resp.value().size() < 4 + num_devices * 4) {
        return Result<std::vector<DeviceId>>::failure(
            {ErrorDomain::protocol, 0,
             L"list_devices", L"count/length mismatch"});
    }
    std::vector<DeviceId> devices;
    for (std::uint32_t i = 0; i < num_devices; ++i) {
        std::uint32_t val{};
        std::memcpy(&val, resp.value().data() + 4 + i * 4, 4);
        devices.push_back(DeviceId{val});
    }
    return Result<std::vector<DeviceId>>::success(std::move(devices));
}

Result<DeviceId> DirectAimAdapter::create_proxy_disk(
    const ProxyDiskSpec& spec) {
    // Encode request
    if (spec.native_object_path.empty()) {
        return Result<DeviceId>::failure(
            {ErrorDomain::usage, 0,
             L"create_proxy_disk", L"empty native object path"});
    }
    if (spec.size_bytes % spec.sector_size != 0) {
        return Result<DeviceId>::failure(
            {ErrorDomain::usage, 0,
             L"create_proxy_disk", L"size not aligned to sector size"});
    }

    // Build DeviceConfiguration payload
    std::uint16_t name_bytes = static_cast<std::uint16_t>(
        spec.native_object_path.size() * sizeof(wchar_t) + sizeof(wchar_t));
    std::size_t pl_size = sizeof(DeviceConfiguration) + name_bytes;
    std::vector<std::byte> payload(pl_size);

    DeviceConfiguration cfg{};
    cfg.DeviceNumber = kAutoDeviceNumber;
    cfg.DiskSize = static_cast<std::int64_t>(spec.size_bytes);
    cfg.BytesPerSector = spec.sector_size;
    cfg.ImageOffset = 0;
    cfg.Flags = kM0CreateFlags;
    cfg.FileNameLength = name_bytes;
    std::memcpy(payload.data(), &cfg, sizeof(cfg));
    std::memcpy(payload.data() + sizeof(cfg),
                spec.native_object_path.data(), name_bytes);

    auto resp = transport_.transact(kCreateDevice, payload, 4, 30);
    if (!resp) return Result<DeviceId>::failure(resp.error());

    if (resp.value().size() < 4) {
        return Result<DeviceId>::failure(
            {ErrorDomain::protocol, 0,
             L"create_proxy_disk", L"response too short"});
    }
    std::uint32_t raw_id{};
    std::memcpy(&raw_id, resp.value().data(), 4);
    return Result<DeviceId>::success(DeviceId{raw_id});
}

Result<DiskInfo> DirectAimAdapter::query_device(DeviceId id) {
    std::uint32_t raw = id.value;
    auto resp = transport_.transact(
        kQueryDevice,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&raw), 4),
        sizeof(DeviceConfiguration) + 260, 30);
    if (!resp) return Result<DiskInfo>::failure(resp.error());

    if (resp.value().size() < sizeof(DeviceConfiguration)) {
        return Result<DiskInfo>::failure(
            {ErrorDomain::protocol, 0,
             L"query_device", L"response too short"});
    }
    DeviceConfiguration cfg{};
    std::memcpy(&cfg, resp.value().data(), sizeof(cfg));

    // Verify returned id matches requested
    if (cfg.DeviceNumber != id.value) {
        return Result<DiskInfo>::failure(
            {ErrorDomain::protocol, 0,
             L"query_device", L"returned device id does not match request"});
    }

    DiskInfo info{};
    info.id = id;
    info.size_bytes = static_cast<std::uint64_t>(cfg.DiskSize);
    info.sector_size = cfg.BytesPerSector;
    info.flags = cfg.Flags;

    if (cfg.FileNameLength > 0 &&
        resp.value().size() >= sizeof(DeviceConfiguration) + cfg.FileNameLength) {
        std::wstring path(
            reinterpret_cast<const wchar_t*>(
                resp.value().data() + sizeof(DeviceConfiguration)),
            (cfg.FileNameLength / sizeof(wchar_t)) - 1);
        info.native_object_path = std::move(path);
    }
    return Result<DiskInfo>::success(info);
}

Result<void> DirectAimAdapter::remove_device(DeviceId id) {
    // Safety: reject sentinel BEFORE any transport call
    if (id.value == kAllDevices || id.value == kAutoDeviceNumber) {
        return Result<void>::failure(
            {ErrorDomain::usage, 0,
             L"remove_device", L"sentinel/all-devices value rejected"});
    }

    DeviceNumber dn = id.value;
    auto resp = transport_.transact(
        kRemoveDevice,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&dn), 4),
        0, 30);
    if (!resp) return Result<void>::failure(resp.error());

    return Result<void>::success();
}

} // namespace myrd::aim
