# M0 V5 Fault-cleanup Evidence

Date: 2026-08-01 (Asia/Shanghai)

## Result

All four injected failures passed the expected-failure cleanup contract, then
one normal Debug lifecycle passed.

| Failure point | Summary | Result |
|---|---|---|
| `create_returned` | `reports/m0-v5-create_returned/summary-20260801-230001-3140488.json` | 1/1 PASS |
| `device_confirmed` | `reports/m0-v5-device_confirmed/summary-20260801-230002-3154044.json` | 1/1 PASS |
| `physical_drive_confirmed` | `reports/m0-v5-physical_drive_confirmed/summary-20260801-230003-6798038.json` | 1/1 PASS |
| `round_trip_verified` | `reports/m0-v5-round_trip_verified/summary-20260801-230005-1148224.json` | 1/1 PASS |
| normal post-check | `reports/m0-v5-normal/summary-20260801-230006-8287130.json` | 1/1 PASS |

Every injected report records `primary_error.operation=injected_failure` and a
nonzero child/report exit code. In every case:

- only the DeviceId returned by that CREATE was removed;
- the created DeviceId disappeared;
- provider stop and all named-object release succeeded;
- AIM DeviceId and PhysicalDrive sets exactly matched their baselines;
- cleanup errors were empty;
- the harness verified each immutable child-report hash after restoring
  `deviosvc`, and state and StartMode matched their original values.

Stage-specific checks also passed: early failure points performed no raw write,
while `round_trip_verified` contained the completed write/read digest evidence.

Verdict: V5 PASS.
