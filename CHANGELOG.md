# Changelog

All notable changes to MyRamDisk are recorded here. The project follows
[Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

**This file is part of the release contract (R-07).** Every shipped version names the
commits its behaviour came from, so "what is in 1.0.0" can be answered without reading the
whole history. Dates are the dates the change landed on `feature/m5-lifecycle`.

## [1.0.0] - 2026-10-30 (release candidate)

First release. **Supported platform: Windows 11 only** (Win10 / Server are not supported).
**Unsigned**: there is no code-signing certificate, so Windows SmartScreen may warn; the
release ships SHA-256 checksums and these notes state the consequence. **No version
rollback**: reinstalling is the supported way back; the installer and the docs say so.

### Single-disk lifecycle (M5)

- One disk at a time: a second `disk create` / `disk start` is refused at admission with the
  stable code `0x5609` (`single_disk_violation`). (`2a1f125`, evidence
  `docs/validation/m5-stab-01-single-disk.md`)
- Create / start / stop / checkpoint / compact / forget, Load Only (`--no-start` + `disk
  start`), volatile-disk loss protection with `--accept-volatile-data-loss`, single-file
  VHDX export, and automatic recovery after a hard power cut.
- `disk show` reports the worker's own memory gauges (`mem=resident/budget`) and the
  durable compaction counters.

### Memory budget (M5 §7, decision 1)

- A fixed `--memory-hard-limit` above the machine's commit limit is **refused at create
  admission** with `0x560A` and the message "M5 does not over-commit"; nothing is left
  behind (no operation, reservation, bundle or worker). (`c8b3d63`, evidence
  `docs/validation/m5-stab-02b-memory-limits.md`)
- The store's `hard_limit_rejections` count travels to `disk show` as
  `budget_rejections=`, so a failed write can be attributed to the budget locally.
- The budget-exhaustion experiment is a repeatable gate step:
  `tools/m5vm/Invoke-M5FullSuite.ps1 -Only memory`.

### Stability work that landed with 1.0.0

- One-command full suite over the guest VM (`tools/m5vm/Invoke-M5FullSuite.ps1`): grouped,
  reset between groups, machine-readable `results.json`, two consecutive runs compared case
  by case. (`3a13308`, evidence `docs/validation/m5-stab-04-full-suite.md`)
- Recovery from a checkpoint interrupted between P4 (commit durable) and P5 (epoch
  published) no longer leaves the operation `running` forever: the epoch is published from
  the recovery inspection's proof. (`6393a89`, evidence
  `docs/validation/m5-stab-03c-process-kills.md`)

### Known limits in 1.0.0

- Volume labels are **not supported**: AIM volumes do not complete `SetVolumeLabel`; the
  entry point refuses and explains (decision 3).
- One disk at a time (G4 multi-disk is deferred).
- Physical RAM exhaustion is **not** a supported path: budgets above the machine's commit
  limit are refused at creation (decision 1).
- Upgrades are supported by reinstall only; there is no downgrade or rollback path in the
  code (decision 5).
- The driver (`phdskmnt`) is checked, never installed or replaced.

## Commit map

| Area | Commit |
|---|---|
| Single-disk admission guard | `2a1f125` |
| Memory budget enforced, >64 MiB disks creatable | `fcd7da1`, `1dbc476` |
| One-command full suite | `46308ec`, `caa92b3`, `369e38d`, `3a13308` |
| Six release decisions (docs) | `ae915d0` |
| Over-commit refusal + budget rejections (R-01) | `c8b3d63` |
| Deterministic P4 window (R-04 apparatus) | `b12b449` |
| P5 from the recovery inspection's proof | `6393a89` |

`[1.0.0]` is a release candidate until the R-19 rehearsal completes; the manifest and the
checksums are produced by `tools/New-M5ReleaseManifest.ps1` and checked by
`tools/Verify-M5Evidence.ps1`.
