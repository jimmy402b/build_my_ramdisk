# M0 V3 Read-Only Driver Gate Evidence

> Date: 2026-08-01
> Implementation commit: bf89d16

## V3 Verification

| Check | Result |
|---|---|
| Executable | `out/build/msvc/src/Release/myrd-m0-poc.exe` |
| Executable SHA-256 | (see Release build output) |
| Import audit | PASS — no AIM user-mode DLL/EXE |
| Release non-real tests | 11/11 pass |
| query-driver exit code | 0 |
| JSON valid (ConvertFrom-Json) | PASS |
| JSON valid (Verify-M0Json.ps1) | PASS |

## Protocol Gate

| Field | Value |
|---|---|
| Adapter path | `\\?\root#scsiadapter#0000#{2accfe60-c130-11d2-b082-00a0c91efb8b}` |
| GUID_DEVICEINTERFACE_STORAGEPORT | `{0x2ACCFE60, 0xC130, 0x11D2, ...}` — verified against Windows SDK winioctl.h |
| SRB_IO_CONTROL layout | `ReturnCode@20, Length@24` — verified against Windows SDK ntddscsi.h |
| QUERY_VERSION API version | 257 (0x0101) — from SrbIoControl.ReturnCode |
| QUERY_VERSION SubVersion | 0x01021C5D — from payload |
| Service | phdskmnt |

## State Invariants

| Check | Before | After | Unchanged |
|---|---|---|---|
| phdskmnt service | Running/Automatic | Running/Automatic | ✅ |
| deviosvc service | Running/Manual | Running/Manual | ✅ |
| PhysicalDrive count | (baseline) | (matches) | ✅ |
| AIM device list | (baseline) | (matches) | ✅ |
| created_device | — | null | ✅ |
| raw_round_trip | — | null | ✅ |

## No state change confirmed

Query-driver did NOT:
- Create any RAM disk
- Delete any device
- Write to any PhysicalDrive
- Modify any service state
- Modify any service start type
- Load any forbidden module

## Regression tests added

1. `srb_layout_returncode_at_20_length_at_24` — offset verification
2. `query_version_response_byte_layout_returncode_is_api_version` — real byte layout
3. `storageport_guid_is_correct_from_sdk` — GUID verification
4. `adapter_query_version_reads_api_version_from_return_code` — api_version from ReturnCode
5. `adapter_query_version_return_code_and_subversion_are_distinct` — no confusion
6. `adapter_query_version_preserves_srb_return_code` — ReturnCode preserved
7. `adapter_query_version_truncated_payload_still_has_api_version` — robustness

## Report

- Path: `reports/m0-v3/query-v3-final.json`
- Mode: query-driver
- Success: true
- Exit code: 0
