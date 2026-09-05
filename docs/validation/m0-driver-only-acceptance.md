# M0 Driver-only Acceptance

## Verdict

**GO** for runtime Driver-only feasibility on this machine. Task29's final
fresh Debug/Release V2 runs and the full Release V6 run all passed.

## Requirement table

| SPEC section | Requirement | Evidence command/report | Result |
|---|---|---|---|
| 3 | No AIM user-mode runtime dependency | Release import audit; per-run loaded-module lists | PASS |
| 5 S1 | Do not uninstall Toolkit/driver | Environment ledger and harness behavior | PASS |
| 5 S2-S3 | Reject all-device sentinel; remove exact owned DeviceId | Unit tests and every V4-V6 report | PASS |
| 5 S4-S6 | No guessed disk; four-part identity; never write sector 0 | Locator/raw identity tests; offset 8 MiB reports | PASS |
| 5 S7 | Restore `deviosvc` state and StartMode | Every harness summary | PASS |
| 5 S8-S10 | Failure report, exact ownership, serialized real tests | Atomic publisher, failure reports, global mutex | PASS |
| 6 | Protocol pinned to audited AIM source | V0 and `aim-m0-contract.md` | PASS |
| 7-13 | Fixed geometry, SHM provider, direct SRB control, exact locator | V1/V2 plus V3-V6 reports | PASS |
| 14-15 | Full lifecycle and raw round trip | V4 report and strict validator | PASS |
| 16-17 | CLI/exit codes and strict JSON evidence | Unit tests, `Verify-M0Json.ps1`, atomic publisher | PASS |
| 18 V0 | Source/contract/diff checks | `Invoke-M0Verification -Tier V0` | PASS |
| 18 V1 | Build/unit/ABI | Debug and Release CTest | PASS |
| 18 V2 | 50 local SHM integrations | V2 command | PASS |
| 18 V3 | Read-only driver query with no state change | V3 reports | PASS |
| 18 V4 | One create/read/write/remove cycle | `m0-v4-single-cycle.md` | PASS |
| 18 V5 | Four cleanup fault injections | `m0-v5-fault-cleanup.md` | PASS |
| 18 V6 | 20 consecutive Release cycles | `m0-v6-20-cycle.md` and aggregate | PASS |
| 19 | Required negative scenarios | Unit suite and V5 | PASS |
| 20.1 | V0-V6 all pass | Tier evidence above | PASS |
| 20.2-20.3 | No `aimapi.dll` import or loaded AIM DLL | Audit and reports | PASS |
| 20.4-20.6 | 20/20 while deviosvc stopped; counters and hashes valid | V6 summary/aggregate | PASS |
| 20.7-20.10 | Exact DeviceId, restored sets, named objects gone, service restored | V6 aggregate | PASS |
| 20.11 | No unexplained failures or residue | Final query/service/object checks | PASS |

## Facts, derivations and conclusion

Observed: `phdskmnt.sys` 1.2.28.93 is Running/Automatic; the Release import audit
passes; V4 is 1/1; all V5 cases are 1/1; V6 is 20/20; every V6 report contains
matching SHA-256 values, exact DeviceId cleanup and zero fatal protocol errors.
The accepted aggregate is
`reports/m0-v6/summary-20260802-005337-3234647-aggregate.json`; its fixed Release
executable SHA-256 is
`CBC84333F1E64504EDC156A5FEA69C1CF90BC8BCE5C8C232EB642C113BD1AB85`.

Derived: because all run/session ids are unique, all final sets match, all named
objects fail to reopen, and the service baseline is restored, the repeated
success is not explained by stale objects or leaked devices.

Conclusion: runtime Driver-only feasibility is proven on this machine.

Runtime Driver-only feasibility is proven on this machine. Toolkit removal and
standalone driver installation remain unapproved until M5 disposable-VM
validation.

M0 does not prove persistence, dynamic allocation, image-file compatibility or
production readiness; those remain M1+ work.
