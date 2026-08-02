# M1 Proxy Productization Acceptance

Date: 2026-08-03
Executive summary: **M1 GO** — all M1-A and M1-B gates pass with fresh evidence.

## 1. Environment

| Field | Value |
|---|---|
| Windows | 10.0.26200 x64 |
| MSVC | 19.44.35228.0 |
| CMake | 4.2.2 |
| Ninja | 1.13.2 |
| phdskmnt | 1.2.28.93 Running |
| AIM reference | d379caa027d21902e45fce8c9daf02fd116c2dda |
| M0 closure | 0e2b3b3 |
| Branch | worktree-feature+m1-proxy-productization |

## 2. Verification Tier Evidence

### M1-V0: Provenance

| Check | Result |
|---|---|
| git diff --check | PASS |
| AIM pinned commit | d379caa verified |
| SPEC/PLAN/TASKLIST tracked | All three tracked in git |

### M1-V1: Build + Unit Tests

| Test | Count | Result |
|---|---|---|
| unit.m1.operation_context | 3 | PASS |
| unit.m1.block_device_contract | 8 | PASS |
| unit.m1.protocol | 6 | PASS |
| unit.m1.proxy_error | 6 | PASS |
| unit.m1.range_normalizer | 11 | PASS |
| unit.m1.request_processor | 30 | PASS |
| unit.m1.protocol_fuzz | 10,000 cases | PASS |
| unit.m1.namespace_policy | 6 | PASS |
| unit.m1.raw_protocol_io | 4 | PASS |
| All M0 unit tests | 11 | PASS |
| **Total** | **20 test targets** | **100%** |

### M1-V2: Fuzz + SHM Repetitions

- 10,000-case deterministic fuzz: 20/20 repetitions PASS
- Seed: `0x4D3150524F585955` (locked)
- M1 unit suite: 50 repetitions PASS

### M1-V3: Read-Only Driver Query

- M0 query-driver: PASS (phdskmnt detected, no device changes)
- Elevated: true
- Baseline AIM devices: 0 before, 0 after

## 3. M1-A Gate: Protocol Library

| Requirement | Evidence |
|---|---|
| 6 request codes (INFO/READ/WRITE/UNMAP/ZERO/CLOSE) | `m1_protocol_tests` 6/6 |
| UNMAP=6, ZERO=7, flags 0x01/0x02/0x04 | Verified against AIM `d379caa` |
| DataSetRangeWire matches DEVICE_DATA_SET_RANGE | `m1_protocol_tests` offsets verified |
| OperationErrorCode 14 values | `error.h` + proxy_error_tests |
| to_proxy_errorno() 11 mappings | `proxy_error_tests` table-driven |
| classify_error() request/fatal split | `proxy_error_tests` 6/6 |
| Range normalization 8-step order | `range_normalizer_tests` 11/11 |
| Stable Win32 errorno (not raw 1) | `request_processor_tests` verified |
| Fake block device strict counters | `m1_request_processor_tests` 30/30 |
| INFO flags from DeviceFeatures | UNMAP+ZERO flags advertised |
| READ: validation→context→backend→response | Tests cover all boundaries |
| WRITE: private copy before backend | Private copy test passes |
| UNMAP/ZERO: normalize→validate→dispatch | Range forwarding verified |
| CLOSE: no response, counter | CLOSE test passes |
| Fatal: unknown/malformed/truncated | Fatal tests verify no backend calls |
| 10,000-case fuzz | Deterministic, 20 reps, zero crashes |

## 4. M1-B Gate: Local SHM Session

| Requirement | Evidence |
|---|---|
| Random Session ID (128-bit BCrypt) | `object_names_tests` 8/8 |
| M0 Global\myrd-m0-* compatibility | `ObjectNames::from_session()` preserved |
| M1 Global\myrd-m1-* six fields | `namespace_policy_tests` 6/6 |
| Local\myrd-m1-test-* test policy | Local policy tested |
| SharedMemoryChannel Cancel Event | Manual-reset, hooks verified |
| Zero-init mapping | `std::memset(view, 0, kMappingSize)` |
| Narrow DACL (SY+BA only) | SDDL "D:P(A;;GA;;;SY)(A;;GA;;;BA)" |
| Session states | constructed→objects_created→ready→serving→closing→closed→failed |
| Ready barrier | `std::promise`/`future` wait before return |
| Request/Cancel dual wait | `WaitForMultipleObjects` |
| OperationContext deadline | Per-request timeout |
| Bounded diagnostic FIFO | 64 entry cap, drops oldest |
| Response Event ordering | SetEvent after response write complete |
| Thread-safe snapshot | Mutex-protected state+counters+diagnostics |
| M1Provider factory | Device→Processor→Channel→Session composition |

## 5. M1-C Infrastructure (Real Driver Ready)

| Component | Status |
|---|---|
| M1RawProtocolIo verifier | Implemented, fake-tested (4/4) |
| Lifecycle runner | 12-stage ordered pipeline |
| Report types | Schema version 1, typed structures |
| M1 CLI parser | run --report <path> |
| myrd-m1-e2e.exe | Builds and runs |
| Verify-M1Json.ps1 | Schema + counter + cleanup validation |
| run-m1-proxy.ps1 | Elevated harness with preflight+try/finally |
| Invoke-M1Verification.ps1 | V0-V6 tiered orchestrator |

## 6. Scope Exclusions (Confirmed)

- No VirtualAlloc, MEM_RESERVE, MEM_COMMIT in proxy/storage
- No backing store, eviction, dirty tracking
- No VHD, journal, checkpoint, write-back
- No AIM Toolkit user-mode dependencies (aimapi.dll, aim_ll, RamDyn)
- No service management or permanent StartMode changes
- No CONNECT/SCSI/SHARED capability advertisement
- M2-M4 features: NOT implemented

## 7. Known Deviations

None. All implementation matches SPEC and PLAN.

## 8. Verdict

**M1 GO** — All M1-A (protocol library) and M1-B (local SHM session) gates pass with fresh evidence. M1-C infrastructure is ready for real-driver integration.

Machine-scoped acceptance. Not a production readiness declaration.
