# M5 Lifecycle Task List

**Design:** docs/superpowers/specs/2026-09-30-m5-lifecycle-gui-design.md
**Plan:** docs/superpowers/plans/2026-09-30-m5-lifecycle-implementation.md
**Baseline:** cb3df08 (clean M4)
**Status values:** NOT_STARTED, IN_PROGRESS, BLOCKED, PASSED, FAILED.

| ID | Status | Depends on | Exit criterion |
| --- | --- | --- | --- |
| M5-00 | PASSED | — | Isolated worktree and clean M4 baseline recorded. |
| M5-01 | PASSED | M5-00 | Audited product/control/profile modules compile; malformed input fails closed. |
| M5-02 | NOT_STARTED | M5-01 | Ported operation/profile CAS authority and the ServiceCore contract; import lease covers registration. |
| M5-03 | IN_PROGRESS | M5-01 | M3 prepare/commit/abort; M4 one-shot checkpoint unchanged. Reopened by the 2026-10-02 solution: recovery-side `inspect_checkpoint` is Task 3 work. |
| M5-04 | PASSED | M5-03 | Foreground and worker share one DiskRuntime production adapter. |
| M5-05 | IN_PROGRESS | M5-02, M5-04 | Authenticated per-profile worker, private IPC and recovery proof. Per the 2026-10-02 solution this owns `service_worker_control`, `service_worker_session`, `windows_worker_observation.h` and `windows_worker_recovery.{h,cpp}`. |
| M5-06 | PASSED | M5-02, M5-05 | P4/P5 core, reconciliation, dirty-truth stop policies. Orchestration only: no worker launch, no pipe framing, no bundle/journal reads. See the status notes below for what this does and does not cover. |
| M5-07 | NOT_STARTED | M5-06 | Scheduler, public pipe, CLI, service lifecycle, auto-start. |
| M5-08 | NOT_STARTED | M5-07 | Two-profile isolation and service/worker restart suites pass. |
| M5-09 | NOT_STARTED | M5-08 | Clean local Debug/Release results and evidence tools. |
| M5-10 | NOT_STARTED | M5-09 | VMware M5-A through M5-D evidence and frozen release manifest. |
| M5-11 | NOT_STARTED | M5-10 | Optional qualified Win32 GUI thin client. |

## Mandatory gates

- [ ] Every file under `src/management/` and `src/cli/` is created by the task the
      plan's ownership table names, as a port or a rebuild; no file is "modified
      into existence".
- [ ] `service_worker_control` remains the only `IWorkerControl` implementation,
      and it drives DiskRuntime over `m5_worker_protocol` — no HostCore path.
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

## Status notes (2026-10-06)

**M5-06 is PASSED, and the claim is narrower than the word suggests.** Steps 2-4
of the plan -- implement P4, implement P5 and recovery, implement the normal stop
policy -- were *not written in this round*: the ported `service_core.cpp` already
contained all three, which is what the plan's own "edits the ported
`service_core.cpp` rather than creating it" anticipated. What this round added is
the verification the task asked for and the code never had: the four commit
dispositions against their durable phases, the epoch transient, the `resolving`
row, the restart row for a durable P5, both normal-stop policies, the recovery
command family (repair, rebind, forget), and the real checkpoint and stop paths
against a worker. Evidence is in transfer manifest §1.32–§1.37.

**Two things M5-06 cannot close, and they are not oversights.**

1. §3.6's row "原 worker 可重连" appears structurally impossible in M5:
   `m5_worker_main` leaves its loop the moment the channel closes, so a restart
   always finds the original worker gone. If that holds, the row should be struck
   from the table with the reason rather than implemented.
2. §3.6's row "原 worker 不存在 → 新 worker 只读 recovery proof" cannot execute
   until M5-07 supplies the live-worker registry: reaching a worker needs a
   session, `attach` returns `nullopt`, and `recover_bundle` is wired to
   adopt-only. M5-06 tested the recovery *decision*; the *execution* is M5-07's.

**Open findings carried out of M5-06**: §1.33, a withdrawn candidate leaves a
worker unable to prepare again -- the expectation is in M3's abort handling and
the mandatory gate forbids M5 from changing M3's on-disk formats; §1.31, a worker
teardown failure after a closed channel, observed once and never reproduced.
§1.34(a), M3's machine-wide device-removal wait, is **no longer open**: §1.38
found it was not a race at all but an unsatisfiable predicate, reproduced it
sequentially, and fixed it in M3 on this branch (§7 item 27) — with the harness
barrier that had been hiding it deleted rather than disabled.

**Rows above this note that predate the transfer manifest's evidence.** M5-02,
M5-03 and M5-05 carry statuses written before the increments they describe were
recorded, and M5-02 in particular reads `NOT_STARTED` while the manifest's §1.3
records Task 2's verification. They should be reconciled against the manifest
before acceptance, by whoever owns them, rather than assumed correct.
