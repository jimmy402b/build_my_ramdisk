# AIM M0 Protocol Contract

> **Upstream commit**: `d379caa027d21902e45fce8c9daf02fd116c2dda`
>
> **Repository**: `https://github.com/ArsenalRecon/Arsenal-Image-Mounter.git`
>
> **Normative sources**:
> 1. `Unmanaged Source/phdskmnt/inc/common.h`
> 2. `Unmanaged Source/phdskmnt/inc/imdproxy.h`
> 3. `Unmanaged Source/phdskmnt/proxy.cpp`
> 4. `Managed Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs`

## 1. SCSI Miniport Control Codes

| Local symbol | Upstream symbol | Exact value | Source file:line | Notes |
|---|---|---|---|---|
| `kSmpImScsi` | `SMP_IMSCSI` | `0x83730000` | common.h:565 | Base control code |
| `kQueryVersion` | `SMP_IMSCSI_QUERY_VERSION` | `0x83730800` | common.h:566 | `SMP_IMSCSI \| 0x800` |
| `kCreateDevice` | `SMP_IMSCSI_CREATE_DEVICE` | `0x83730801` | common.h:567 | `SMP_IMSCSI \| 0x801` |
| `kQueryDevice` | `SMP_IMSCSI_QUERY_DEVICE` | `0x83730802` | common.h:568 | `SMP_IMSCSI \| 0x802` |
| `kQueryAdapter` | `SMP_IMSCSI_QUERY_ADAPTER` | `0x83730803` | common.h:569 | `SMP_IMSCSI \| 0x803` |
| `kRemoveDevice` | `SMP_IMSCSI_REMOVE_DEVICE` | `0x83730806` | common.h:572 | `SMP_IMSCSI \| 0x806` |

## 2. Device Type and Proxy Constants

| Local symbol | Upstream symbol | Exact value | Source file:line | Notes |
|---|---|---|---|---|
| `kDeviceTypeHd` | `IMSCSI_DEVICE_TYPE_HD` | `0x00000010` | common.h:62 | Hard disk device type |
| `kTypeProxy` | `IMSCSI_TYPE_PROXY` | `0x00000300` | common.h:78 | Proxy-backed device |
| `kProxyTypeShm` | `IMSCSI_PROXY_TYPE_SHM` | `0x00003000` | common.h:90 | Shared memory proxy |
| `kAllDevices` | `IMSCSI_ALL_DEVICES` | `0x00FFFFFFUL` | common.h:134 | Remove-all sentinel |
| `kAutoDeviceNumber` | `IMSCSI_AUTO_DEVICE_NUMBER` | `0x00FFFFFFUL` | common.h:137 | Auto-allocation sentinel |
| `kDriverVersion` | `IMSCSI_DRIVER_VERSION` | `0x0101` | common.h:31 | Driver API version |

## 3. IMDPROXY Request Codes

| Local symbol | Upstream symbol | Exact value | Source file:line | Notes |
|---|---|---|---|---|
| `kProxyReqInfo` | `IMDPROXY_REQ_INFO` | `1` | imdproxy.h:59 | Enumerated: `IMDPROXY_REQ_NULL=0` |
| `kProxyReqRead` | `IMDPROXY_REQ_READ` | `2` | imdproxy.h:60 | READ request |
| `kProxyReqWrite` | `IMDPROXY_REQ_WRITE` | `3` | imdproxy.h:61 | WRITE request |
| `kProxyReqClose` | `IMDPROXY_REQ_CLOSE` | `5` | imdproxy.h:63 | After `IMDPROXY_REQ_CONNECT=4` |

## 4. IMDPROXY Flags

| Local symbol | Upstream symbol | Exact value | Source file:line | Notes |
|---|---|---|---|---|
| `kFlagRo` | `IMDPROXY_FLAG_RO` | `0x01` | imdproxy.h:49 | Read-only |
| `kFlagSupportsUnmap` | `IMDPROXY_FLAG_SUPPORTS_UNMAP` | `0x02` | imdproxy.h:50 | NOT in M0 |
| `kFlagSupportsZero` | `IMDPROXY_FLAG_SUPPORTS_ZERO` | `0x04` | imdproxy.h:51 | NOT in M0 |
| `kFlagSupportsScsi` | `IMDPROXY_FLAG_SUPPORTS_SCSI` | `0x08` | imdproxy.h:52 | NOT in M0 |
| `kFlagSupportsShared` | `IMDPROXY_FLAG_SUPPORTS_SHARED` | `0x10` | imdproxy.h:53 | NOT in M0 |
| `kFlagKeepOpen` | `IMDPROXY_FLAG_KEEP_OPEN` | `0x20` | imdproxy.h:54 | NOT in M0 |

M0 INFO response flags: `0` (none of the optional capability flags advertised).

## 5. SRB Protocol Structures

### 5.1 SRB_IO_CONTROL (Windows SDK)

Standard Windows structure. The signature `"PhDskMnt"` (8 bytes, no NUL) is set by `ImScsiInitializeSrbIoBlock` (common.h:361, 375-377).

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| HeaderLength | ULONG | 4 | 0 | Set to `sizeof(SRB_IO_CONTROL)` |
| Signature[8] | UCHAR | 8 | 4 | `"PhDskMnt"` — zero-padded to 8 bytes |
| Timeout | ULONG | 4 | 12 | Set per-call (30 s default) |
| ControlCode | ULONG | 4 | 16 | SMP_IMSCSI_* value |
| ReturnCode | ULONG | 4 | 20 | Set to 0 before call; driver sets result. For QUERY_VERSION: = API version (0x0101) |
| Length | ULONG | 4 | 24 | Payload length after header |

`sizeof(SRB_IO_CONTROL)` = 28 bytes on x64.
Corrected 2026-08-01 per Windows SDK ntddscsi.h and AIM source srbioctl.cpp (upstream commit d379caa).

### 5.2 DEVICE_NUMBER

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| PathId | UCHAR | 1 | 0 | Bits 0-7 of LongNumber |
| TargetId | UCHAR | 1 | 1 | Bits 8-15 of LongNumber |
| Lun | UCHAR | 1 | 2 | Bits 16-23 of LongNumber |
| LongNumber | ULONG | 4 | 0 | Union overlay |

`sizeof(DEVICE_NUMBER)` = 4, `alignof(DEVICE_NUMBER)` = 4.

### 5.3 IMSCSI_DEVICE_CONFIGURATION

`#pragma pack(push, 4)` applied. `sizeof` = 34 + variable FileName.

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| DeviceNumber | DEVICE_NUMBER | 4 | 0 | Device identifier union |
| DiskSize | LARGE_INTEGER | 8 | 4 | Total size in bytes |
| BytesPerSector | ULONG | 4 | 12 | 512 for M0 |
| WriteOverlayFileNameLength / Reserved | USHORT / ULONG | 4 | 16 | Not used in M0 |
| ImageOffset | LARGE_INTEGER | 8 | 20 | Image file offset |
| Flags | ULONG | 4 | 28 | Creation flags |
| FileNameLength | USHORT | 2 | 32 | Length in **bytes** of FileName (includes NUL if present) |
| FileName[] | WCHAR | variable | 34 | UTF-16LE string |

`sizeof` (without FileName) = 34 bytes.

M0 CREATE flags: `IMSCSI_DEVICE_TYPE_HD | IMSCSI_TYPE_PROXY | IMSCSI_PROXY_TYPE_SHM` = `0x00003310`.

### 5.4 SRB_IMSCSI_CREATE_DATA

`SRB_IO_CONTROL` (28 bytes) + `IMSCSI_DEVICE_CONFIGURATION` (34+ bytes).

Total header size before FileName: 28 + 34 = 62 bytes.

### 5.5 SRB_IMSCSI_QUERY_ADAPTER

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| SrbIoControl | SRB_IO_CONTROL | 28 | 0 | Standard SRB header |
| NumberOfDevices | ULONG | 4 | 28 | Count of DeviceList entries |
| DeviceList[] | DEVICE_NUMBER | 4*N | 32 | Variable-length array |

### 5.6 SRB_IMSCSI_REMOVE_DEVICE

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| SrbIoControl | SRB_IO_CONTROL | 28 | 0 | Standard SRB header |
| DeviceNumber | DEVICE_NUMBER | 4 | 28 | Exact device to remove |

`sizeof(SRB_IMSCSI_REMOVE_DEVICE)` = 32 bytes.

### 5.7 SRB_IMSCSI_QUERY_VERSION

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| SrbIoControl | SRB_IO_CONTROL | 28 | 0 | ReturnCode = API version |
| SubVersion | ULONG | 4 | 28 | Revision version |

`sizeof(SRB_IMSCSI_QUERY_VERSION)` = 32 bytes.

## 6. IMDPROXY Wire Structures

All use natural alignment (no pack pragma in imdproxy.h). All use `ULONGLONG` (8 bytes).

### 6.1 IMDPROXY_INFO_RESP (Response to INFO)

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| file_size | ULONGLONG | 8 | 0 | 64 MiB for M0 |
| req_alignment | ULONGLONG | 8 | 8 | 512 for M0 |
| flags | ULONGLONG | 8 | 16 | 0 for M0 |

`sizeof(IMDPROXY_INFO_RESP)` = 24 bytes.

### 6.2 IMDPROXY_READ_REQ

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| request_code | ULONGLONG | 8 | 0 | `IMDPROXY_REQ_READ` = 2 |
| offset | ULONGLONG | 8 | 8 | Byte offset in device |
| length | ULONGLONG | 8 | 16 | Bytes to read |

`sizeof(IMDPROXY_READ_REQ)` = 24 bytes.

### 6.3 IMDPROXY_READ_RESP

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| errorno | ULONGLONG | 8 | 0 | 0 = success |
| length | ULONGLONG | 8 | 8 | Bytes transferred |

`sizeof(IMDPROXY_READ_RESP)` = 16 bytes.

### 6.4 IMDPROXY_WRITE_REQ

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| request_code | ULONGLONG | 8 | 0 | `IMDPROXY_REQ_WRITE` = 3 |
| offset | ULONGLONG | 8 | 8 | Byte offset in device |
| length | ULONGLONG | 8 | 16 | Bytes to write |

`sizeof(IMDPROXY_WRITE_REQ)` = 24 bytes.

### 6.5 IMDPROXY_WRITE_RESP

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| errorno | ULONGLONG | 8 | 0 | 0 = success |
| length | ULONGLONG | 8 | 8 | Bytes transferred |

`sizeof(IMDPROXY_WRITE_RESP)` = 16 bytes.

### 6.6 IMDPROXY_CLOSE_REQ

| Field | Type | Size | Offset | Notes |
|---|---|---|---|---|
| request_code | ULONGLONG | 8 | 0 | `IMDPROXY_REQ_CLOSE` = 5 |

`sizeof(IMDPROXY_CLOSE_REQ)` = 8 bytes.

## 7. Shared Memory Object Naming

### 7.1 Object Name Format

For a session identified by `<name>`, the following kernel objects are created:

| Object | Win32 Name | Kernel Native Path | Source |
|---|---|---|---|
| File Mapping | `Global\<name>` | `\BaseNamedObjects\Global\<name>` | DevioShmService.cs:352 |
| Request Event | `Global\<name>_Request` | — | proxy.cpp:440, DevioShmService.cs:134 |
| Response Event | `Global\<name>_Response` | — | proxy.cpp:472, DevioShmService.cs:136 |
| Server Mutex | `Global\<name>_Server` | — | DevioShmService.cs:138 |

The driver-side filename passed via `IMSCSI_DEVICE_CONFIGURATION.FileName` uses the native path `\BaseNamedObjects\Global\<name>` (ScsiAdapter.cs:352,594). When the filename does not already begin with `\BaseNamedObjects\`, the managed adapter prepends `\BaseNamedObjects\Global\` (ScsiAdapter.cs:352).

### 7.2 Mapping Layout

| Region | Offset | Size | Notes |
|---|---|---|---|
| Header | 0 | 4096 bytes | `IMDPROXY_HEADER_SIZE` |
| Data area | 4096 | 8 MiB | Read/write data |

Total mapping size = 8 MiB + 4096 bytes.

## 8. Request-Response Protocol

### 8.1 General Flow

1. Client (driver) writes a request structure at offset 0 of the mapping header.
2. Client signals the Request event.
3. Server reads the request code (`ULONGLONG` at offset 0).
4. Server processes based on request code.
5. Server writes response data at offset 0.
6. Server signals the Response event.

### 8.2 INFO

Request: `ULONGLONG request_code = IMDPROXY_REQ_INFO` (8 bytes at offset 0).
Response: `IMDPROXY_INFO_RESP` (24 bytes at offset 0). Data area untouched.

### 8.3 READ

Request: `IMDPROXY_READ_REQ` (24 bytes at offset 0). Server reads data from backend into data area.
Response: `IMDPROXY_READ_RESP` (16 bytes at offset 0). Data is in data area.

### 8.4 WRITE

Request: `IMDPROXY_WRITE_REQ` (24 bytes at offset 0). Data area contains bytes to write.
Response: `IMDPROXY_WRITE_RESP` (16 bytes at offset 0).

Server must copy data from shared memory BEFORE calling the backend, to prevent TOCTOU races.

### 8.5 CLOSE

Request: `IMDPROXY_CLOSE_REQ` (8 bytes at offset 0). No response signaled.

### 8.6 Unknown Codes

Any unrecognized request code is a fatal protocol error. M0 only implements INFO, READ, WRITE, CLOSE.

## 9. FileName Length Semantics

The `FileNameLength` field in `IMSCSI_DEVICE_CONFIGURATION` is the byte count of `FileName`, including the NUL terminator if present (common.h:242). For M0, the path is:

```
\BaseNamedObjects\Global\myrd-m0-<32-hex-chars>
```

Length of this path in WCHAR: `25 + 32 = 57` characters × 2 = 114 bytes + NUL = 116 bytes total.

## 10. SRB ReturnCode Semantics

- `SrbIoControl.ReturnCode`: Set by the driver. For most control codes, 0 = success, non-zero = driver error.
- **QUERY_VERSION special case**: `ReturnCode` = API compatibility version (`IMSCSI_DRIVER_VERSION` = 0x0101). The 4-byte payload after the SRB header is `SubVersion` (build-specific revision).
- M0 must preserve the raw ReturnCode value and distinguish it from Win32 API errors.
- Verified against AIM source: `srbioctl.cpp` line ~112-126 (upstream commit d379caa).

## 11. DeviceId Decoding

`DeviceId` is a `ULONG` (DEVICE_NUMBER.LongNumber).

| Byte | Bits | Field |
|---|---|---|
| 0 | 0-7 | PathId |
| 1 | 8-15 | TargetId |
| 2 | 16-23 | Lun |
| 3 | 24-31 | (reserved, typically 0) |

## Audit Transcript

```
$ rg -n 'SMP_IMSCSI_(QUERY_VERSION|CREATE_DEVICE|QUERY_DEVICE|QUERY_ADAPTER|REMOVE_DEVICE)' out/reference/aim
common.h:566-572 — all five control codes defined
NativeFileIO.cs:1174-1180 — managed-side duplicates

$ rg -n 'IMSCSI_(AUTO_DEVICE_NUMBER|ALL_DEVICES|TYPE_PROXY|PROXY_TYPE_SHM|DEVICE_TYPE_HD)' out/reference/aim
common.h:62,78,90,134,137 — all five constants defined

$ rg -n 'IMDPROXY_REQ_(INFO|READ|WRITE|CLOSE)' out/reference/aim
imdproxy.h:59-63 — all four request codes in enum

$ rg -n '_Request|_Response|_Server|BaseNamedObjects' out/reference/aim
proxy.cpp:440,472 — _Request, _Response suffixes in driver
DevioShmService.cs:134,136,138 — _Request, _Response, _Server in managed code
ScsiAdapter.cs:352,594 — \BaseNamedObjects\Global\ prefix
aimapi.cpp:1194,1198 — \BaseNamedObjects\ prefix

$ rg -n 'IMDPROXY_HEADER_SIZE' out/reference/aim
imdproxy.h:202 — #define IMDPROXY_HEADER_SIZE 4096
```
