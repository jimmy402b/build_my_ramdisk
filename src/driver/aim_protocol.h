#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "src/common/result.h"

// AIM protocol ABI — all values from audited contract
// Upstream commit: d379caa027d21902e45fce8c9daf02fd116c2dda

namespace myrd::aim {

// ── Device identifier ──────────────────────────────────────────

struct DeviceId {
    std::uint32_t value;

    bool operator==(DeviceId other) const noexcept {
        return value == other.value;
    }
    bool operator!=(DeviceId other) const noexcept {
        return value != other.value;
    }
};

struct DeviceAddress {
    std::uint8_t path_id;
    std::uint8_t target_id;
    std::uint8_t lun;
};

inline DeviceAddress decode_device_address(DeviceId id) noexcept {
    DeviceAddress addr{};
    addr.path_id  = static_cast<std::uint8_t>(id.value & 0xFF);
    addr.target_id = static_cast<std::uint8_t>((id.value >> 8) & 0xFF);
    addr.lun       = static_cast<std::uint8_t>((id.value >> 16) & 0xFF);
    return addr;
}

// ── Fixed M0 parameters ────────────────────────────────────────

constexpr std::uint64_t kM0DiskSize   = 64ull * 1024ull * 1024ull;
constexpr std::uint32_t kM0SectorSize = 512;

struct ProxyDiskSpec {
    std::uint64_t size_bytes;
    std::uint32_t sector_size;
    std::wstring  native_object_path;
};

// ── Control codes (SMP_IMSCSI | offset) ────────────────────────

constexpr std::uint32_t kSmpImScsi      = 0x83730000u;
constexpr std::uint32_t kQueryVersion    = 0x83730800u;  // SMP_IMSCSI | 0x800
constexpr std::uint32_t kCreateDevice    = 0x83730801u;  // SMP_IMSCSI | 0x801
constexpr std::uint32_t kQueryDevice     = 0x83730802u;  // SMP_IMSCSI | 0x802
constexpr std::uint32_t kQueryAdapter    = 0x83730803u;  // SMP_IMSCSI | 0x803
constexpr std::uint32_t kRemoveDevice    = 0x83730806u;  // SMP_IMSCSI | 0x806

// ── Device type / proxy constants ──────────────────────────────

constexpr std::uint32_t kDeviceTypeHd   = 0x00000010u;
constexpr std::uint32_t kTypeProxy       = 0x00000300u;
constexpr std::uint32_t kProxyTypeShm    = 0x00003000u;
constexpr std::uint32_t kOptionSparseFile = 0x00000004u;
constexpr std::uint32_t kAllDevices      = 0x00FFFFFFu;
constexpr std::uint32_t kAutoDeviceNumber = 0x00FFFFFFu;
constexpr std::uint32_t kDriverVersion   = 0x0101u;

// M0 CREATE flags: HD | PROXY | SHM
constexpr std::uint32_t kM0CreateFlags =
    kDeviceTypeHd | kTypeProxy | kProxyTypeShm;  // 0x00003310
// QUERY_DEVICE reconstructs persistent device semantics and does not return
// the proxy transport subtype (see pinned srbioctl.cpp ImScsiQueryDevice).
constexpr std::uint32_t kM0QueryFlags =
    kDeviceTypeHd | kTypeProxy;  // 0x00000310
// QUERY_DEVICE maps an INFO SupportsZero response to the historical
// IMSCSI_OPTION_SPARSE_FILE bit. M1 advertises ZERO, unlike M0.
constexpr std::uint32_t kM1QueryFlags =
    kM0QueryFlags | kOptionSparseFile;  // 0x00000314

// ── SRB signature ──────────────────────────────────────────────

constexpr const char kSrbSignature[9] = "PhDskMnt";  // 8 chars + NUL

// ── SRB_IO_CONTROL (Windows SDK ntddscsi.h layout, x64) ────────
// Correct field order verified against Windows SDK 10.0.26100.0:
//   ULONG HeaderLength;  // offset 0
//   UCHAR Signature[8];  // offset 4
//   ULONG Timeout;       // offset 12
//   ULONG ControlCode;   // offset 16
//   ULONG ReturnCode;    // offset 20
//   ULONG Length;        // offset 24
// sizeof == 28 on x64

#pragma pack(push, 1)
struct SrbIoControl {
    std::uint32_t HeaderLength;
    std::uint8_t  Signature[8];
    std::uint32_t Timeout;
    std::uint32_t ControlCode;
    std::uint32_t ReturnCode;
    std::uint32_t Length;
};
#pragma pack(pop)

static_assert(sizeof(SrbIoControl) == 28, "SRB_IO_CONTROL size mismatch");
static_assert(offsetof(SrbIoControl, HeaderLength) == 0);
static_assert(offsetof(SrbIoControl, Signature) == 4);
static_assert(offsetof(SrbIoControl, Timeout) == 12);
static_assert(offsetof(SrbIoControl, ControlCode) == 16);
static_assert(offsetof(SrbIoControl, ReturnCode) == 20);
static_assert(offsetof(SrbIoControl, Length) == 24);

// ── DEVICE_NUMBER ──────────────────────────────────────────────

using DeviceNumber = std::uint32_t;

inline std::uint8_t devnum_path_id(DeviceNumber dn) noexcept {
    return static_cast<std::uint8_t>(dn & 0xFFu);
}
inline std::uint8_t devnum_target_id(DeviceNumber dn) noexcept {
    return static_cast<std::uint8_t>((dn >> 8) & 0xFFu);
}
inline std::uint8_t devnum_lun(DeviceNumber dn) noexcept {
    return static_cast<std::uint8_t>((dn >> 16) & 0xFFu);
}

// ── IMSCSI_DEVICE_CONFIGURATION (pack 4) ───────────────────────

#pragma pack(push, 4)
struct DeviceConfiguration {
    std::uint32_t DeviceNumber;
    std::int64_t  DiskSize;
    std::uint32_t BytesPerSector;
    std::uint32_t Reserved;
    std::int64_t  ImageOffset;
    std::uint32_t Flags;
    std::uint16_t FileNameLength;
    // WCHAR FileName[] follows
};
#pragma pack(pop)

// sizeof may be 34 or 36 depending on tail-padding under pack(4);
// key offsets are the normative contract.
static_assert(offsetof(DeviceConfiguration, DeviceNumber) == 0);
static_assert(offsetof(DeviceConfiguration, DiskSize) == 4);
static_assert(offsetof(DeviceConfiguration, BytesPerSector) == 12);
static_assert(offsetof(DeviceConfiguration, Reserved) == 16);
static_assert(offsetof(DeviceConfiguration, ImageOffset) == 20);
static_assert(offsetof(DeviceConfiguration, Flags) == 28);
static_assert(offsetof(DeviceConfiguration, FileNameLength) == 32);

// ── SRB-wrapped structures ─────────────────────────────────────

#pragma pack(push, 4)
struct SrbCreateData {
    SrbIoControl         SrbIoControl;
    DeviceConfiguration  Fields;
};
#pragma pack(pop)

// Size depends on tail-padding of DeviceConfiguration; offset is what matters.
static_assert(offsetof(SrbCreateData, Fields) == 28);

#pragma pack(push, 4)
struct SrbQueryAdapter {
    SrbIoControl   SrbIoControl;
    std::uint32_t  NumberOfDevices;
    // DeviceNumber DeviceList[] follows
};
#pragma pack(pop)

static_assert(sizeof(SrbQueryAdapter) == 32);

#pragma pack(push, 4)
struct SrbRemoveDevice {
    SrbIoControl   SrbIoControl;
    std::uint32_t  DeviceNumber;
};
#pragma pack(pop)

static_assert(sizeof(SrbRemoveDevice) == 32);
static_assert(offsetof(SrbRemoveDevice, DeviceNumber) == 28);

#pragma pack(push, 4)
struct SrbQueryVersion {
    SrbIoControl  SrbIoControl;
    std::uint32_t SubVersion;
};
#pragma pack(pop)

static_assert(sizeof(SrbQueryVersion) == 32);

} // namespace myrd::aim
