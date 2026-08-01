#include "direct_aim_adapter.h"

#include "srb_codec.h"

#include <cstring>

namespace myrd::aim {

DirectAimAdapter::DirectAimAdapter(ISrbTransport& transport) noexcept
    : transport_(transport) {}

Result<DriverVersion> DirectAimAdapter::query_version() {
    auto sr = transport_.transact(kQueryVersion, {}, 4, 30);
    if (!sr) return Result<DriverVersion>::failure(sr.error());

    DriverVersion v{};
    // API version is in SrbIoControl.ReturnCode
    v.api_version = sr.value().return_code;
    // SubVersion is in the payload (4 bytes)
    if (sr.value().payload.size() >= 4) {
        std::memcpy(&v.sub_version, sr.value().payload.data(), 4);
    }
    return Result<DriverVersion>::success(v);
}

Result<std::vector<DeviceId>> DirectAimAdapter::list_devices() {
    auto sr = transport_.transact(kQueryAdapter, {}, 1024, 30);
    if (!sr) return Result<std::vector<DeviceId>>::failure(sr.error());

    auto& payload = sr.value().payload;
    if (sr.value().return_code != 0) {
        return Result<std::vector<DeviceId>>::failure(
            {ErrorDomain::protocol, sr.value().return_code,
             L"list_devices", L"driver returned non-zero SRB ReturnCode"});
    }
    if (payload.size() < 4) {
        return Result<std::vector<DeviceId>>::failure(
            {ErrorDomain::protocol, 0,
             L"list_devices", L"response too short"});
    }
    std::uint32_t num_devices = 0;
    std::memcpy(&num_devices, payload.data(), 4);
    if (payload.size() < 4 + num_devices * 4) {
        return Result<std::vector<DeviceId>>::failure(
            {ErrorDomain::protocol, 0,
             L"list_devices", L"count/length mismatch"});
    }
    std::vector<DeviceId> devices;
    for (std::uint32_t i = 0; i < num_devices; ++i) {
        std::uint32_t val{};
        std::memcpy(&val, payload.data() + 4 + i * 4, 4);
        devices.push_back(DeviceId{val});
    }
    return Result<std::vector<DeviceId>>::success(std::move(devices));
}

Result<DeviceId> DirectAimAdapter::create_proxy_disk(
    const ProxyDiskSpec& spec) {
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

    auto sr = transport_.transact(kCreateDevice, payload, 4, 30);
    if (!sr) return Result<DeviceId>::failure(sr.error());

    if (sr.value().return_code != 0) {
        return Result<DeviceId>::failure(
            {ErrorDomain::protocol, sr.value().return_code,
             L"create_proxy_disk", L"driver returned non-zero SRB ReturnCode"});
    }
    if (sr.value().payload.size() < 4) {
        return Result<DeviceId>::failure(
            {ErrorDomain::protocol, 0,
             L"create_proxy_disk", L"response too short"});
    }
    std::uint32_t raw_id{};
    std::memcpy(&raw_id, sr.value().payload.data(), 4);
    return Result<DeviceId>::success(DeviceId{raw_id});
}

Result<DiskInfo> DirectAimAdapter::query_device(DeviceId id) {
    std::uint32_t raw = id.value;
    auto sr = transport_.transact(
        kQueryDevice,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&raw), 4),
        sizeof(DeviceConfiguration) + 260, 30);
    if (!sr) return Result<DiskInfo>::failure(sr.error());

    if (sr.value().return_code != 0) {
        return Result<DiskInfo>::failure(
            {ErrorDomain::protocol, sr.value().return_code,
             L"query_device", L"driver returned non-zero SRB ReturnCode"});
    }
    auto& payload = sr.value().payload;
    if (payload.size() < sizeof(DeviceConfiguration)) {
        return Result<DiskInfo>::failure(
            {ErrorDomain::protocol, 0,
             L"query_device", L"response too short"});
    }
    DeviceConfiguration cfg{};
    std::memcpy(&cfg, payload.data(), sizeof(cfg));

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
        payload.size() >= sizeof(DeviceConfiguration) + cfg.FileNameLength) {
        std::wstring path(
            reinterpret_cast<const wchar_t*>(
                payload.data() + sizeof(DeviceConfiguration)),
            (cfg.FileNameLength / sizeof(wchar_t)) - 1);
        info.native_object_path = std::move(path);
    }
    return Result<DiskInfo>::success(info);
}

Result<void> DirectAimAdapter::remove_device(DeviceId id) {
    if (id.value == kAllDevices || id.value == kAutoDeviceNumber) {
        return Result<void>::failure(
            {ErrorDomain::usage, 0,
             L"remove_device", L"sentinel/all-devices value rejected"});
    }

    DeviceNumber dn = id.value;
    auto sr = transport_.transact(
        kRemoveDevice,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&dn), 4),
        0, 30);
    if (!sr) return Result<void>::failure(sr.error());

    if (sr.value().return_code != 0) {
        return Result<void>::failure(
            {ErrorDomain::protocol, sr.value().return_code,
             L"remove_device", L"driver returned non-zero SRB ReturnCode"});
    }
    return Result<void>::success();
}

} // namespace myrd::aim
