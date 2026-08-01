# M0 V4 Single-cycle Evidence

Date: 2026-08-01 (Asia/Shanghai)

## Result

V4 passed: 1 attempted, 1 passed.

- Summary: `reports/m0-v4/summary-20260801-225959-6283944.json`
- Lifecycle report: `reports/m0-v4/run-20260801-225959-6283944-1.json`
- Query before/after are recorded by the summary.

## Observed values

| Evidence | Value |
|---|---|
| Run id | `f21c8835f5e12c6d8e8b829414f4a4c1` |
| Provider session | `664521034091cd4b675aca9090cca064` |
| Created/removed DeviceId | `0` / `0` |
| SCSI address | Path 0, Target 0, LUN 0 |
| Normalized QUERY flags | `0x00000310` (784) |
| PhysicalDrive | `\\.\PhysicalDrive1` |
| Disk length | 67,108,864 bytes |
| INFO/READ/WRITE/CLOSE | 1 / 2 / 1 / 1 |
| Raw offset/length | 8,388,608 / 4,096 bytes |
| Written/read SHA-256 | `2A69B199D3645AE1CDF2FED22A92FC574D072FA0BEBDAA0A64F6303B82A0C996` |
| Flush | succeeded |
| Debug executable SHA-256 | `91B2BF3BF078455025207116C9B6890206ECED79675621D0E6113C9475723817` |
| Driver SHA-256 | `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9` |

The atomically published lifecycle report confirms QUERY_DEVICE identity,
four-part PhysicalDrive identity, flush success, matching digests, exact
removal, device disappearance, provider stop, all named-object release and
exact before/after list equality. The harness summary independently confirms
that the immutable report hash did not change and that `deviosvc` state and
StartMode were restored.

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

Verdict: V4 PASS.
