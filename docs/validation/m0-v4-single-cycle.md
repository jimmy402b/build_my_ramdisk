# M0 V4 Single-cycle Evidence

Date: 2026-08-02 (Asia/Shanghai)

## Result

V4 passed: 1 attempted, 1 passed.

- Summary: `reports/m0-v4/summary-20260802-005326-7137299.json`
- Lifecycle report: `reports/m0-v4/run-20260802-005326-7137299-1.json`
- Query before/after are recorded by the summary.

## Observed values

| Evidence | Value |
|---|---|
| Run id | `1867955068cee93c03e05118a8086947` |
| Provider session | `e427b93f8ac94af651f8d387a2af5293` |
| Created/removed DeviceId | `0` / `0` |
| SCSI address | Path 0, Target 0, LUN 0 |
| Normalized QUERY flags | `0x00000310` (784) |
| PhysicalDrive | `\\.\PhysicalDrive1` |
| Disk length | 67,108,864 bytes |
| INFO/READ/WRITE/CLOSE | 1 / 2 / 1 / 1 |
| Raw offset/length | 8,388,608 / 4,096 bytes |
| Written/read SHA-256 | `74A3AB627B36412C63ABB13C905F739F5C1F7A3E876996A49391E0B464302FF6` |
| Flush | succeeded |
| Debug executable SHA-256 | `D23F0D47200F4E2A52712FC3BA50EC2F8278081D6122BEF48E2223D9C4172014` |
| Driver SHA-256 | `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9` |

The atomically published lifecycle report confirms QUERY_DEVICE identity,
four-part PhysicalDrive identity, flush success, matching digests, exact
removal, device disappearance, provider stop, all named-object release and
exact before/after list equality. The harness summary independently confirms
that the immutable report hash did not change and that `deviosvc` state and
StartMode were restored.

Before the lifecycle, the V4 gate also passed dedicated preflight tests proving
that an existing junction is rejected before any child directory is created and
that an abandoned global serial mutex is safely recovered and released.

## Defects fixed before acceptance

- CREATE FileName wire offset corrected from padded offset 36 to ABI offset 34.
- Provider server thread is started before CREATE.
- SRB returned length uses actual DeviceIoControl bytes.
- Adapter PnP rescan is performed after CREATE.
- Disk locator and raw identity handles request required read access.
- Windows' non-persistent read-only attribute on the newly owned disk is
  cleared only after exact identity checks.
- QUERY_DEVICE path parsing uses ABI offset 34 without dropping the last WCHAR.
- Raw write rechecks PhysicalDrive number, SCSI address and length on both
  read-only and read-write handles.
- Cleanup retries transient PnP enumeration errors until the complete
  PhysicalDrive baseline is observed again.
- Cleanup condition-waits for delayed kernel release of all four per-session
  named objects, while preserving a hard 15-second failure deadline.

Verdict: V4 PASS.
