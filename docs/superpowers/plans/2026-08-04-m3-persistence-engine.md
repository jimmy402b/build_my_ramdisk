# M3 Persistence Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `subagent-driven-development` (recommended) or `executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Execute exactly one numbered task at a time. Before each task, read the matching section in `M3_TASKLIST.md`; after each task, run every listed check, record evidence, and re-read this plan to prove the implementation has not drifted.

**Goal:** Add the M2.1 consistent-snapshot/COW prerequisite and build an M3 persistence engine that creates VHDX-backed image bundles, incrementally checkpoints dirty blocks into an append-only journal, recovers only complete generations, lazily loads blocks, compacts journals, and passes real AIM+NTFS crash-recovery acceptance.

**Architecture:** Preserve the accepted AIM kernel, M1 proxy contract, and M2 lazy block store. Extend only M2's management plane with a bounded consistent snapshot epoch, then add a separate `myrd_persistence` library for checked file I/O, manifest/journal/index/VHDX codecs, immutable composite backing views, recovery, checkpoint, reconciliation, and compaction. Product orchestration remains thin: M3 exposes manual operations and evidence; M4 later supplies scheduling, service lifecycle, volume flush policy, and GUI.

**Tech Stack:** C++20, MSVC, CMake/CTest, Win32 file and synchronization APIs, BCrypt SHA-256, project-owned CRC32C, Microsoft MS-VHDX as the normative format reference, Windows Virtual Disk API for independent compatibility checks, PowerShell for elevated E2E orchestration, and the repository's lightweight C++ test framework.

**Planning baseline:** `80d1ae3`, whose parent `a37ce86` is the accepted M2 GO commit.

---

## 0. Authority and execution contract

The authority order is:

1. `docs/superpowers/specs/2026-08-04-m3-vhdx-journal-checkpoint-design.md`
2. this implementation plan
3. `M3_TASKLIST.md`
4. existing accepted M0/M1/M2 contracts

If these documents conflict, stop the current task, preserve the failing evidence, and repair the lower-authority document before changing product code. An agent may not silently reinterpret the durability contract.

The implementation starts from the M2 GO source at `a37ce86`. The planning commit only adds documentation. Task 00 must prove the current branch contains that source and that the full Release regression is green.

### 0.1 Non-negotiable boundaries

- Do not modify the AIM driver or consume AIM Toolkit user-mode binaries.
- Do not add a new IMDPROXY opcode or a fake `IBlockDevice::flush()`.
- Do not make M1 depend on M2 or M3 implementation types.
- Keep `src/storage/` independent of image file formats and AIM lifecycle.
- Keep `src/persistence/` independent of AIM device identifiers and GUI/service code.
- Do not use existing `copy_dirty_batch()` for a durable filesystem checkpoint.
- Do not acknowledge persistence before installing the exact successor backing view.
- Do not silently map corruption to zero, an older record, or an older VHDX block.
- Do not allow a second bundle mutation during checkpoint, compaction, reconciliation, or commit resolution.
- Do not delete a file referenced by either valid manifest slot or a live `BackingView`.
- Do not claim M3 GO without the real AIM+NTFS gate.
- Do not memory-map an entire user image, read the full VHDX/journal at open, or allocate directly from an untrusted on-disk count.
- Keep fault injection test-only or behind an explicit test build/flag; production defaults to no injection.

### 0.2 Delegation model

Every task has one of three execution classes:

| Class | Meaning | Claude Code authority |
|---|---|---|
| **C — Claude-primary** | Mechanical foundation with a fixed contract | Claude Code may implement the complete task, but a separate reviewer must inspect the diff and rerun verification before merge. |
| **S — Split** | Useful scaffolding plus correctness-sensitive logic | Claude Code writes red tests, fakes, codec skeletons, and uncomplicated branches; the logic owner implements or line-reviews state transitions and publication order. |
| **O — Logic-owner** | Concurrency, linearization, recovery authority, or destructive lifecycle | Claude Code may prepare tests or diagnostics only. The logic owner writes/reviews the production algorithm and signs the drift audit. |

Claude Code must receive one task at a time. It must not implement “the rest of M3,” combine commits, weaken tests, edit evidence into passing form, or proceed to the next task without explicit handoff.

Recommended division:

- Claude-primary: build targets, public data types, CRC/streaming SHA, byte codec, file fakes, exact I/O wrappers, path validation, raw manifest/journal codecs, strict VHDX structural parser scaffolding, report schema, CLI parsing, PowerShell preflight.
- Split: snapshot tests and fakes, persistent index, journal reader/writer, VHDX BAT reader/writer, composite backing, image creation/recovery scaffolding, compaction extent enumeration, E2E harness.
- Logic-owner: mutation admission, COW publication, whole-disk snapshot proof, manifest candidate selection, checkpoint commit/indeterminate boundary, backing install/ack reconciliation, compaction publication, old-file deletion authority, crash verdict, final GO.

Independent testing rule: the agent that writes production recovery selection must not write the only recovery reference model. Golden wire fixtures and the crash-state oracle must not call production codecs.

## 1. Frozen contracts

### 1.1 M2.1 public contract

Create `src/storage/consistent_snapshot.h` with semantics equivalent to:

```cpp
namespace myrd {

struct ConsistentSnapshotToken {
    std::uint64_t snapshot_id{};
    BackingGeneration source_backing_generation{};
    std::uint64_t cutoff_dirty_epoch{};
};

enum class ConsistentSnapshotState : std::uint8_t {
    Active,
    Aborted,
    FullyScanned,
};

struct ConsistentSnapshotStatus {
    ConsistentSnapshotState state{};
    std::uint64_t cow_bytes{};
    std::uint64_t cow_records{};
    std::uint64_t blocked_writers{};
};

} // namespace myrd
```

Add these `DynamicBlockStore` methods:

```cpp
Result<ConsistentSnapshotToken> begin_consistent_snapshot(
    const OperationContext& context);

Result<std::vector<DirtyBlockSnapshot>>
copy_consistent_snapshot_batch(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    DirtyScanCursor& cursor,
    std::size_t max_bytes);

Result<void> finish_consistent_snapshot(
    const ConsistentSnapshotToken& token);

Result<void> abort_consistent_snapshot(
    const ConsistentSnapshotToken& token);

Result<ConsistentSnapshotStatus> consistent_snapshot_status(
    const ConsistentSnapshotToken& token) const;
```

The existing per-entry `dirty_epoch` and global `dirty_epoch_` are retained. Do not add a second epoch with overlapping meaning.

Extend `DynamicBlockStoreOptions` with:

```cpp
std::uint64_t snapshot_cow_max_bytes;
std::chrono::milliseconds snapshot_writer_stall_limit;
```

Validation requires capacity for one complete budgeted DATA pre-image (64 KiB payload plus its coordinator-owned record/container allocation) and a positive stall limit no greater than 5 seconds. The COW coordinator must account every dynamic record, index node and payload allocation through one budgeted allocator; ZERO/DISCARD records therefore also consume nonzero budget. Product/e2e defaults use 250 ms; tests pass explicit values.

Add stable M2.1 errors to `DynamicBlockStoreErrorCode`:

```cpp
SnapshotBusy,
SnapshotTokenInvalid,
SnapshotAborted,
SnapshotNotFullyScanned,
SnapshotResourceExhausted,
```

Add coherent/lifetime statistics for active snapshot id/state, COW current/peak bytes, COW records, blocked writers, writer stalls, automatic snapshot aborts, consistent batches, and consistent records. Existing M2 JSON schema tests must be updated fail-closed.

### 1.2 Mutation admission and lock order

Add a request-independent `MutationToken`. Public `write()`, `zero()`, and `unmap()` acquire it for their entire multi-block operation after acquiring the existing request lifetime token and before changing any block. Reads do not acquire it.

`begin_consistent_snapshot()` closes mutation admission, waits for `active_mutations_ == 0`, captures backing generation and `dirty_epoch_`, activates a token, then reopens admission. It must never hold all block stripes.

Required lock order:

```text
lifecycle/mutation-admission mutex
    -> block stripe (at most one)
        -> snapshot-coordinator mutex
            -> state_mutex_ only in existing short publication sections
```

The coordinator mutex must never acquire a block stripe. Any code path requiring reverse order must copy scalar state, release the coordinator lock, then retry under the permitted order.

While a consistent token is `Active` and not fully scanned, `install_backing_view()` and `mark_persisted()` return `SnapshotBusy`; otherwise they could remove/change a cut member without a pre-image. Once the token is `FullyScanned`, M3 may install the committed successor and perform exact conditional acknowledgments before `finish_consistent_snapshot()`, as required by the checkpoint order.

### 1.3 COW pre-image contract

For an active token with cutoff `E`, mutation of a dirty block with `entry.dirty_epoch <= E` must preserve the old logical value before publishing the mutation unless that block was already consumed by the snapshot reader.

```cpp
struct SnapshotPreimage {
    std::uint64_t block_index{};
    std::uint64_t version{};
    DirtyRecordKind kind{DirtyRecordKind::Data};
    BackingGeneration base_generation{};
    std::uint64_t dirty_epoch{};
    std::vector<std::byte> data;
};
```

- DATA owns exactly one 64 KiB payload.
- ZERO/DISCARD own no payload.
- One block has at most one pre-image per token.
- A newly dirty post-cut block is not part of the token.
- Scanning is strictly ascending. The coordinator owns one monotonic `next_scan_block` frontier; blocks below it are consumed/excluded and need no later pre-image. Do not allocate an unbounded consumed-block set.
- Budget exhaustion waits up to the configured stall limit, then atomically aborts the snapshot and allows the foreground mutation to continue.

### 1.4 M3 format and generation constants

Create `src/persistence/persistence_types.h` and freeze:

```cpp
inline constexpr std::size_t kManifestSlotBytes = 4096;
inline constexpr std::size_t kManifestFileBytes = 8192;
inline constexpr std::size_t kJournalAlignment = 4096;
inline constexpr std::size_t kIndexNodeBytes = 4096;
inline constexpr std::size_t kMaxRelativeNameBytes = 255;
inline constexpr std::uint32_t kM3FormatMajor = 1;
inline constexpr std::uint32_t kM3FormatMinor = 0;
inline constexpr std::uint32_t kVhdxDataBlockBytes = 2U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxIndexDepth = 8;
```

All project wire integers are little-endian. All fixed records have magic, version, length, zero reserved fields, and CRC32C. All offsets and sizes use checked arithmetic before allocation or I/O.

Generation rules:

- a new image starts at checkpoint/base/backing generation 0 and journal epoch 1;
- ordinary checkpoint publishes `G + 1` in the same journal epoch;
- compaction builds a base representing G, starts a new journal epoch, and publishes composite generation `G + 1`;
- `BackingView::generation == checkpoint_generation`;
- generation and epoch never wrap.

Define caller-supplied `PersistenceLimits` for maximum virtual bytes, journal committed bytes/records, index cache bytes/entries, BAT cache bytes/entries, I/O scratch bytes and verification batch bytes. Validation must guarantee every required single record/node/block fits, reject zero/overflow/impossible combinations, and derive tighter geometry bounds where possible. Library code has no hidden unbounded fallback; CLI/E2E pass explicit documented values.

### 1.5 Commit outcome

Create these stable outcome types:

```cpp
enum class VolumeBarrierKind : std::uint8_t {
    WindowsVolumeFlushed,
    RawBlockTestNoFilesystemCache,
};

enum class CommitDisposition : std::uint8_t {
    NotCommitted,
    CommitIndeterminate,
    Committed,
    CommittedNeedsReconciliation,
};

struct CheckpointOutcome {
    CommitDisposition disposition{CommitDisposition::NotCommitted};
    BackingGeneration generation{};
    bool manifest_candidate_verified{};
    bool backing_view_installed{};
    std::uint64_t acknowledged_blocks{};
    std::uint64_t still_dirty_blocks{};
    std::optional<Error> error;
};
```

Every checkpoint/compaction cut receives an explicit `VolumeBarrierKind`. Product/real-filesystem orchestration may pass `WindowsVolumeFlushed` only after a successful volume `FlushFileBuffers()` for the exact online disk/session and records that result in evidence. `RawBlockTestNoFilesystemCache` is accepted only by explicit raw/local-test entry points and must be visible in the outcome/report; it cannot produce a filesystem-durability claim. M3 core does not invent a flush opcode or silently infer this precondition.

The manifest slot write begins an uncancellable publish attempt. Successful manifest `FlushFileBuffers()` is the only known commit point. A slot-write or flush error after the attempt starts returns `CommitIndeterminate`; it must never be mapped to cancellation or `NotCommitted`.

### 1.6 Normative VHDX references

The VHDX implementation must be checked against Microsoft primary sources:

- MS-VHDX: `https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/`
- `OpenVirtualDisk`: `https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-openvirtualdisk`
- `CreateVirtualDisk`: `https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-createvirtualdisk`
- `AttachVirtualDisk`: `https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-attachvirtualdisk`
- `GetVirtualDiskPhysicalPath`: `https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-getvirtualdiskphysicalpath`

Do not copy format logic from an unlicensed implementation. Record the exact MS-VHDX revision/date used in `THIRD_PARTY.md` or the M3 validation document.

## 2. Target repository shape

### 2.1 Existing files modified for M2.1

| File | Responsibility of change |
|---|---|
| `src/storage/dynamic_block_types.h` | M2.1 options, errors, stats; retain existing `dirty_epoch`. |
| `src/storage/dynamic_block_store_internal.h` | `MutationToken`, coordinator ownership, deterministic test seams. |
| `src/storage/dynamic_block_store.h` | Public consistent snapshot API and private admission hooks. |
| `src/storage/dynamic_block_store_core.cpp` | Mutation admission, begin cut, close interaction, stats snapshot. |
| `src/storage/dynamic_block_store_write.cpp` | DATA pre-image before resident and load+mutate publication. |
| `src/storage/dynamic_block_store_read.cpp` | Copy-on-load mutation pre-image and gate integration. |
| `src/storage/dynamic_block_store_ranges.cpp` | ZERO/DISCARD pre-image before decommit/publication. |
| `src/storage/dynamic_block_store_snapshot.cpp` | Token-scoped consistent batch, finish, abort, status. |
| `src/storage/dynamic_block_store_backing.cpp` | Prevent backing publication during the brief cut and preserve ack behavior. |
| `src/m2/m2_report.*` | New stats/schema fields without claiming persistence. |

### 2.2 New M2.1 files

| File | Responsibility |
|---|---|
| `src/storage/consistent_snapshot.h` | Public token/status types only. |
| `src/storage/snapshot_coordinator.h` | Internal coordinator API, pre-image metadata, lock-order contract. |
| `src/storage/snapshot_coordinator.cpp` | Bounded COW table, state machine, wait/abort/consume. |
| `tests/unit/dynamic_block_store_consistent_snapshot_tests.cpp` | Deterministic cut/COW/token/resource tests. |
| `tests/unit/dynamic_block_store_consistent_snapshot_model_tests.cpp` | Independent whole-disk point-in-time model. |

### 2.3 New persistence library files

| File | One responsibility |
|---|---|
| `src/common/crc32c.h/.cpp` | CRC32C Castagnoli one-shot and streaming API. |
| `src/common/sha256_stream.h/.cpp` | Bounded streaming BCrypt SHA-256; existing one-shot API delegates to it. |
| `src/persistence/persistence_types.h/.cpp` | Constants, UUID/file identities, errors, outcomes, geometry checks. |
| `src/persistence/byte_codec.h` | Checked little-endian fixed-buffer reads/writes; no file I/O. |
| `src/persistence/persistence_file.h/.cpp` | Exact offset read/write, length, truncate, flush, file identity. |
| `src/persistence/persistence_path.h/.cpp` | Relative-name validation, canonical containment, bundle lock. |
| `src/persistence/fault_injector.h` | Production-noop/test-injected named durability boundaries. |
| `src/persistence/manifest_format.h/.cpp` | 4096-byte slot encode/decode/CRC. |
| `src/persistence/manifest_selector.h/.cpp` | Full candidate validation and highest-valid selection. |
| `src/persistence/journal_format.h/.cpp` | Header/record/commit wire format and digest input. |
| `src/persistence/journal_index.h/.cpp` | Immutable 4096-byte B+tree read and COW path rewrite. |
| `src/persistence/journal_reader.h/.cpp` | Committed-root lookup and payload validation. |
| `src/persistence/journal_writer.h/.cpp` | Append, index update, commit, flush, complete readback. |
| `src/persistence/vhdx_format.h/.cpp` | Strict profile structures/checksums/metadata validation. |
| `src/persistence/vhdx_reader.h/.cpp` | BAT lookup, sparse zero detection, range/extent enumeration. |
| `src/persistence/vhdx_writer.h/.cpp` | Dynamic sparse VHDX create, extent write, finalize. |
| `src/persistence/composite_backing_store.h/.cpp` | Journal-first immutable `IBackingStore`. |
| `src/persistence/image_session.h/.cpp` | Bundle handles, writer ownership, session/commit states. |
| `src/persistence/image_creator.h/.cpp` | Generation-0 create through normal recovery validation. |
| `src/persistence/recovery_engine.h/.cpp` | Manifest recovery and immutable `BackingView` construction. |
| `src/persistence/checkpoint_coordinator.h/.cpp` | Snapshot-to-journal pipeline, publish, install and conditional ack. |
| `src/persistence/commit_resolver.h/.cpp` | Resolve indeterminate manifest publication without guessing. |
| `src/persistence/live_extent_enumerator.h/.cpp` | Bounded immutable-view extent stream for compaction. |
| `src/persistence/compaction_coordinator.h/.cpp` | VHDX merge, new snapshot/journal commit, publish and reconciliation. |
| `src/persistence/retention_manager.h/.cpp` | Slot/view identity accounting and safe obsolete-file cleanup. |
| `src/persistence/image_inspector.h/.cpp` | Read-only bounded inspect and fast/full verification. |

### 2.4 M3 application and evidence files

| File | One responsibility |
|---|---|
| `src/m3/m3_report.h/.cpp` | Stable JSON schema and evidence serialization. |
| `src/m3/m3_evidence_publisher.h/.cpp` | Unique, flushed, atomic evidence publication without proxy dependency. |
| `src/m3/m3_cli.h/.cpp` | Parse create/inspect/verify/checkpoint/compact/recover-test/fault-matrix. |
| `src/m3/m3_lifecycle_runner.h/.cpp` | Manual operation orchestration and exact cleanup. |
| `src/m3/m3_provider.h/.cpp` | Compose the accepted M1 provider with M2 store and M3 session. |
| `src/m3/m3_windows_acceptance.h/.cpp` | Real driver, volume flush, native VHDX validation adapters. |
| `src/m3/main.cpp` | Thin executable entry point. |
| `tests/support/fake_persistence_file.h` | Sparse in-memory file, short I/O, flush and kill-boundary injection. |
| `tests/support/m3_reference_model.h/.cpp` | Independent generation oracle; no production codec includes. |
| `tests/e2e/M3Harness.Common.ps1` | Safe disk ownership and evidence helpers. |
| `tests/e2e/Invoke-M3NtfsWorkload.ps1` | Format-once dataset, volume flush, hash, trim/zero workload. |
| `tests/e2e/run-m3-persistence.ps1` | Elevated end-to-end orchestration. |
| `tools/Invoke-M3Verification.ps1` | Local/real verification tiers. |
| `tools/Verify-M3Json.ps1` | Fail-closed report validation. |
| `docs/validation/m3-persistence-acceptance.md` | Commands, evidence and final verdict. |

### 2.5 Build targets

- Extend `myrd_common` with CRC32C/streaming SHA.
- Add `myrd_persistence`, linking only `myrd_storage`, `myrd_common`, `bcrypt`, and Win32 libraries required by file identity.
- Add `myrd_m3`, `myrd_m3_report`, and `myrd_m3_windows`; only the Windows acceptance adapter links `virtdisk`, driver, and product lifecycle libraries.
- Add `myrd-m3-e2e`.
- Add focused test executables instead of one enormous test source: M2.1 snapshot, format, journal index, VHDX, backing/recovery, checkpoint, compaction, M3 report/CLI, and harness preflight.

## 3. Verification tiers

| Tier | Stable command/label | Purpose |
|---|---|---|
| M3-V0 | `ctest --preset release --output-on-failure` before changes | Freeze 57-test M2 baseline. |
| M3-V1 | `ctest --preset release -L m3-v1 --output-on-failure` | Formats, codecs, checked I/O and fuzz. |
| M3-V2 | `ctest --preset release -L m3-v2 --output-on-failure` | Checkpoint/recovery model and crash matrix. |
| M3-V3 | `ctest --preset release -L m3-v3 --output-on-failure` | M2.1 cut/COW/generation/concurrency. |
| M3-V4 | `ctest --preset release -L m3-v4 --output-on-failure` | Compaction, rotation, retention. |
| M3-V5 | `ctest --preset release -L m3-v5 --output-on-failure` | Native Windows VHDX compatibility. |
| M3-V6 | `tools/Invoke-M3Verification.ps1 -Tier RealFunctional` | Real AIM+NTFS recovery without reformat. |
| M3-V7 | `tools/Invoke-M3Verification.ps1 -Tier Soak` | Resource, lazy-load, performance, kill/restart soak. |

Every task runs its focused target, `git diff --check`, and the cumulative local tier named in `M3_TASKLIST.md`. Tasks changing M2 run all labels `m2-v1` through `m2-v5`; tasks changing common/proxy integration run the complete Release suite.

No task may mark a test passed from an old log. Evidence records commit, binary SHA-256, command, exit code, test count, machine, administrator state, driver version, and timestamp.

## 4. Implementation tasks

### Task 00: Freeze M2 GO and create the persistence build boundary

**Class:** C — Claude-primary
**Depends on:** none
**Files:** Create `src/persistence/persistence_types.h`, `src/persistence/persistence_types.cpp`, `tests/unit/persistence_boundary_tests.cpp`, `docs/validation/m3-persistence-acceptance.md`; modify `src/CMakeLists.txt`, `tests/CMakeLists.txt`.

- [ ] **Step 1: Prove the baseline.** Run `cmake --preset msvc`, `cmake --build --preset release`, then `ctest --preset release --output-on-failure`. Expected: configure/build exit 0 and 57/57 tests pass. Record `a37ce86` ancestry and current status.
- [ ] **Step 2: Write the structural red test.** Add a CMake/test assertion that `myrd_persistence` links `myrd_storage` and `myrd_common` but not `myrd_driver`, `myrd_proxy`, `myrd_m1`, or `myrd_m2`. Expected before target creation: configure FAIL naming missing `myrd_persistence`.
- [ ] **Step 3: Add the minimal boundary.** Start the public header with real version constants, not an empty marker:

```cpp
namespace myrd::persistence {
inline constexpr std::uint32_t kFormatMajor = 1;
inline constexpr std::uint32_t kFormatMinor = 0;
}
```

- [ ] **Step 4: Register `myrd_persistence` and `unit.m3.boundary`.** Apply existing warning/Unicode/runtime conventions. Do not link `virtdisk` into the core persistence library. Reserve CTest labels `m3-v0` through `m3-v5` and internal `m2.1-v0` through `m2.1-v5`; M3-V6/V7 remain verification-wrapper tiers and are not faked as local CTest gates.
- [ ] **Step 5: Verify.** Run `cmake --build --preset release --target myrd_persistence`, `ctest --preset release -R '^unit\.m3\.boundary$' --output-on-failure`, then the full Release suite. Expected: all exit 0.
- [ ] **Step 6: Commit.** Stage only listed files and commit `build(m3): establish persistence library boundary`.

### Task 01: Freeze M2.1 public types, options, errors and statistics

**Class:** C — Claude-primary with logic-owner API review
**Depends on:** 00
**Files:** Create `src/storage/consistent_snapshot.h`, `tests/unit/dynamic_block_store_consistent_snapshot_tests.cpp`; modify `src/storage/dynamic_block_types.h`, `src/storage/dynamic_block_store.h`, `src/m2/m2_report.h`, `src/m2/m2_report.cpp`, `tests/unit/m2_report_tests.cpp`, CMake files.

- [ ] **Step 1: Add compile-time red tests** for the exact token fields, state enum, status fields, option validation, stable error strings, and stats JSON keys. Example:

```cpp
static_assert(std::is_trivially_copyable_v<myrd::ConsistentSnapshotToken>);
REQUIRE_EQ(myrd::to_string(
    myrd::DynamicBlockStoreErrorCode::SnapshotBusy), "SnapshotBusy");
```

- [ ] **Step 2: Run the focused tests.** Expected: compile FAIL because the new public types/options do not exist.
- [ ] **Step 3: Add the types and declarations** exactly as Plan §1.1. Extend option validation so `snapshot_cow_max_bytes` can hold one exact allocator-accounted DATA record plus its 64 KiB payload and `0ms < stall <= 5000ms`; update every existing test fixture to pass explicit values. Do not add an arbitrary block-alignment rule to this byte budget.
- [ ] **Step 4: Add stats serialization/schema.** Existing report verification must reject a report missing any new required M2.1 field.
- [ ] **Step 5: Verify.** Build `myrd_storage_tests`, `myrd_m2_report_tests`, and the new snapshot test; run their CTest names and all `m2-v1` tests.
- [ ] **Step 6: Commit.** `feat(storage): define consistent snapshot contracts`.

### Task 02: Add mutation admission and a bounded consistency cut

**Class:** O — Logic-owner; Claude Code may write gates/tests only
**Depends on:** 01
**Files:** Modify `src/storage/dynamic_block_store.h`, `src/storage/dynamic_block_store_internal.h`, `src/storage/dynamic_block_store_core.cpp`, `src/storage/dynamic_block_store_write.cpp`, `src/storage/dynamic_block_store_ranges.cpp`, snapshot tests and concurrency tests.

- [ ] **Step 1: Write deterministic red gates** proving: a mutation admitted before the cut must finish before `begin_consistent_snapshot()` returns; a mutation arriving after the gate closes waits; reads continue; close wakes every waiter.
- [ ] **Step 2: Run `unit.m3.m2_consistent_snapshot`.** Expected: FAIL because begin/gating is absent.
- [ ] **Step 3: Add `MutationToken` and admission state:**

```cpp
struct MutationToken { DynamicBlockStore* store{}; bool active{}; };
bool mutation_gate_closed_{};
std::uint64_t active_mutations_{};
std::uint64_t mutation_waiters_{};
```

Implement cancellation/deadline-aware `enter_mutation()` and noexcept `leave_mutation()`. Hold a token for the complete public write/zero/unmap request, not one block segment.
- [ ] **Step 4: Implement the short cut.** Close admission, drain `active_mutations_`, capture `backing_view_.generation` and `dirty_epoch_`, activate one token, reopen admission, notify waiters. Roll back all state on cancellation/deadline/close.
- [ ] **Step 5: Audit lock order.** Add comments and a deterministic test that would deadlock if coordinator/state/stripe ordering is reversed; use finite deadlines.
- [ ] **Step 6: Verify.** Run the focused test 20 times, `unit.m2.concurrency`, `unit.m2.write`, `unit.m2.ranges`, and all `m2-v3`.
- [ ] **Step 7: Commit.** `feat(storage): establish consistent mutation cut`.

### Task 03: Implement the bounded snapshot coordinator and metadata-only COW

**Class:** S — Claude writes red tests/class shell; logic-owner implements transitions
**Depends on:** 02
**Files:** Create `src/storage/snapshot_coordinator.h`, `src/storage/snapshot_coordinator.cpp`; modify store internals and snapshot tests.

- [ ] **Step 1: Write red tests** for one active token, invalid token ownership, duplicate begin/finish/abort, ZERO/DISCARD pre-image insertion, monotonic-frontier consumed suppression, and exact COW counters.
- [ ] **Step 2: Run focused test.** Expected: FAIL on missing coordinator behavior.
- [ ] **Step 3: Define the internal API without exposing file concepts:**

```cpp
Result<void> preserve_metadata_before_mutation(
    const OperationContext&, const ConsistentSnapshotToken&,
    const SnapshotPreimage& metadata);
std::optional<SnapshotPreimage> peek_preimage(std::uint64_t block_index) const;
```

The real implementation owns a mutex, condition variable, token state, bounded pre-image map, monotonic `next_scan_block`, current/peak bytes, and abort reason. Every COW map/payload allocation must use its budgeted allocator; no metadata-only path may bypass the hard byte limit. There is no per-consumed-block dynamic set.
- [ ] **Step 4: Implement state transitions.** No coordinator method may acquire a block stripe. Token ids and all counters use checked monotonic increment.
- [ ] **Step 5: Verify invariants.** Run focused tests, `validate_invariants_for_test()`, and Thread Sanitizer-equivalent deterministic race gates available in the project; repeat 100 times.
- [ ] **Step 6: Commit.** `feat(storage): add bounded snapshot coordinator`.

### Task 04: Preserve DATA pre-images on every write publication path

**Class:** O — Logic-owner
**Depends on:** 03
**Files:** Modify `src/storage/dynamic_block_store_write.cpp`, `src/storage/dynamic_block_store_read.cpp`, `src/storage/snapshot_coordinator.*`, snapshot and write tests.

- [ ] **Step 1: Add red cases** for ResidentDirty→ResidentDirty, dirty block partial write, full write, write after the block was consumed, and write to a clean/post-cut block. Capture payload hashes before and after.
- [ ] **Step 2: Add red cases** for backing-unresident load+mutate and hard-limit read-through mutation. Expected snapshot payload is the pre-cut logical block, not the new payload or predecessor backing after a newer dirty value.
- [ ] **Step 3: Run focused tests.** Expected: snapshot returns current/new bytes, demonstrating the bug.
- [ ] **Step 4: Integrate preservation immediately before mutation publication:**

```cpp
if (snapshot_needs_preimage(block_index, *entry)) {
    MYRD_TRY(preserve_data_before_mutation(
        context, block_index, *entry, arena_block_span));
}
// Only now publish bytes/version/dirty_epoch.
```

For load+mutate, preserve the old dirty logical value only when it existed at the cut; a clean backing block at the cut is excluded.
- [ ] **Step 5: Prove rollback.** Allocation failure, cancellation, and snapshot abort must leave either the old mutation state or a completed foreground write; never a changed block with missing required pre-image.
- [ ] **Step 6: Verify.** Run snapshot/write/read/limit/concurrency suites and `m2-v1`, `m2-v3`.
- [ ] **Step 7: Commit.** `feat(storage): preserve data at consistent snapshot cut`.

### Task 05: Preserve ZERO/DISCARD transitions and partial-range mutations

**Class:** O — Logic-owner
**Depends on:** 04
**Files:** Modify `src/storage/dynamic_block_store_ranges.cpp`, `src/storage/dynamic_block_store_read.cpp`, `src/storage/dynamic_block_store_write.cpp`, snapshot/range tests.

- [ ] **Step 1: Add the full transition red matrix:** DATA→ZERO, DATA→DISCARD, ZERO→DATA, DISCARD→DATA, ZERO→DISCARD, partial zero, partial unmap normalized through DATA.
- [ ] **Step 2: Run focused tests.** Expected: at least one post-cut transition replaces the old kind/value in the snapshot.
- [ ] **Step 3: Preserve metadata before full-block decommit** and preserve DATA before any partial range path mutates resident bytes. A ZERO/DISCARD pre-image has an empty payload and exact old kind/version/base generation.
- [ ] **Step 4: Verify no resurrection.** After snapshot, install a fake successor, ack exact records, evict/reload, and assert ZERO/DISCARD never falls through to predecessor data.
- [ ] **Step 5: Run `unit.m2.ranges`, snapshot tests, backing-generation tests, and `m2-v3`.**
- [ ] **Step 6: Commit.** `feat(storage): snapshot zero and discard preimages`.

### Task 06: Implement token-scoped batch copy, complete scan, finish and abort

**Class:** O — Logic-owner
**Depends on:** 05
**Files:** Modify `src/storage/dynamic_block_store_snapshot.cpp`, `snapshot_coordinator.*`, public header and tests.

- [ ] **Step 1: Write red tests** for ascending cursor/frontier agreement, COW-first lookup, unchanged dirty lookup, post-cut dirty exclusion, exact-once records, last short block zero padding, pre-first-record cancellation, partial-success cancellation after ownership begins, and fully-scanned detection. `install_backing_view()`/`mark_persisted()` fail `SnapshotBusy` before FullyScanned and retain their existing semantics after FullyScanned.
- [ ] **Step 2: Run focused tests.** Expected: missing API or fuzzy snapshot mismatch.
- [ ] **Step 3: Implement batch selection:**

```cpp
if (auto old = coordinator.peek_preimage(block_index)) {
    append_snapshot_record(*old);
} else if (entry->is_dirty() && entry->dirty_epoch <= token.cutoff_dirty_epoch) {
    copy_current_dirty_under_existing_pin_rules();
}
```

The supplied cursor must equal the token's `next_scan_block`; arbitrary rewind/skip is rejected. Use the existing sparse directory/dirty metadata to jump absent leaves and clean ranges while preserving ascending logical order; do not probe every logical block. Build a bounded staging batch without deleting COW entries. Once its return object owns every selected payload, atomically erase covered COW entries, advance both caller cursor and coordinator frontier (including proven-empty skipped ranges), and wake budget waiters. If cancellation/deadline occurs before any record ownership, return error with no frontier change; once ownership/progress begins, return the safe partial batch and surface cancellation on the next call. Mark `FullyScanned` only at `total_blocks()`.
- [ ] **Step 3a: Add a sparse-scan complexity test** for a 1 TiB geometry with one dirty block. Directory/entry visits must be proportional to populated leaves plus returned records, not total logical blocks; scanning must not create leaves.
- [ ] **Step 4: Implement finish/abort/status.** Finish rejects incomplete scans; abort is safe after any pre-commit failure; duplicate or foreign tokens fail without mutating the active token.
- [ ] **Step 5: Verify.** Run new tests 100 times, original `unit.m2.snapshot`, concurrency, model, and all `m2-v3`.
- [ ] **Step 6: Commit.** `feat(storage): expose point-in-time snapshot batches`.

### Task 07: Bound COW pressure, writer stalls, close and resource cleanup

**Class:** O — Logic-owner
**Depends on:** 06
**Files:** Modify coordinator/core/write/range files, test access seams, stats/report tests.

- [ ] **Step 1: Add deterministic red tests** with a one-block COW budget: first pre-image fills it, second writer blocks, batch consumption releases it, and the writer resumes. Add the no-progress 250 ms abort case.
- [ ] **Step 2: Add close/cancel/deadline red tests** at every wait: admission drain, COW budget, snapshot batch transient block, finish, and abort.
- [ ] **Step 3: Implement bounded wait.** At stall deadline atomically mark token Aborted, notify all snapshot/writer waiters, let the foreground mutation continue, and make the checkpoint reader observe `SnapshotResourceExhausted`.
- [ ] **Step 3a: Never wait for COW capacity while holding a block stripe.** A mutation first attempts a coordinator reservation while its stripe protects the candidate value. If capacity is unavailable, it releases the stripe, waits, reacquires the stripe and revalidates token/frontier/pre-image/entry identity before retrying. A successful reservation publishes a per-block Preparing marker before copy; the snapshot reader may wait for that marker without acquiring the stripe. Cancellation/allocation failure rolls the reservation back and wakes readers. Add the cursor-at-blocked-block/one-record-budget regression that deadlocks if this protocol is violated.
- [ ] **Step 4: Extend close notification** to coordinator and mutation admission. Destruction requires zero active token, zero COW bytes, zero blocked writers, zero pins, and zero waiters.
- [ ] **Step 5: Verify counters and report schema.** Gauges return to zero; lifetime abort/stall counters increase exactly once.
- [ ] **Step 6: Run focused tests, `unit.m2.concurrency`, `unit.m2.stress`, all `m2-v1` and `m2-v3`.**
- [ ] **Step 7: Commit.** `fix(storage): bound consistent snapshot pressure`.

### Task 08: Build an independent whole-disk snapshot reference model

**Class:** S — Claude-primary for the test model; production snapshot code is read-only to this task
**Depends on:** 07
**Files:** Create `tests/unit/dynamic_block_store_consistent_snapshot_model_tests.cpp`, `tests/support/consistent_snapshot_model.h`; modify test CMake only.

- [ ] **Step 1: Implement a simple map model** storing logical block value/kind/version at a chosen cut. It must not include `snapshot_coordinator.h` or call production batch helpers.
- [ ] **Step 2: Generate deterministic schedules** of 2–64 blocks and 1,000–100,000 operations with writes/zero/discard/begin/copy batches/finish. Save the random seed on failure.
- [ ] **Step 3: Assert exact equality** between the union of returned records and the model cut, including absence of post-cut clean→dirty blocks.
- [ ] **Step 4: Run one deliberately broken build or test seam** that disables pre-image preservation; expected: the model test fails. Restore production behavior.
- [ ] **Step 5: Run Debug and Release model tests with fixed seeds plus 100 generated seeds.**
- [ ] **Step 6: Commit.** `test(storage): model consistent snapshot generations`.

### Task 09: Issue the M2.1 prerequisite verdict

**Class:** O — Logic-owner/reviewer
**Depends on:** 08
**Files:** Modify `docs/validation/m3-persistence-acceptance.md`, `M3_TASKLIST.md`; product code only for defects discovered by fresh verification.

- [ ] **Step 1: Run a clean Release build** and complete CTest suite.
- [ ] **Step 2: Run `m3-v3` twice**, all existing `m2-v1` through `m2-v5`, and the model with at least 100 fresh seeds.
- [ ] **Step 3: Run a bounded concurrency soak** combining writes/zero/unmap/batch copy/abort/close; verify no leaks or invariant errors.
- [ ] **Step 4: Review the M2.1 diff line-by-line** against Plan §1.1–1.3 and the SPEC consistency section. Confirm no format/M3 dependency entered `src/storage/`.
- [ ] **Step 5: Record `M2.1 PASS` or a specific failure.** Do not call this M3 GO and do not proceed to checkpoint integration if it fails.
- [ ] **Step 6: Commit evidence/docs.** `docs(m3): accept consistent snapshot prerequisite`.

### Task 10: Add CRC32C, streaming SHA-256 and checked byte codec

**Class:** C — Claude-primary
**Depends on:** 00; may run in parallel with the M2.1 task wave on a separate worktree
**Files:** Create `src/common/crc32c.h/.cpp`, `src/common/sha256_stream.h/.cpp`, `src/persistence/byte_codec.h`, `tests/unit/m3_checksum_codec_tests.cpp`; modify `src/common/sha256.cpp`, CMake.

- [ ] **Step 1: Write red vectors.** CRC32C of ASCII `123456789` must equal `0xE3069283`; one-shot and byte-at-a-time updates match; SHA-256 streaming of split input matches existing one-shot uppercase hex.
- [ ] **Step 2: Add byte-codec red tests** for little-endian u16/u32/u64, UUID bytes, bounded UTF-8 names, zero reserved bytes, short source/destination, and checked offset advance.
- [ ] **Step 3: Run focused tests.** Expected: compile FAIL on missing APIs.
- [ ] **Step 4: Implement table-driven or hardware-independent CRC32C** with deterministic output; no runtime CPU feature assumption. Implement an RAII BCrypt streaming hash whose finalize cannot be called twice.
- [ ] **Step 5: Implement fixed-span codec helpers** returning typed errors rather than throwing or advancing on failure.
- [ ] **Step 6: Verify Debug/Release focused tests and `unit.common`; commit `feat(common): add persistence checksums and byte codec`.**

### Task 11: Build exact random-access file I/O and a sparse fake

**Class:** C — Claude-primary
**Depends on:** 10
**Files:** Create `src/persistence/persistence_file.h/.cpp`, `src/persistence/fault_injector.h`, `tests/support/fake_persistence_file.h`, `tests/unit/m3_persistence_file_tests.cpp`; modify CMake.

- [ ] **Step 1: Define a narrow interface:**

```cpp
class IPersistenceFile {
public:
    virtual Result<void> read_exact(std::uint64_t, std::span<std::byte>) = 0;
    virtual Result<void> write_exact(std::uint64_t, std::span<const std::byte>) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<std::uint64_t> length() const = 0;
    virtual Result<void> truncate(std::uint64_t) = 0;
    virtual ~IPersistenceFile() = default;
};
```

- [ ] **Step 2: Red-test** short read/write at every byte, offset overflow, EOF, disk-full code mapping, access denied, flush failure, truncate failure, and zero-length policy.
- [ ] **Step 3: Implement `WindowsPersistenceFile`** with positioned `OVERLAPPED` I/O or explicit synchronized file pointer; loop until exact completion and preserve native error codes. No silent short I/O.
- [ ] **Step 4: Implement sparse fake storage** whose logical length need not allocate all intervening bytes. Add nth-call and named-boundary injection.
- [ ] **Step 5: Prove a multi-terabyte sparse fake opens and reads one range with bounded calls/memory** and no whole-file mapping/allocation. Fault injection is production-noop and only enabled by explicit test construction/build flags.
- [ ] **Step 6: Verify focused tests under Debug/Release and handle-count before/after 10,000 opens; commit `feat(persistence): add exact file io abstraction`.**

### Task 12: Enforce bundle path containment, file identity and writer ownership

**Class:** C — Claude-primary; destructive cleanup reviewed separately
**Depends on:** 11
**Files:** Create `src/persistence/persistence_path.h/.cpp`, `tests/unit/m3_persistence_path_tests.cpp`; extend persistence file identity support.

- [ ] **Step 1: Red-test** empty/over-255/invalid UTF-8 names, absolute/drive/UNC paths, `..`, separators, ADS colon, trailing device names, case variants, symlink/junction escape, and canonical sibling-prefix tricks.
- [ ] **Step 2: Red-test writer lock.** A second writable session fails `ImageInUse`; read-only inspect follows the declared sharing policy; process exit releases the lock.
- [ ] **Step 3: Implement canonical containment** by opening the bundle directory and target with reparse-safe flags, comparing normalized final paths and platform file IDs. File names are still constrained to one relative component.
- [ ] **Step 4: Add immutable identity types** for journal UUID and VHDX header GUIDs plus optional Windows file ID. Identity mismatch is not treated as a missing file.
- [ ] **Step 5: Verify without deleting user files.** All reparse tests use a dedicated temporary directory with exact resolved targets.
- [ ] **Step 6: Commit.** `feat(persistence): secure bundle paths and ownership`.

### Task 13: Define persistence errors, geometry, identities and outcomes

**Class:** C — Claude-primary with API review
**Depends on:** 10, 11, 12
**Files:** Expand `src/persistence/persistence_types.h/.cpp`; create `tests/unit/m3_persistence_types_tests.cpp`; modify `src/common/error.h` only if a new `persistence` domain is chosen consistently.

- [ ] **Step 1: Red-test every constant and stable error string** from the SPEC: invalid/unsupported/corrupt/in-use/busy/generation/snapshot/disk/access/I/O/cancel/deadline/verification/indeterminate/fatal.
- [ ] **Step 2: Red-test geometry validation** for 512/4096 logical sectors, physical 4096, 64 KiB M2 blocks, 2 MiB VHDX blocks, exact divisibility, zero and overflow.
- [ ] **Step 2a: Red-test caller-supplied `PersistenceLimits`** for zero, overflow, a required record/node/block not fitting, cache entry/byte disagreement, geometry-derived record limits and no hidden unlimited value.
- [ ] **Step 3: Implement `CommitDisposition` and `CheckpointOutcome`** exactly as Plan §1.5. Add checked generation/epoch increment returning fatal error on wrap.
- [ ] **Step 3a: Implement `VolumeBarrierKind` and checkpoint request validation.** Unknown/missing barrier fails before snapshot begin; raw-test mode is explicit and propagates to reports without a filesystem durability claim.
- [ ] **Step 4: Add phase and session enums** for Opening, Ready, Checkpointing, Compacting, ResolvingCommit, Reconciling, Closing, Closed, PersistenceFaulted.
- [ ] **Step 5: Require every public persistence error/outcome context** to carry operation, phase, native Win32 code when applicable, image UUID, operation id, generation and commit disposition; red-test safe defaults and redaction.
- [ ] **Step 6: Verify exhaustive `to_string()` switch tests and ABI-independent serialization tests; commit `feat(persistence): define stable persistence contracts`.**

### Task 14: Encode and decode one 4096-byte manifest slot

**Class:** C — Claude-primary
**Depends on:** 13
**Files:** Create `src/persistence/manifest_format.h/.cpp`, `tests/unit/m3_manifest_format_tests.cpp`, `tests/fixtures/m3/manifest-v1.hex`.

- [ ] **Step 1: Write a golden-byte red test** for one exact slot containing all SPEC fields. The fixture is generated by an independent hand-coded test builder, not production encode.
- [ ] **Step 2: Add corruption red tests** for every fixed field, CRC byte, reserved byte, invalid UTF-8 name, length, geometry, file identity, generation relation, root/commit range, and unknown major/mandatory flag.
- [ ] **Step 3: Implement fixed-offset encode/decode** into exactly 4096 bytes. Zero the destination first; calculate CRC with the CRC field zero; reject trailing semantic data in reserved space.
- [ ] **Step 4: Round-trip randomized valid values** and truncate the slot at every byte boundary.
- [ ] **Step 5: Verify `m3-v1` format tests and commit `feat(persistence): add manifest slot codec`.**

### Task 15: Select the highest fully valid dual-slot candidate

**Class:** O — Logic-owner; reference oracle by a different executor
**Depends on:** 14, 20, 22 (tests may scaffold earlier; final implementation waits)
**Files:** Create `src/persistence/manifest_selector.h/.cpp`, `tests/support/m3_reference_model.h/.cpp`, `tests/unit/m3_manifest_selector_tests.cpp`.

- [x] **Step 1: Have the independent executor build candidate fixtures** representing A/B valid/invalid, equal generation with conflicting identity, higher slot with bad VHDX/journal/root/commit, path escape, and predecessor fallback.
- [x] **Step 2: Run red tests.** Expected: selector missing.
- [x] **Step 3: Implement selection as full validation, not CRC sorting:**

```cpp
for (candidate : candidates_sorted_high_to_low) {
    if (validate_manifest(candidate) &&
        validate_vhdx_reference(candidate) &&
        validate_journal_commit_and_root(candidate)) {
        return candidate;
    }
}
return ManifestCorrupt;
```

- [x] **Step 4: Reject equal-generation split-brain slots** unless every durable identity/root/commit field is byte-equivalent. Record fallback reason.
- [x] **Step 5: Verify the selector against the independent oracle and fault fixtures; commit `ea02e0a` (`feat(persistence): select complete manifest generations`).**

### Task 16: Implement journal header, records, commit and chain-digest codec

**Class:** C — Claude-primary for wire codec; digest review by logic owner
**Depends on:** 13
**Files:** Create `src/persistence/journal_format.h/.cpp`, `tests/unit/m3_journal_format_tests.cpp`, journal golden fixtures.

- [ ] **Step 1: Red-test exact header and DATA/ZERO/DISCARD/INDEX_NODE/CHECKPOINT_COMMIT bytes**, 4096 alignment, zero padding, full 64 KiB DATA, final logical block padding, and record CRC.
- [ ] **Step 2: Red-test canonical digest** by hashing one checkpoint record-by-record and comparing an independent fixed SHA-256 fixture. Mutating padding, order, previous digest, generation, root or payload changes the digest.
- [ ] **Step 3: Implement strict decode** with unknown mandatory record rejection, length/alignment/overflow checks, UUID/epoch/generation checks, and no allocation before payload length validation.
- [ ] **Step 4: Implement genesis and cross-epoch predecessor digest handling** exactly as the SPEC.
- [ ] **Step 5: Fuzz/truncate every record type and commit `feat(persistence): add journal wire format`.**

### Task 17: Read immutable persistent B+tree nodes

**Class:** S — Claude can implement parser/cache; logic owner reviews corruption handling
**Depends on:** 11, 16
**Files:** Create `src/persistence/journal_index.h/.cpp`, `tests/unit/m3_journal_index_tests.cpp`.

- [ ] **Step 1: Red-test** empty root, one leaf, multi-level tree, boundary keys, absent keys, ZERO/DISCARD descriptors, corrupt node CRC, unsorted/duplicate keys, cycles, out-of-range child offsets, depth 9, committed-length escape, and cache eviction.
- [ ] **Step 2: Implement immutable lookup** iteratively with maximum depth 8 and a visited-offset set. Every node read is exact and inside committed length.
- [ ] **Step 3: Implement a bounded node cache** keyed by file identity/epoch/root generation/node offset. Never share nodes across a different immutable view identity.
- [ ] **Step 4: Run one million random lookups against `std::map` reference data and all corruption cases.**
- [ ] **Step 5: Commit.** `feat(persistence): read immutable journal index`.

### Task 18: Build copy-on-write index paths for a checkpoint

**Class:** S — Claude writes builder tests/shell; logic owner reviews path-copy algorithm
**Depends on:** 17
**Files:** Modify `journal_index.*`; create `tests/unit/m3_journal_index_builder_tests.cpp`.

- [ ] **Step 1: Red-test ordered update batches** for insert, replace DATA with ZERO, replace ZERO with DATA, split root/leaf/internal nodes, unchanged subtree reuse, and deterministic byte output independent of input batch ordering.
- [ ] **Step 2: Red-test duplicate block updates in one checkpoint.** Builder must reject them; the snapshot contract should already prevent them.
- [ ] **Step 3: Implement pure planning first:** produce new immutable node images and child references without writing files. Only affected root paths are copied.
- [ ] **Step 4: Add append-offset assignment** with checked arithmetic and then encode nodes. Failure before append leaves the old root untouched.
- [ ] **Step 5: Compare every new root lookup with a `std::map` model over at least 100,000 updates.**
- [ ] **Step 6: Commit.** `feat(persistence): build copy-on-write journal indexes`.

### Task 19: Append and read back a complete journal checkpoint

**Class:** S — Claude implements exact append mechanics; logic owner reviews durability order
**Depends on:** 11, 16, 18
**Files:** Create `src/persistence/journal_writer.h/.cpp`, `tests/unit/m3_journal_writer_tests.cpp`.

- [x] **Step 1: Red-test named phases** for each data record, node, commit, flush, and readback. Inject short write, disk full, cancellation and deadline before every phase.
- [x] **Step 2: Implement append from manifest committed length.** Never overwrite committed bytes; pad every record with zeros; accumulate the canonical streaming digest.
- [x] **Step 3: Append COW index nodes and commit**, flush exactly once after the complete commit, then re-read every newly appended byte and independently recalculate record CRC, node CRC, digest and committed end.
- [x] **Step 4: Return a `PreparedCheckpoint`** containing generation, root/commit descriptors, committed length, digest, record counts and an ack-record stream descriptor. It is not yet published.
- [x] **Step 5: Prove any pre-manifest failure leaves only an ignorable tail and old root. Run `m3-v1` and writer fault tests.**
- [x] **Step 6: Commit.** `feat(persistence): prepare durable journal checkpoints` (`5e0fa901b54c2692316b0e205382cc0fd61d91de`).

### Task 20: Resolve committed blocks with `JournalReader`

**Class:** S — Claude-primary implementation; corruption policy reviewed by logic owner
**Depends on:** 17, 19
**Files:** Create `src/persistence/journal_reader.h/.cpp`, `tests/unit/m3_journal_reader_tests.cpp`.

- [x] **Step 1: Red-test DATA, ZERO, DISCARD and absent results** from an immutable root. DATA must validate header/descriptor/payload CRC and exact block index/generation.
- [x] **Step 2: Red-test corruption** where the index descriptor is valid but payload/record/type/index mismatches. Expected: `BackingDataCorrupt`, never Absent.
- [x] **Step 3: Implement bounded exact reads** and `BlockLoadKind` translation. Reads never cross manifest committed length or use a later physical file tail.
- [x] **Step 4: Add cancellation/deadline and concurrent immutable-reader lifetime tests.**
- [x] **Step 5: Run journal/index/format tests and commit `feat(persistence): read committed journal blocks` (`de107626c410ff361c7ae9fdb34e501bd85d63bd`).**

### Task 21: Parse the supported VHDX profile strictly

**Class:** C — Claude-primary; malformed-input review by logic owner
**Depends on:** 10, 11, 13
**Files:** Create `src/persistence/vhdx_format.h/.cpp`, `tests/unit/m3_vhdx_format_tests.cpp`, and minimal golden fixtures under `tests/fixtures/m3/vhdx/`.

- [x] **Step 1: Write golden-byte red tests** for both file identifiers, both headers, both region tables, metadata table entries, file parameters, virtual disk size, logical/physical sector sizes, and BAT geometry.
- [x] **Step 2: Red-test malformed input:** bad signatures/checksums/GUIDs, conflicting header sequence numbers, duplicate/missing regions or metadata, integer overflow, overlap, invalid alignment, unsupported differencing/compression flags, invalid block/sector sizes, and truncated files at every structural boundary.
- [x] **Step 3: Implement checked little-endian parsing** for the frozen dynamic, non-differencing VHDX subset only. Select the highest valid header and highest valid region table copy independently.
- [x] **Step 4: Expose immutable `VhdxLayout`** with virtual size, sector sizes, block size, BAT offset/count, metadata ranges and file identity; do not expose writable pointers or silently accept unsupported features.
- [x] **Step 5: Cross-check fixtures with an independent fixture builder and commit `feat(persistence): parse strict vhdx profile`.**

### Task 22: Resolve VHDX BAT entries lazily

**Class:** S — Claude can implement exact reads/cache; logic owner owns BAT-state semantics
**Depends on:** 21
**Files:** Create `src/persistence/vhdx_reader.h/.cpp`, `tests/unit/m3_vhdx_reader_tests.cpp`.

- [x] **Step 1: Red-test `read_block()`** for fully present, not present, undefined, zero and unmapped payload states; boundary blocks; final partial block; 512/4096 logical sectors; and a 2 MiB product block split into 64 KiB store blocks.
- [x] **Step 2: Red-test rejected BAT states** including partially present, invalid state bits, physical overlap, payload outside file, payload before data region, arithmetic overflow and file truncation after open.
- [x] **Step 3: Implement on-demand BAT lookup** with a bounded page cache keyed by immutable base identity. Read payload only when requested; absent/zero blocks return zero without allocating a full-image buffer.
- [x] **Step 4: Enumerate allocated payload extents** without reading payload data; return sorted, unique, bounds-checked virtual ranges for compaction.
- [x] **Step 5: Compare random sparse images with an independent map oracle and commit `feat(persistence): read sparse vhdx blocks lazily`.**

### Task 23: Write a valid sparse dynamic VHDX base

**Class:** S — Claude writes structural mechanics; logic owner reviews publish/flush and sparse semantics
**Depends on:** 11, 21, 22
**Files:** Create `src/persistence/vhdx_writer.h/.cpp`, `tests/unit/m3_vhdx_writer_tests.cpp`.

- [x] **Step 1: Red-test empty and sparse images** at minimum geometry, large logical geometry, final partial block, all-zero allocated candidates, and non-contiguous payload blocks.
- [x] **Step 2: Implement deterministic layout planning** before file mutation. Use checked arithmetic; allocate metadata/BAT first; place payloads on required boundaries; skip all-zero blocks; zero all reserved bytes.
- [x] **Step 3: Write to a same-directory temporary file**, flush, close, reopen through `VhdxReader`, verify every structural checksum and sampled payload, then atomically rename to the final unique filename. Never overwrite a manifest-referenced base.
- [x] **Step 4: Inject failure at create, extend, metadata, BAT, payload, flush, close, reopen and rename. Assert no partially written final filename is returned. The dynamic VHDX container is an ordinary file; Windows rejects the NTFS sparse attribute.**
- [x] **Step 5: Commit.** `feat(persistence): write sparse dynamic vhdx bases`.

### Task 24: Prove Windows accepts product-written VHDX files

**Class:** C — Claude-primary harness; requires administrator/native Windows test host
**Depends on:** 23
**Files:** Create `tests/integration/m3_windows_vhdx_compat_tests.cpp`, `tests/e2e/test-m3-vhdx-compat.ps1`; modify CMake test registration.

- [x] **Step 1: Add a capability probe** for `OpenVirtualDisk`, `AttachVirtualDisk` and `GetVirtualDiskPhysicalPath`; unsupported/non-admin environments skip with a machine-readable reason, not pass silently.
- [x] **Step 2: Generate empty, sparse, boundary and 512/4096-sector fixtures** using product code, open each with Windows read-only/no-drive-letter flags, query geometry, attach, sample known data and detach in finally-style cleanup.
- [x] **Step 3: Open one independently Windows-created compatible VHDX** through `VhdxReader` and compare geometry/data. Keep only a documented generator script if redistributing the binary fixture is inappropriate.
- [x] **Step 4: Assert product files are never attached while the AIM-exported RAM disk is online; record exact Windows error codes and cleanup outcomes.**
- [x] **Step 5: Commit.** `31a12c7` `test(m3): add native vhdx compatibility gate`.

### Task 25: Compose journal-first and VHDX-fallback block resolution

**Class:** O — Logic-owner; Claude may prepare tests and adapters only
**Depends on:** 20, 22
**Files:** Create `src/persistence/composite_backing_store.h/.cpp`, `tests/unit/m3_composite_backing_store_tests.cpp`.

- [x] **Step 1: Have a separate test author build a truth table** for journal DATA/ZERO/DISCARD/Absent over VHDX data/zero/absent. DATA wins; ZERO and DISCARD suppress older VHDX data; only journal Absent falls through.
- [x] **Step 2: Red-test immutable view isolation:** an old reader remains bound to its old base/journal/root while a new generation is published. Cache keys must include complete backing identity.
- [x] **Step 3: Implement one-block resolution** with cancellation/deadline propagation and stable corruption provenance. Never interpret corruption as Absent or retry from an older layer.
- [x] **Step 4: Prove lazy behavior** with read counters: opening performs bounded metadata/index reads; reading one uncached 64 KiB block reads only the relevant index/BAT/payload ranges.
- [x] **Step 5: Commit.** `6594907` `feat(persistence): compose immutable backing layers`.

### Task 26: Recover the highest complete generation into a `BackingView`

**Class:** O — Logic-owner; independent oracle/fixture creation by another executor
**Depends on:** 15, 25
**Files:** Create `src/persistence/recovery_engine.h/.cpp`, `tests/integration/m3_recovery_tests.cpp`.

- [x] **Step 1: Build red fixtures** for valid A only, valid B only, both valid, newer torn/corrupt with valid predecessor, both invalid, missing/replaced files, truncated committed tail, valid ignorable tail, split-brain and unsupported version.
- [x] **Step 2: Implement bounded candidate validation** using the manifest selector. A candidate is selectable only after base identity/layout, journal identity/header, commit, digest, root and committed length all validate.
- [x] **Step 3: Construct one immutable `BackingView`** from the selected candidate and open handles. Do not mutate files, truncate tails, repair slots or acknowledge dirty data during recovery.
- [x] **Step 4: Return structured fallback diagnostics** naming each rejected slot and first stable reason without leaking absolute paths by default.
- [x] **Step 5: Compare selection/read results with the independent reference oracle and commit `1bddc2b`, with acceptance fixes through `e619a65`.**

### Task 27: Create and reopen generation zero

**Class:** S — Claude scaffolds creation; logic owner reviews first durable publication
**Depends on:** 12, 14, 16, 19, 23, 25, 26
**Files:** Create `src/persistence/image_creator.h/.cpp`, `tests/integration/m3_image_create_tests.cpp`;
extend `image_paths.*`/`writer_lock.*` only for the atomic `CREATE_NEW` manifest claim.

- [x] **Step 1: Red-test image creation** for valid geometry, existing destination, path escape, low disk space, every injected I/O failure, and concurrent creators.
- [x] **Step 2: Create a unique empty base VHDX and journal genesis**, flush/read back both, create an exact 8192-byte manifest with the inactive slot zeroed/invalid, then publish one manifest slot as generation zero and flush/read back the full slot.
- [x] **Step 3: Reopen only through the Task 26 production recovery path** and read boundary blocks as zero. Creation succeeds only if recovery independently selects generation zero.
- [x] **Step 4: Assert retries never overwrite a published image** and incomplete unreferenced artifacts are diagnosable and safely removable.
- [x] **Step 5: Commits.** `9803ef3`, `2fdc67c`, `5eb3caf`, `4efee3a`, `1d8f886`.

### Task 28: Integrate recovered backing with lazy block loading

**Class:** O — Logic-owner; Claude may add instrumentation and test fixtures
**Depends on:** 09, 27
**Files:** Modify `src/storage/dynamic_block_store*.cpp`, `src/storage/backing_store.h`, persistence integration adapters; create `tests/integration/m3_lazy_backing_tests.cpp`.

- [x] **Step 1: Red-test install/open and first-touch** with one recovered `BackingView`: no eager payload reads, one concurrent load per block, readers share the result, and a writer arriving during load preserves mutation ordering.
- [x] **Step 2: Red-test ZERO/DISCARD/Absent translation**, corruption propagation, cancellation, deadline, retry policy and close while a backing read is in flight.
- [x] **Step 3: Install only complete immutable views** through the existing `install_backing_view()` lifetime boundary. Old views remain alive until every in-flight load releases them.
- [x] **Step 4: Exercise eviction/reload** so clean blocks reload from the exact selected generation while dirty/snapshot-protected blocks are not evicted incorrectly.
- [x] **Step 5: Run M2 regression tests plus new integration tests and commit `e503dcd` (`feat(m3): load recovered blocks on first access`).**

### Task 29: Enforce image-session state and structured outcomes

**Class:** O — Logic-owner; Claude can implement table-driven transition tests
**Depends on:** 13, 27, 28
**Files:** Create `src/persistence/image_session.h/.cpp`, `tests/unit/m3_image_session_tests.cpp`.

- [x] **Step 1: Red-test every allowed and forbidden state transition** among Opening, Ready, Checkpointing, Compacting, ResolvingCommit, Reconciling, Closing, Closed and PersistenceFaulted.
- [x] **Step 2: Implement a serialized operation gate** for publish-affecting operations while allowing immutable backing reads. Reject concurrent checkpoint/compaction/close with stable busy/closing errors.
- [x] **Step 3: Preserve operation-scoped `CommitDisposition` and first failure** in every public result. `CommitIndeterminate` transitions to ResolvingCommit and blocks new mutations/checkpoints until resolved. Explicit publication stages make abandoned leases fail closed.
- [x] **Step 4: Prove close is idempotent**, drains operations in documented order, invokes stop callbacks outside the session lock, observes cancel/deadline, and never reports durable success based only on an in-memory state update.
- [x] **Step 5: Commits.** `551c1c9` (`feat(persistence): enforce image session state`) and `a039b09` (`fix(persistence): harden image session boundaries`).

### Task 30: Prepare a checkpoint through the pre-manifest durability boundary

**Class:** O — Logic-owner; Claude may implement phase fakes and red tests
**Depends on:** 09, 19, 29
**Files:** Create `src/persistence/checkpoint_coordinator.h/.cpp`, `tests/integration/m3_checkpoint_tests.cpp`.

- [x] **Step 1: Red-test preconditions and ordered phases:** an unknown/missing volume barrier fails before snapshot begin; then begin stable snapshot, stream each token member once, append records/index/commit, flush journal, read back, and become Prepared. Manifest and dirty acknowledgment must not occur yet.
- [x] **Step 2: Cover DATA/ZERO/DISCARD**, overwritten-after-cut blocks, clean image, partial final block, cancellation/deadline and every injected pre-manifest failure.
- [x] **Step 3: Implement bounded streaming** from `copy_snapshot_batch(token)` to `JournalWriter`; do not materialize a disk-sized snapshot or retain copied block payloads after durable append.
- [x] **Step 4: On pre-manifest failure**, abort snapshot, preserve old manifest/view and return `NotCommitted`; appended bytes are an ignorable tail.
- [x] **Step 5: Commit.** `feat(persistence): prepare checkpoint durability`.

### Task 31: Publish a manifest slot and resolve indeterminate commits

**Class:** O — Logic-owner; independent crash oracle required
**Depends on:** 15, 30
**Files:** Modify `checkpoint_coordinator.*`; create `src/persistence/commit_resolver.h/.cpp`, `tests/integration/m3_commit_publish_tests.cpp`.

- [x] **Step 1: Red-test alternate-slot publication** with exact phase injection before write, during short/torn write, after write, during flush, after flush and during readback.
- [x] **Step 2: Implement publication:** encode generation N+1 into the inactive slot, issue one exact 4096-byte write, flush manifest, re-read slot, and run full candidate validation.
- [x] **Step 3: Classify outcomes exactly at the SPEC commit point:** failures before the slot-write attempt are `NotCommitted`; slot-write or manifest-flush errors after the attempt begins are `CommitIndeterminate`; successful manifest `FlushFileBuffers()` establishes `Committed`. A later readback/validation failure is post-commit and therefore `CommittedNeedsReconciliation`, never `CommitIndeterminate` or `NotCommitted`.
- [x] **Step 4: Implement resolver** by closing/reopening file handles, validating the candidate through normal recovery, and re-executing the manifest flush when the new candidate is complete. If N+1 is complete and that flush succeeds, enter reconciliation; if the attempted slot is provably invalid and N is the only complete candidate, abort the token as `NotCommitted`; otherwise enter PersistenceFaulted.
- [x] **Step 5: Prove the resolver never guesses from cached bytes or the return value of `FlushFileBuffers`; commit `feat(persistence): publish and resolve checkpoint commits`.**

### Task 32: Install the committed view and reconcile snapshot state

**Class:** O — Logic-owner
**Depends on:** 06, 28, 31
**Files:** Modify `checkpoint_coordinator.*`, `image_session.*`, storage integration; create `tests/integration/m3_checkpoint_reconcile_tests.cpp`.

- [x] **Step 1: Red-test successful order:** durable commit proof, construct/recover new immutable view, install backing view, acknowledge token records conditionally by `dirty_epoch`, finish snapshot, then report success.
- [x] **Step 2: Red-test a block modified after the cut.** Its old pre-image is checkpointed, but `mark_persisted()` must leave the newer dirty epoch dirty.
- [x] **Step 3: Red-test install/ack/finish failures after durable commit.** Return `CommittedNeedsReconciliation`; reopen must recover the new generation and reconciliation must be safely retryable. Return plain `Committed` only after required runtime reconciliation completes.
- [x] **Step 4: Ensure COW pre-images are released only after each token member is either conditionally acknowledged or preserved as newer dirty data.**
- [x] **Step 5: Commit.** `feat(m3): reconcile durable checkpoints with live memory`.

### Task 33: Validate checkpoint atomicity with an independent crash matrix

**Class:** O — Independent test executor writes oracle; implementation owner may not edit expected outcomes
**Depends on:** 31, 32
**Files:** Create `tests/model/m3_checkpoint_crash_model_tests.cpp`, `tests/fixtures/m3/crash-matrix.json`; modify test registration.

- [x] **Step 1: Enumerate every durable primitive boundary** for records, nodes, commit, journal flush/readback, manifest write/flush/readback, view install and acknowledgment. Include short writes and sector-aligned torn slots.
- [x] **Step 2: For every crash point**, discard process memory and recover from cloned bytes only. Expected state is exactly old generation or complete new generation, never a mixed mapping.
- [x] **Step 3: Compare every block with a simple independent map model** that knows only the SPEC transaction rules, not production codecs or selectors.
- [x] **Step 4: Require deterministic seeds and persist the seed/phase on failure. Run at least 10,000 randomized schedules in CI-friendly mode and a larger stress profile manually.**
- [x] **Step 5: Commit.** `test(m3): prove checkpoint crash atomicity`.

### Task 34: Enumerate the live image for base compaction

**Class:** S — Claude can build streaming enumerator; logic owner reviews snapshot identity
**Depends on:** 22, 25, 29
**Files:** Create `src/persistence/live_extent_enumerator.h/.cpp`; extend
`journal_index.*`, `journal_reader.*`, `vhdx_reader.*` and
`composite_backing_store.*` with bounded ordered candidate streaming; add
focused reader/composite tests and `tests/unit/m3_live_extent_tests.cpp`.

- [x] **Step 1: Red-test merged logical extents** across journal DATA/ZERO/DISCARD and VHDX BAT mappings, including shadowed base payloads and final partial blocks.
- [x] **Step 2: Implement enumeration against one immutable `BackingView`** captured at compaction start. Merge journal-index lower bounds and VHDX BAT allocated candidates with two one-element lookaheads; never scan every 64 KiB logical block or enumerate live mutable RAM state.
- [x] **Step 3: Skip logical zero ranges** while preserving exact nonzero data; validate each DATA source before handing it to the VHDX writer.
- [x] **Step 4: Compare all descriptors and payload hashes with a full-map oracle over randomized sparse images.**
- [x] **Step 5: Commit.** `feat(persistence): enumerate live image extents`.

### Task 35: Compact into a new base and journal epoch

**Class:** O — Logic-owner; Claude may scaffold streaming adapters
**Depends on:** 09, 23, 29, 32, 34
**Files:** Create `src/persistence/compaction_coordinator.h/.cpp`, `tests/integration/m3_compaction_tests.cpp`.

- [x] **Step 1: Red-test the full SPEC pipeline:** hold the session's single modifying-operation gate and capture durable immutable view G; write/flush/readback/verify a unique sparse VHDX representing G; publish its unique final filename with write-through rename; create and flush journal epoch E+1.
- [x] **Step 2: Allow ordinary live mutations** during VHDX construction, but do not allow a checkpoint or second compaction to start. After the new base/journal are ready, require the explicit volume barrier for this operation, establish an M2.1 consistent cut, and stream all current dirty state relative to G into the new journal.
- [x] **Step 3: Append the new COW index and a commit for G+1 even when the dirty set is empty**, flush/readback the journal, then use the normal manifest publish, indeterminate-resolution, view-install and conditional-ack sequence. Writes after the cut remain dirty through the normal M2.1 rule.
- [x] **Step 4: Fault-test every pre/post-commit phase.** Before manifest publication, old bundle/runtime state remains authoritative and new files are unreferenced artifacts; after commit, recover/install the new view and retain old base/journal for old slots, old readers and reconciliation.
- [x] **Step 5: Implement and red-test pure `should_compact()` policy output** from explicit journal physical bytes, journal/base ratio, checkpoint count and index depth. It reports a recommendation only; it never schedules work.
- [x] **Step 6: Verify the cut-time logical disk against an independent model and verify byte-for-byte logical equivalence after reopen; commit `feat(persistence): compact committed image generations`.**

### Task 36: Retain and clean obsolete immutable files safely

**Class:** O — Logic-owner
**Depends on:** 12, 27, 35
**Files:** Create `src/persistence/retention_manager.h/.cpp`, `tests/integration/m3_retention_tests.cpp`.

- [x] **Step 1: Red-test reference accounting** from both valid manifest slots plus live `BackingView` handle identities. Unknown, replaced, hard-linked or path-escaping files are never deleted automatically.
- [x] **Step 2: Implement cleanup eligibility:** a file is obsolete only when no fully valid slot references it, no live view references it, and its validated image identity belongs to this manifest.
- [x] **Step 3: Make deletion best-effort and recoverable.** A sharing violation or crash leaves harmless files and emits diagnostics; it cannot invalidate the current generation.
- [x] **Step 4: Run races with concurrent reads, close, recovery and compaction publication. Confirm deletion starts only after the final handle/reference release.**
- [x] **Step 5: Commit.** `feat(persistence): retain immutable generation files safely`.

### Task 37: Add inspect, verify and machine-readable reports

**Class:** C — Claude-primary; schema reviewed by logic owner
**Depends on:** 27, 33, 36
**Files:** Create `src/persistence/image_inspector.h/.cpp`, `src/m3/m3_report.h/.cpp`, `src/m3/m3_evidence_publisher.h/.cpp`, `tests/unit/m3_report_tests.cpp`; reuse `src/poc/json_writer.*` patterns but do not link the persistence library to proxy/Poc report publication.

- [x] **Step 1: Red-test deterministic JSON** for image geometry, both slot statuses, selected/fallback generation, file identities, journal/root/commit descriptors, corruption provenance, cleanup candidates and no-selection cases.
- [x] **Step 2: Implement `inspect` as read-only bounded validation.** It must not truncate, repair, publish, acknowledge or delete anything.
- [x] **Step 3: Add `verify --full`** to stream every logically allocated block and report hashes/counts; default verify remains structural and bounded.
- [x] **Step 4: Redact absolute paths and block contents by default and add explicit path opt-in for diagnostics. Validate schema/version, operation id, source commit, binary hash, artifact hashes and stable enum strings.**
- [x] **Step 5: Add atomic evidence publication tests:** unique same-directory temporary file, exact write, flush, write-through rename, no overwrite, fault retention and no dependency on M1/proxy identifiers.
- [x] **Step 6: Commit.** `feat(m3): inspect and verify persistent images`.

### Task 38: Build the M3 CLI against local fake storage

**Class:** C — Claude-primary; workflow semantics reviewed by logic owner
**Depends on:** 27, 29, 30, 35, 37
**Files:** Create `src/m3/m3_cli.h/.cpp`, `src/m3/m3_lifecycle_runner.h/.cpp`, thin `src/m3/main.cpp`, `tests/integration/m3_cli_tests.cpp`; modify `src/CMakeLists.txt`, presets/test labels if required.

- [ ] **Step 1: Red-test the SPEC commands** `create`, `inspect`, `verify`, `checkpoint`, `compact`, `recover-test` and `fault-matrix` using temporary directories and fake storage; cover exit codes and JSON/stdout/stderr separation.
- [ ] **Step 2: Implement explicit geometry/path/timeout arguments** and confirmation-free non-destructive operation. No command may infer or format a Windows volume.
- [ ] **Step 3: Route every command through production session/recovery code**, not command-specific shortcuts. Surface `CommitDisposition` and stable error code in JSON.
- [ ] **Step 4: Add `--help` and invalid-input tests; ensure secrets/full paths are absent from normal output.**
- [ ] **Step 5: Commit.** `feat(m3): add persistence lifecycle cli`.

### Task 39: Connect M3 to the M1/M2 provider without changing protocol

**Class:** O — Logic-owner; Claude may generate wiring tests
**Depends on:** 28, 32, 38
**Files:** Create `src/m3/m3_provider.h/.cpp`, `src/m3/m3_windows_acceptance.h/.cpp`, `tests/integration/m3_provider_integration_tests.cpp`; modify only the M3 lifecycle composition root and CMake. Existing M1/M2 providers remain behaviorally unchanged.

- [x] **Step 1: Red-test provider startup** from a recovered image, reads through lazy backing, live writes, TRIM/zero, checkpoint, reconnect and reopen. Keep all M1 wire golden tests unchanged.
- [x] **Step 2: Inject `ImageSession` and `DynamicBlockStore`** at the composition root. Persistence code must not depend on AIM driver/proxy types; provider transport remains unaware of VHDX/journal formats.
- [x] **Step 3: Preserve stop order:** stop accepting external mutations, flush upper layers when orchestrated, checkpoint if requested, resolve any indeterminate commit, drain provider/store/session and close handles.
- [x] **Step 4: Run all M0/M1/M2 tests. Any protocol fixture or driver contract delta is a design stop, not an opportunistic edit.**
- [x] **Step 5: Commit.** `feat(m3): integrate persistence with provider lifecycle`.

### Task 40: Add a destructive real-host preflight and cleanup harness

**Class:** C — Claude-primary harness; human explicitly selects scratch targets
**Depends on:** 24, 39
**Files:** Create `tests/e2e/M3Harness.Common.ps1`, `tests/e2e/test-m3-harness-preflight.ps1`, `tests/e2e/run-m3-persistence.ps1`, `tools/Invoke-M3Verification.ps1`, `tools/Verify-M3Json.ps1`, `docs/testing/m3-e2e.md`.

- [x] **Step 1: Probe administrator rights, AIM driver/device interface, build artifacts, filesystem capabilities, free space, power policy and native VHDX APIs. Emit JSON plus human summary.**
- [x] **Step 2: Require an explicit scratch directory and test disk identity.** Refuse system volumes, workspace roots, ambiguous device paths, mounted user data and paths outside the declared scratch root.
- [x] **Step 3: Implement finally-style cleanup** for provider process, proxy/disk device, drive letter, VHDX attachment and temporary files. Record anything left behind with manual recovery commands.
- [x] **Step 4: Add stable verification routing and fail-closed JSON validation.** `Invoke-M3Verification.ps1` exposes Local, Preflight, RealFunctional, Crash and Soak tiers; unavailable implementations return BLOCKED/nonzero until their tasks land. `Verify-M3Json.ps1` rejects schema/source-commit/binary-hash mismatches and required SKIP.
- [x] **Step 5: Dry-run test all safety checks without administrator/device access; add Pester or script-level tests for target rejection.**
- [ ] **Step 6: Commit.** `test(m3): add safe real-host e2e harness`.

### Task 41: Prove real AIM, NTFS, lazy loading and native VHDX compatibility

**Class:** O — Logic-owner executes/reviews; Claude can operate the frozen harness only
**Depends on:** 24, 32, 35, 40
**Files:** Create `tests/e2e/Invoke-M3NtfsWorkload.ps1`; extend `tests/e2e/run-m3-persistence.ps1`, scenario manifests and `docs/evidence/m3/` generated reports only.

- [ ] **Step 1: Create generation zero**, export through the real AIM endpoint, initialize/format the scratch device as NTFS using explicit disk identity, mount it and perform deterministic file-tree writes, overwrites, sparse files, rename/delete and flush.
- [ ] **Step 2: Checkpoint after a filesystem flush**, record memory/backing I/O counters, cleanly stop, reopen and verify file hashes/metadata. Formatting is a test setup operation, not a hidden product feature.
- [ ] **Step 3: Restart with cold memory without repartitioning or formatting**, run the declared filesystem check, read a small sample and prove bounded startup/read I/O; then read the full corpus and verify exact content. Demonstrate that untouched image payload is not eagerly loaded.
- [ ] **Step 4: Flush the volume again and compact the durable NTFS generation**, prove the new `base_generation` hash matches that exact durable view, take the RAM disk offline, open that base VHDX with Windows native APIs read-only, verify partition/NTFS recognition and detach. Never compare a pre-compaction base to newer journal contents or dual-attach the same writable data.
- [ ] **Step 5: Store command lines, versions, seed, counters, hashes and cleanup result; commit harness/manifest changes, not mutable machine-specific evidence unless the repository policy requires it.**

### Task 42: Execute simulated and real-process crash recovery matrices

**Class:** O — Logic-owner; independent expected-outcome oracle
**Depends on:** 33, 40, 41
**Files:** Create `tests/e2e/Invoke-M3CrashMatrix.ps1`; extend `tests/e2e/run-m3-persistence.ps1`, scenario definitions and generated evidence schema.

- [ ] **Step 1: Execute deterministic simulated crashes** at every named phase using file clones and fresh recovery processes.
- [ ] **Step 2: Execute real process termination** before journal flush, after journal flush, during/before manifest publication where injection permits, after publication proof and before/after reconciliation. Power-loss claims require an actual controlled power-cut setup and are otherwise labelled simulated.
- [ ] **Step 3: After each run, recover, mount read-only first, verify the expected old/new generation and full corpus hash, then inspect slot/fallback diagnostics.**
- [ ] **Step 4: Fail on mixed generation, silent fallback from corruption, unresolved indeterminate state, leaked device/drive/handle or non-reproducible outcome.**
- [ ] **Step 5: Record environment and pass/fail evidence with stable scenario IDs; commit `test(m3): add crash recovery matrix`.**

### Task 43: Run soak, resource, performance and final M3 gates

**Class:** O — Logic-owner evaluates; Claude can run commands and collect raw output
**Depends on:** 36, 41, 42
**Files:** Create `tests/e2e/Invoke-M3Soak.ps1`; extend `tools/Invoke-M3Verification.ps1` and `tools/Verify-M3Json.ps1`; create/modify `docs/evidence/m3/M3_VERIFICATION_REPORT.md` and machine-readable report files.

- [ ] **Step 1: Run the full test suite** from a clean Release build and archive exact commands/results.
- [ ] **Step 2: Run mixed read/write/TRIM/checkpoint/reopen/compaction soak** with deterministic seeds; include concurrent first-touch, low-memory cache pressure, injected I/O failures and repeated close/reopen.
- [ ] **Step 3: Measure bounded-memory and lazy-I/O invariants**, checkpoint stall/COW pressure, journal growth/compaction ratio, handle/thread/device leaks and data hashes. Treat thresholds from the SPEC as gates, not observations.
- [ ] **Step 4: Re-run native VHDX and real AIM/NTFS scenarios** after soak. Record skipped prerequisites separately; a skipped required gate cannot produce GO.
- [ ] **Step 5: Issue `GO`, `NO-GO` or `BLOCKED`** with the internal M3-V0 baseline and every official M3-V1..V7 gate, artifact hash, unresolved defect and limitation. Commit only reproducible evidence/report updates.

### Task 44: Close documentation, license and design drift

**Class:** C for documentation inventory; O for final acceptance
**Depends on:** 43
**Files:** Modify project README/status/license notices and M3 design/plan/task list only where implementation evidence requires correction.

- [ ] **Step 1: Document supported/unsupported VHDX profile**, persistence files, backup/restore rules, failure outcomes, compaction, safe shutdown, limitations and operator recovery for `CommitIndeterminate`.
- [ ] **Step 2: Inventory every third-party header/library/tool** and confirm license/redistribution obligations. The product must not copy AIM Toolkit user-mode code unless deliberately reviewed and documented.
- [ ] **Step 3: Perform line-by-line drift review** against the M3 SPEC, this PLAN and `M3_TASKLIST.md`. Product code does not redefine frozen semantics by implementation accident.
- [ ] **Step 4: Run final clean-tree Debug build/tests, Release build/tests, and verify docs commands. Update the M3 verdict only from fresh evidence.**
- [ ] **Step 5: Commit.** `docs(m3): close persistence milestone`.

## Coverage and ownership matrix

| Concern | Primary tasks | Required owner | Independent evidence |
|---|---:|---|---|
| Whole-disk snapshot cut and bounded COW | 01–09 | Logic owner | Reference disk model and M2 regression |
| Binary codecs and checked I/O | 10–14, 16, 21 | Claude-primary with review | Golden bytes, truncation/fuzz fixtures |
| Recovery selection and immutable layering | 15, 17–20, 22, 25, 27–29 | Logic owner for semantics | Separate oracle and corruption matrix |
| VHDX creation/compatibility | 21–24 | Split | Windows native API gate |
| Checkpoint atomicity | 30–33 | Logic owner | Independent crash model |
| Compaction/retention | 34–36 | Logic owner | Logical hash and file-reference model |
| Product integration and UX | 37–40 | Split | Full regressions and dry-run safety tests |
| Real-host acceptance | 41–43 | Logic owner | AIM/NTFS, crash and soak evidence |
| Release closure | 44 | Logic owner | Fresh clean-tree verification |

## Required stable test inventory

- Internal `m2.1-v0` through `m2.1-v5` labels split contracts, mutation-cut linearization, pre-image semantics, bounded pressure, the whole-disk reference model, and lifecycle cleanup. Together they implement the official **M3-V3** gate; they are not replacement milestone numbers.
- Internal `m3-v0` is the fresh 57-test baseline/build-boundary check. The SPEC's official acceptance gates start at M3-V1.
- `m3-v1` covers checked I/O, integrity primitives, manifest/journal/index codecs, the strict VHDX reader/writer profile, golden bytes, corruption and fuzz.
- `m3-v2` covers recovery candidate selection, immutable layering, checkpoint publication/reconciliation and the independent crash matrix.
- `m3-v3` aggregates all M2.1 labels plus generation/ack/lazy-load concurrency integration.
- `m3-v4` covers compaction, journal rotation, retention equivalence and fault safety.
- `m3-v5` covers native Windows VHDX open/attach/geometry/data compatibility while the AIM disk is offline.
- `m3-v6` provider, real AIM/NTFS and lazy-loading E2E.
- `m3-v7` covers resources, lazy-I/O limits, soak, performance and kill/restart repetition.

Every label must be registered in CTest or in a versioned harness that emits a machine-readable result. A skipped environment-dependent gate is not a pass.

## Stop conditions

Stop the current task and record `BLOCKED` when any of these occurs:

- The implementation requires an M1 wire-protocol or AIM driver-contract change not approved by a revised SPEC.
- A proposed shortcut permits mixed-generation recovery, treats corruption as absence, or acknowledges dirty state before durable publication is proven.
- Snapshot COW cannot remain within its configured bound without silently blocking beyond the declared stall contract.
- Product-written VHDX cannot pass native Windows validation for the frozen profile.
- A destructive E2E target cannot be identified unambiguously.
- The independent crash/reference model disagrees with production behavior and the disagreement is not explained by a SPEC defect.
- Required real-host evidence is unavailable; report `BLOCKED`, never manufacture GO from simulated tests alone.

## Definition of done

M3 is complete only when:

1. Tasks 00–44 are `DONE` in `M3_TASKLIST.md`, each with pre-task and post-task drift review.
2. M2.1 proves one bounded, whole-disk snapshot cut under concurrency.
3. Generation zero, checkpoint, reopen, fallback, compaction and retention work through production paths.
4. Recovery always selects the highest fully valid generation and never exposes a mixed image.
5. First-touch loading is demonstrably lazy and bounded.
6. Product VHDX files pass native Windows validation while offline from AIM.
7. All M0/M1/M2 regressions, the internal M3-V0 baseline, and official M3-V1..V7 gates pass from a clean Release build.
8. The real AIM/NTFS, crash and soak reports support a final `GO`; otherwise the milestone remains `NO-GO` or `BLOCKED`.

## Execution handoff

Use `M3_TASKLIST.md` as the parent status/authority document and
`M3_MICROTASKS.md` as the mandatory remaining-work execution document. Give an
agent exactly one ready microcard at a time, including its exact allowlist,
frozen test discovery count, expected semantic RED and acceptance commands.
Medium/non-SOTA agents may complete `M` cards; they may only prepare the
explicitly allowed tests/scaffolds for `H` cards and must stop at every `X`
card with `PAUSED_FOR_HIGH_REASONING`. Independent oracle/fixture cards are
committed and hashed before production cards, which then treat them as
read-only. Class O production semantics remain under logic-owner control.

The recommended waves are:

1. Tasks 00–09: land M2.1 and do not start persistence integration until its verdict is GO.
2. In parallel after prerequisites: Tasks 10–14, 16, 21 and the test scaffolds for 17–20/22.
3. Tasks 15–33: recovery, lazy backing and checkpoint atomicity, with short review checkpoints after every task.
4. Tasks 34–39: compaction, retention, reports, CLI and provider integration.
5. Tasks 40–44: destructive-host gates, crash/soak evidence and final acceptance.

Never hand an agent “the rest of M3” or even an unsplit parent Task. Every
handoff must include one frozen microcard ID, dependency SHAs, reasoning gate,
expected assertion-level RED, exact allowed/forbidden paths, nonzero test
discovery count, verification commands, commit boundary and post-card drift
audit.
