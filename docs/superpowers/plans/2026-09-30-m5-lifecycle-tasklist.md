# M5 Lifecycle Task List

**Design:** docs/superpowers/specs/2026-09-30-m5-lifecycle-gui-design.md
**Plan:** docs/superpowers/plans/2026-09-30-m5-lifecycle-implementation.md
**Baseline:** cb3df08 (clean M4)
**Status values:** NOT_STARTED, IN_PROGRESS, BLOCKED, PASSED, FAILED.

| ID | Status | Depends on | Exit criterion |
| --- | --- | --- | --- |
| M5-00 | NOT_STARTED | — | Isolated worktree and clean M4 baseline recorded. |
| M5-01 | NOT_STARTED | M5-00 | Audited product/control/profile modules compile; malformed input fails closed. |
| M5-02 | NOT_STARTED | M5-01 | Operation/profile CAS authority; import lease covers registration. |
| M5-03 | NOT_STARTED | M5-01 | M3 prepare/commit/abort; M4 one-shot checkpoint unchanged. |
| M5-04 | NOT_STARTED | M5-03 | Foreground and worker share one DiskRuntime production adapter. |
| M5-05 | NOT_STARTED | M5-02, M5-04 | Authenticated per-profile worker and recovery proof. |
| M5-06 | NOT_STARTED | M5-02, M5-05 | P4/P5 core, reconciliation, dirty-truth stop policies. |
| M5-07 | NOT_STARTED | M5-06 | Scheduler, public pipe, CLI, service lifecycle, auto-start. |
| M5-08 | NOT_STARTED | M5-07 | Two-profile isolation and service/worker restart suites pass. |
| M5-09 | NOT_STARTED | M5-08 | Clean local Debug/Release results and evidence tools. |
| M5-10 | NOT_STARTED | M5-09 | VMware M5-A through M5-D evidence and frozen release manifest. |
| M5-11 | NOT_STARTED | M5-10 | Optional qualified Win32 GUI thin client. |

## Mandatory gates

- [ ] No task changes M3 bundle, journal, manifest, or profile on-disk formats.
- [ ] `feature/m5-supervisor-service` is read-only historical reference; no JSON profile, DiskSupervisor, HostCore, or old service composition is copied from it.
- [ ] No task installs, replaces, upgrades, or uninstalls phdskmnt; it remains an externally supplied prerequisite only.
- [ ] Every mutation is scheduler-owned; client timeout never cancels a safe running operation.
- [ ] Cross-session live epoch versus durable epoch comparison does not exist.
- [ ] Same-session P5 live epoch is checked against its P4 cut epoch.
- [ ] Import writer lease covers validation through registration publication.
- [ ] disk remove/forget without proof that the writer lock, matching device, and retained bundle identity are safe is refused; it retains both registration and bundle and never reports success.
- [ ] Worker replacement cannot trigger removal or synthetic checkpoint success.
- [ ] M5-A: single profile lifecycle.
- [ ] M5-B: dual-profile isolation.
- [ ] M5-C: service restart and auto-start recovery.
- [ ] M5-D: bounded preshutdown and VMware power-off recovery.
- [ ] M5-E: frozen hashes and M5 VM EVIDENCE PASS.
- [ ] GUI starts only after M5-E.
