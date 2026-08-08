# M3 final Task44 audit

## Scope and provenance

This audit closes Task44 without changing product algorithms, wire protocols,
file formats or persistence ordering. Product/test source is frozen at
`1ee1e350ced851f3b9702116dfb83ff01317d370`. Task41, Task42 and Task43 evidence
was generated at that same commit. Task44 may add a later documentation-only
sign-off commit; the final record must prove that no product, test, fixture,
tool or build-definition path changed between the evidence commit and that
sign-off commit.

The authoritative requirements are:

- `docs/superpowers/specs/2026-08-04-m3-vhdx-journal-checkpoint-design.md`;
- `docs/superpowers/plans/2026-08-04-m3-persistence-engine.md`;
- `M3_TASKLIST.md` and `M3_MICROTASKS.md`;
- Task43's same-commit design and execution plan.

## Frozen-decision traceability

| # | Frozen contract | Production implementation | Automated verification | Real/final evidence |
|---:|---|---|---|---|
| 1 | VHDX + append-only journal + dual-slot manifest | `src/persistence/vhdx_*`, `journal_*`, `manifest_*`, `recovery_engine.*` | `m3_vhdx_*`, `m3_journal_*`, `m3_manifest_*`, `m3_recovery*` tests | Task41 create/checkpoint/reopen/compaction; Task42 recovery matrix |
| 2 | Normal write acknowledges RAM; only a successful checkpoint promises power-loss recovery | `src/storage/dynamic_block_store_*`, `src/m3/m3_provider.cpp`, `src/persistence/checkpoint_coordinator.cpp` | M2 write/snapshot tests; M3 checkpoint and lifecycle tests | Task41 verifies durable generations; Task42 loses only post-checkpoint changes |
| 3 | Successful manifest flush is the known commit point; uncertain publication is `CommitIndeterminate` | `src/persistence/commit_resolver.cpp`, `checkpoint_coordinator.cpp`, `image_session.cpp` | `m3_commit_publish_tests.cpp`, `m3_checkpoint_tests.cpp`, `m3_image_session_tests.cpp` | Task42 cuts before/after slot write, manifest flush and readback |
| 4 | Checkpoint consumes an M2.1 whole-disk consistency cut | `src/storage/snapshot_coordinator.*`, `dynamic_block_store_snapshot.cpp`, `checkpoint_coordinator.cpp` | consistent-snapshot reference/model/concurrency tests; checkpoint integration | Task43 deterministic mixed-operation soak |
| 5 | First-overwrite COW; one active snapshot per disk | `src/storage/dynamic_block_store_snapshot.cpp`, `snapshot_coordinator.*` | `dynamic_block_store_consistent_snapshot_tests.cpp` and model/stress suites | Formal soak bounds COW, stalls and cleanup |
| 6 | Runtime lazy-loads VHDX/journal directly; it never online-attaches the base as a second product disk | `src/persistence/composite_backing_store.*`, `vhdx_reader.*`, `recovered_store_adapter.*` | composite, VHDX reader and lazy-backing tests | Task41 backing-read counters plus separate read-only native-VHDX check |
| 7 | M3 accepts only the product strict dynamic non-differencing VHDX profile | `src/persistence/vhdx_format.*`, `vhdx_writer.*` | strict malformed matrix, writer and native compatibility tests | Task41 Windows native read-only attach/detach |
| 8 | Journal index is immutable sparse COW and startup/lookup do not scan the whole journal | `src/persistence/journal_index.*`, `journal_reader.*`, `composite_backing_store.*` | journal index/builder/reader and cache-limit tests | Task41 bounded first-touch reads; Task43 zero startup reads |
| 9 | Checkpoint/compaction serialize while ordinary I/O remains available | `src/persistence/image_session.*`, `checkpoint_coordinator.*`, `compaction_coordinator.*` | image-session, checkpoint, compaction and concurrency tests | Task41 live NTFS workflow; Task43 mutation/stall thresholds |
| 10 | Compaction builds from an immutable durable view and publishes a successor generation | `src/persistence/live_extent_enumerator.*`, `compaction_coordinator.*` | live-extent and compaction integration tests | Task41 generation-3 compaction and native VHDX validation |
| 11 | Files referenced by either valid manifest slot, a live backing view or in-flight recovery/reconciliation cannot be deleted | `src/persistence/retention_manager.*`, `image_paths.*`, `image_session.*` | retention and persistence-path identity/race tests | Task41/42 cleanup ledgers have zero residuals |
| 12 | Corrupt data never silently becomes zero or older block data | `src/persistence/recovery_engine.*`, `journal_reader.*`, `composite_backing_store.*` | recovery oracle, corruption and journal-shadowing tests | Task42 full-corpus hashes, fallback=false in all scenarios |
| 13 | Post-commit failure enters resolution/reconciliation, not an ordinary failure | `src/persistence/commit_resolver.*`, `checkpoint_reconciler.*`, `image_session.*` | commit-publish phase runner and reconcile integration tests | Task42 manifest-flush/readback/reconcile interruption scenarios |
| 14 | Timer, service, GUI, startup and shutdown policy remain M4 | M3 exports management mechanisms only; `myrd-m3-aim-server` accepts explicit commands | CLI argument/route tests and dependency-boundary tests | Operator document explicitly excludes automatic policy |
| 15 | M3 GO includes real AIM+NTFS abnormal-recovery evidence | `src/m3/m3_provider.*`, `m3_aim_server_main.cpp`, real E2E harness | provider/windows acceptance and fail-closed verifier tests | Task41 PASS; Task42 10/10 real scenarios; Task43 Formal PASS |

## Cross-layer drift audit

| Boundary | Audit result | Evidence |
|---|---|---|
| M1 IMDPROXY wire | `git diff master..1ee1e35` is empty for `imdproxy_protocol.h`, request/channel implementation and `aim_protocol.h`; no M3 format or management type enters the frozen wire. | `tests/unit/protocol_tests.cpp`, `m1_protocol_tests.cpp`, `m1_protocol_fuzz_tests.cpp`; privileged provider regression |
| AIM contract | M3 calls the existing direct adapter/provider composition and does not load AIM user-mode DLLs or executables. Device removal remains exact-ID and cleanup-verified. | `src/driver/*`, `src/m3/m3_provider.cpp`, Task41/42 environment inventories |
| M2 block semantics | `block_device.h`, `backing_store.h`, M1 wire and AIM contract are byte-identical to M2 GO commit `a37ce865`; M2.1 adds management/snapshot APIs without changing the block I/O contract. | M2 and M2.1 cumulative tests; Task41 counters; Task43 soak |
| Durability order | Journal records/index/commit are written and flushed before inactive manifest-slot publication; successful manifest flush proves commit; readback/install/ack follow. | checkpoint/commit/reconcile tests and Task42 cuts |
| Error/disposition | Stable persistence error codes and four commit dispositions are encoded/decoded fail-closed; corruption is distinct from absence/zero. | `persistence_types.*`, `image_session.*`, report/verifier tests |
| Cleanup authority | Paths are handle-anchored and identity-validated; cleanup additionally requires retention proof and exact device ownership. | persistence-path, retention, lifecycle tests; real cleanup ledgers |
| M4 handoff | M3 exposes create/open/checkpoint/compact/inspect/verify/close mechanisms and metrics. It contains no timer/service/GUI policy and does not require M4. | frozen SPEC section 20; `docs/operations/m3-persistence.md` |

## Official gate map

Local CTest labels overlap and are not additive.

| Gate | Local registration at evidence freeze | Required external evidence | Task43 aggregate state |
|---|---:|---|---|
| M3-V1 format/codec/I/O | 13 tests | strict profile plus native artifact provenance | PASS |
| M3-V2 recovery/checkpoint/crash | 32 tests | Task42 simulated and real crash matrix | PASS |
| M3-V3 M2.1/lazy integration | 7 tests plus `m2.1-v0..v5` | Task41 lazy/NTFS and exact provider regression | PASS |
| M3-V4 compaction/retention/reporting | 8 tests | Task41 compaction and cleanup | PASS |
| M3-V5 Windows native VHDX | 7 tests | elevated native read-only attach/detach | PASS |
| M3-V6 real AIM+NTFS | 4 local prerequisites | Task41 RealFunctional and Task42 real recovery | PASS |
| M3-V7 soak/resource/performance | 4 tests | Task43 Formal, bound to Task41/42 | PASS |

## Evidence catalog

Evidence freeze commit:
`1ee1e350ced851f3b9702116dfb83ff01317d370`.

| Artifact | Result | SHA-256 / identity |
|---|---|---|
| Task41 RealFunctional report | PASS; create, NTFS, two checkpoints, reopen, lazy reads, compaction, native VHDX, cleanup | `0F92D894A98B8B20516E6817F164A12C60932D6E9214D0ABF719EBD55F7B9EDB` |
| Task42 simulated report | PASS; 10,000 schedules, 6/6 model tests; not sufficient alone | `A066D90AB308EB91000E6A5D71ED0D2F2C08F84D627D03A37E0F26E7C66F6A2F` |
| Task42 Full aggregate | PASS; 10/10 real scenarios, 8 process kills, 2 controlled power cuts | `82B7C7F1A4BC47D4845BF7CE56F2AA347EAF567A1E4CEED250780AC1C44225D5` |
| Task43 Formal | PASS; 100 seeds, 204,800 operations, no failed seed, thresholds/cleanup PASS | `10F7C74A5AA346E336CFA47F2B1E34DB097830111AB6B20A2B0837C11BBE7210` |
| Privileged exact-binary supplement | 6/6; combined with host 114/120 for complete 120/120 coverage | archive `2852F5FD282B9AA252D53CA2051D6845AC46F51CA7CA5F73645EC75FC3571804`; result `DB61B53918B1FDFE7CF14768848DE3AD3E67070DB0B5571AFF087B22613F987E` |

Production evidence binaries:

- `myrd-m3-aim-server.exe`:
  `5EC08F88613CBE195B5FB978E9F8B4A60479CB4585230A089684B7E7295011E7`;
- `myrd_m3_e2e.exe`:
  `DD8561EF4CDA320E23566BE1A838EED87BAA3DB7746B46A997D654B524AFDCBA`;
- `myrd_m3_soak_tests.exe`:
  `B398B10AAE7C03D9077F1A02CC823C559B5376B3D460C80D6A459E91A9C1F81A`;
- `myrd_m3_checkpoint_crash_model_tests.exe`:
  `7A2BD0B63B6107C6E26BE5D396E3146C2F86CF56E6603C7F1F4FCAA108950562`.

## License and redistribution audit

`THIRD_PARTY.md` records AIM/ImDisk protocol provenance, the separately
installed driver boundary, Microsoft specifications/system APIs, build/test
tools and project-authored fixtures. No tracked third-party binary exists. The
project does not claim authorization to redistribute `phdskmnt.sys` or AIM
Toolkit user-mode binaries. Fresh `dumpbin /dependents` audits of the Release
M3 engineering CLI and AIM server found none of the forbidden AIM user-mode
modules.

## Final Task44 gates

At the start of this audit, product evidence is PASS and Task41–43 are DONE.
Task44 final GO additionally requires:

1. a documentation-only diff audit against the evidence freeze;
2. fresh clean-tree Debug and Release builds/full CTest;
3. fresh M0/M1/M2/M2.1 cumulative regressions and documented M3 commands;
4. revalidation of required real evidence and exact hashes with no required
   `SKIP` or `BLOCKED` represented as PASS;
5. independent specification and quality review with P0/P1 equal to zero;
6. a final signed commit/branch record in `M3_TASKLIST.md`.

The milestone remains **BLOCKED pending completion of these Task44 gates**. This
sentence must be replaced by the final GO record only after fresh command output
has been inspected.
