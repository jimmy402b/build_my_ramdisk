# M2 Dynamic Block Store Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `subagent-driven-development` (recommended) or `executing-plans` to implement this plan task-by-task. Execute one numbered task at a time. Before each task, read the corresponding section in `M2_TASKLIST.md`; after each task, run every listed check, record the evidence, and re-read this plan to confirm the implementation has not drifted.

**Goal:** Implement a production-shaped, thread-safe dynamic block store that exposes the accepted M1 `IBlockDevice` contract, reserves a large contiguous virtual address range, commits 64 KiB blocks only when needed, lazily loads immutable backing views, supports generation-safe clean eviction and versioned dirty snapshots, and passes both deterministic model tests and a real 64 GiB NTFS dynamic-disk acceptance run.

**Architecture:** Keep the accepted M1 proxy and protocol layers unchanged. Add a storage layer implementing `IBlockDevice`, with immutable `IBackingStore` objects behind an atomically replaceable, monotonically increasing `BackingView`, plus a Windows virtual-memory arena, sparse block directory, range-operation engine, snapshot engine, and eviction manager. Test-only M2 lifecycle code injects this store into the M1 proxy, creates a 64 GiB SCSI disk, drives an NTFS workload while the provider is alive, and captures structured evidence. M2 defines generation installation and exact version/kind/generation acknowledgement but does not implement persistence, journaling, swap, compression, deduplication, encryption, networking, or VHD/VHDX parsing.

**Tech Stack:** C++20, MSVC, CMake/CTest, Windows `VirtualAlloc`/`VirtualFree`, Win32 synchronization and process APIs, the repository's existing lightweight test framework in `tests/support/test_framework.h`, PowerShell 7/Windows PowerShell for elevated end-to-end orchestration, and JSON evidence using the repository's M1 conventions.

**Status:** Ready to execute from Task 00. No M2 product code is implemented or accepted by this planning document.

---

## 0. Authority, assumptions, and non-negotiable boundaries

The approved design authority is:

- `docs/superpowers/specs/2026-08-01-m1-m2-proxy-dynamic-block-store-design.md`
- this implementation plan
- `M2_TASKLIST.md`

The accepted M1 baseline is present on `master` at planning commit `2adf84a`; its functional M1 acceptance evidence identifies `3eac4f7` as the tested implementation commit. This is a planning baseline, not a substitute for a fresh run. Before executing Task 01, the implementing agent must prove all of the following:

- `docs/validation/m1-proxy-acceptance.md` exists and records `M1 GO`;
- the M0 acceptance evidence required by the design records final `M0 GO` for the namespace/driver-only contract;
- `2adf84a` is the current planning baseline and contains the accepted `3eac4f7` implementation, or the worktree relationship is explicitly recorded;
- the fresh local M1 verification tier named by the M1 plan passes;
- `IBlockDevice`, cancellation/deadline context, proxy ownership, structured result reporting, and exact-device cleanup are available and covered by tests.

If any item is absent, stop M2 execution. Repair or finish M1 under the M1 plan; do not invent compatibility shims inside M2.

M2 must preserve these boundaries:

- `src/proxy/` may depend on `IBlockDevice`; it must not depend on `DynamicBlockStore`, `VirtualAlloc`, block metadata, eviction, or backing-store details.
- `src/storage/` must not include AIM/Arsenal driver headers or know SCSI device identifiers.
- no real-driver test may start until all non-destructive local M2 gates pass.
- no M2 code may claim durable persistence. Dirty snapshots and generation handoff are M3-facing contracts only; M2 may use fake successor views to test them.
- no request may silently succeed after cancellation, deadline expiry, resource exhaustion, invariant failure, or a partial range mutation.
- dirty data must never be evicted.

## 1. Fixed contracts and implementation decisions

### 1.1 Geometry and memory

- Logical sector size: inherited from M1; M2 acceptance uses 512-byte sectors unless the M1 contract explicitly supplies another supported value.
- Allocation block size: exactly 64 KiB (`65,536` bytes).
- Sparse-directory leaf size: exactly 1,024 blocks, covering 64 MiB.
- Public logical size remains the requested size; arena reservation is rounded up to 64 KiB with checked arithmetic.
- The arena is reserved once with `MEM_RESERVE | PAGE_NOACCESS`.
- Resident data blocks are committed with `MEM_COMMIT | PAGE_READWRITE`.
- Clean block memory is released with `MEM_DECOMMIT`; the full reservation is released only with `MEM_RELEASE` at store destruction.
- The implementation must support at least 64 GiB and 1 TiB logical-size construction without committing proportional data memory.

### 1.2 Public storage types

Create the following public concepts without altering their meaning:

```cpp
inline constexpr std::uint64_t kBlockSizeBytes = 64ULL * 1024ULL;
inline constexpr std::uint64_t kBlocksPerLeaf = 1024ULL;
using BackingGeneration = std::uint64_t;

enum class InitialBlockState {
    CleanZero,
    BackingUnresident,
};

struct DynamicBlockStoreOptions {
    std::uint64_t soft_limit_bytes;
    std::uint64_t hard_limit_bytes;
    std::uint64_t eviction_low_watermark_bytes;
    std::uint64_t max_snapshot_batch_bytes;
    std::size_t max_snapshot_batch_records;
    std::size_t lock_stripe_count;
};

struct DynamicBlockStoreCreateArgs {
    std::uint64_t logical_size_bytes;
    std::uint32_t sector_size_bytes;
    DynamicBlockStoreOptions options;
    InitialBlockState initial_state;
    std::shared_ptr<const IBackingStore> backing_store;
    BackingGeneration initial_backing_generation;
};
```

Validation rules:

- logical size and sector size are non-zero;
- logical size is sector-aligned;
- all additions, multiplications, and round-ups are checked for overflow;
- `0 < eviction_low_watermark_bytes <= soft_limit_bytes < hard_limit_bytes`;
- memory limits are block-aligned;
- snapshot byte limit is at least one block and block-aligned;
- snapshot record limit is positive;
- stripe count is a power of two and at least 64;
- `BackingUnresident` requires a non-null backing store;
- `CleanZero` may use `ZeroBackingStore`, and a null pointer must either be rejected or normalized consistently by the factory and tests. Use normalization to `ZeroBackingStore`.
- the initial generation may be any recovered generation; a new empty store passes `0` explicitly;

The storage library has no product defaults for these limits; every caller passes them explicitly. The acceptance tool may derive and record block-aligned test values from physical RAM: soft limit at 25%, hard limit at 50%, and low watermark at 80% of the derived soft limit, provided all validation constraints and the 1 GiB memory test preflight are satisfied.

Construction must use a fallible factory such as:

```cpp
static Result<std::unique_ptr<DynamicBlockStore>> create(
    DynamicBlockStoreCreateArgs args);
```

Do not expose a partially initialized object or throw across proxy request boundaries.

### 1.3 Backing-store contract

Each backing object is block-granular and immutable after publication. The Store may replace the complete current view, but never mutate an already published object:

```cpp
enum class BlockLoadKind { Data, AllZero };

struct BlockLoadResult {
    BlockLoadKind kind;
};

class IBackingStore {
public:
    virtual ~IBackingStore() = default;
    virtual Result<BlockLoadResult> load_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const = 0;
};

struct BackingView {
    BackingGeneration generation;
    std::shared_ptr<const IBackingStore> store;
};
```

`destination.size()` is always exactly 64 KiB. Implementations must be thread-safe. M2 supplies `ZeroBackingStore` and test-only `FakeBackingStore`; file-backed persistence belongs to M3. The Store additionally exposes:

```cpp
BackingGeneration backing_generation() const noexcept;
Result<void> install_backing_view(BackingView next_view);
```

The current pointer and generation are published as one coherent pair. `next_view.store` must be non-null and `next_view.generation` must be strictly greater than the current generation. A successor is a complete logical-disk view containing every earlier acknowledged generation, not a patch. Same/lower generations, wraparound, null stores, and closed-store installation return `BackingGenerationConflict` or the precise lifecycle error without changing the current view. In-flight readers retain a `shared_ptr` to the captured object.

### 1.4 Block states and metadata

Stable logical states:

- `Unresident`: content exists in backing but is not resident.
- `ResidentClean`: committed data is covered by `clean_generation` and every complete successor view; it may be evicted only when that generation is installed.
- `ResidentDirty`: committed data differs from backing and must not be evicted.
- `ZeroClean`: logical zero content agrees with the clean source and needs no committed page.
- `ZeroDirty`: logical zero content overrides non-zero or unknown backing data and must be snapshotted.

Transient states:

- `Loading`
- `Committing`
- `Evicting`

Each block entry must retain enough information to restore its prior stable state after a transient failure. At minimum it needs state, prior state, owner/transition token, monotonically increasing 64-bit content version, pin count, `last_access_epoch`, `dirty_epoch`, `dirty_base_generation`, `clean_generation`, reference bit, load/transition epoch, and the dirty zero operation kind needed for snapshots. Every successful mutation captures the current backing generation as `dirty_base_generation`; every backing load or successful persisted acknowledgement records its source as `clean_generation`. Version or generation wrap is a fatal invariant failure, not a return to zero.

Metadata leaves are lazy and have stable lifetime until store destruction. A fixed power-of-two stripe array supplies mutexes and condition variables. Leaf publication may use a separate short-held lock, but no global or leaf-publication lock may be held during backing I/O, memory pressure handling, or a wait on another transition.

### 1.5 Range semantics

All requests must validate the full byte range before changing state:

- offset and length are sector-aligned;
- `offset + length` uses checked arithmetic and is within logical size;
- buffers match request length;
- cancellation and deadline are checked before work and at bounded intervals;
- zero-length READ/WRITE is invalid, exactly as required by the M1/M2 contract.

Split valid requests by 64 KiB block boundaries. Acquire at most one stripe/block mutation lock at a time. Process blocks in ascending order. Do not hold a metadata lock while copying a large user buffer, calling backing storage, committing/decommitting memory, or waiting on a condition variable.

### 1.6 Snapshot cursor semantics

Use the approved M3-facing types:

```cpp
enum class DirtyRecordKind { Data, Zero, Discard };

struct DirtyBlockSnapshot {
    std::uint64_t block_index;
    std::uint64_t version;
    DirtyRecordKind kind;
    BackingGeneration base_generation;
    std::vector<std::byte> data; // exactly 64 KiB only for Data
};

struct DirtyScanCursor {
    std::uint64_t next_block_index{};
};
```

The store exposes `copy_dirty_batch(...)`, `install_backing_view(...)`, and `mark_persisted(...)` through the exact signatures approved by the design document. A cursor initialized to zero scans `[next_block_index, total_blocks)`. Completion sets it to `total_blocks`; another call returns an empty batch. The caller explicitly resets it to zero for a new pass. Batch limits are the minimum of per-call and configured limits, and byte limits are block-aligned and at least one block.

The exact methods are:

```cpp
Result<std::vector<DirtyBlockSnapshot>> copy_dirty_batch(
    const OperationContext& context,
    DirtyScanCursor& cursor,
    std::size_t max_bytes);

Result<void> mark_persisted(
    std::uint64_t block_index,
    std::uint64_t expected_version,
    DirtyRecordKind expected_kind,
    BackingGeneration persisted_generation);
```

`copy_dirty_batch` copies immutable data plus `(block_index, version, kind, base_generation)` while protecting each block against concurrent mutation. `mark_persisted` cleans only if the block is still dirty, version and kind exactly match, `persisted_generation > dirty_base_generation`, and that generation has already been installed (`persisted_generation <= backing_generation()`). A stale version/kind returns `VersionChanged`; an invalid/uninstalled generation returns `BackingGenerationConflict`. Both leave state and content dirty.

The only legal M3 handoff order is: copy a batch; build and durably publish a complete successor backing object; call `install_backing_view()`; then acknowledge only records included in that generation. M2 tests this order using immutable fake views. It does not write files or claim durability.

## 2. Target repository shape

Create or modify only the files required by the accepted M1 layout. If M1 chose equivalent names, adapt paths while preserving ownership boundaries and record the mapping in the task log before editing.

```text
src/
  storage/
    block_constants.h
    block_math.h
    dynamic_block_types.h
    backing_store.h
    zero_backing_store.h
    zero_backing_store.cpp
    virtual_memory_arena.h
    virtual_memory_arena.cpp
    block_directory.h
    block_directory.cpp
    dynamic_block_store.h
    dynamic_block_store_internal.h
    dynamic_block_store_core.cpp
    dynamic_block_store_read.cpp
    dynamic_block_store_write.cpp
    dynamic_block_store_ranges.cpp
    dynamic_block_store_backing.cpp
    dynamic_block_store_snapshot.cpp
    eviction_manager.h
    eviction_manager.cpp
  m2/
    m2_provider.h
    m2_provider.cpp
    m2_performance_probe.h
    m2_performance_probe.cpp
    m2_volume_workload.h
    m2_volume_workload.cpp
    m2_lifecycle_runner.h
    m2_lifecycle_runner.cpp
    m2_report.h
    m2_report.cpp
    m2_cli.h
    m2_cli.cpp
    main.cpp
tests/
  support/
    fake_backing_store.h
    fake_virtual_memory_api.h
    reference_block_model.h
  unit/
    block_math_tests.cpp
    backing_store_tests.cpp
    virtual_memory_arena_tests.cpp
    block_directory_tests.cpp
    dynamic_block_store_core_tests.cpp
    dynamic_block_store_read_tests.cpp
    dynamic_block_store_write_tests.cpp
    dynamic_block_store_range_tests.cpp
    dynamic_block_store_backing_generation_tests.cpp
    dynamic_block_store_snapshot_tests.cpp
    dynamic_block_store_eviction_tests.cpp
    dynamic_block_store_limit_tests.cpp
    dynamic_block_store_model_tests.cpp
    dynamic_block_store_concurrency_tests.cpp
    m2_provider_tests.cpp
    m2_lifecycle_runner_tests.cpp
    m2_report_tests.cpp
    m2_performance_probe_tests.cpp
  integration/
    m2_virtual_memory_tests.cpp
    m2_shm_proxy_tests.cpp
  e2e/
    Invoke-M2NtfsWorkload.ps1
    run-m2-dynamic-disk.ps1
    test-m2-harness-preflight.ps1
tools/
  Verify-M2Json.ps1
  Invoke-M2Verification.ps1
docs/validation/
  m2-dynamic-block-store-acceptance.md
```

Update `src/CMakeLists.txt` and `tests/CMakeLists.txt` incrementally. Extend the existing `myrd_storage` target and keep it independent from driver/proxy implementation targets; add `myrd-m2-e2e` as the executable that may depend on storage plus the accepted M1 proxy/runtime/driver targets. Use the repository's existing test framework and registration helper rather than adding another test dependency.

## 3. Verification tiers and evidence

Use these stable tier names in CTest labels, scripts, task records, and the final report:

- **M2-V0 — Baseline:** authority docs, M1 GO evidence, accepted commit, clean Release configure/build, fresh required M1 local suite.
- **M2-V1 — Core unit:** options, checked block math, immutable backing contract, generation installation, sparse directory, state transitions, reads, writes, UNMAP/ZERO, snapshots, limits, eviction, and post-ack reload safety.
- **M2-V2 — Model and VM integration:** deterministic 100,000-operation reference-model run plus 64 GiB and 1 TiB reserve/commit/decommit tests.
- **M2-V3 — Concurrency/resource:** same-block loading, old-load/view-switch races, read/write/range races, generation-bound acknowledgement/eviction races, cancellation/deadline, memory pressure, long stress, invariant and leak checks.
- **M2-V4 — Proxy/local integration:** all required M1 regressions and M2 store behind the local shared-memory proxy path, with no kernel driver.
- **M2-V5 — Real NTFS gate:** one exact 64 GiB M2 device, GPT/NTFS, at least 4 GiB verified file data, modification/rename/delete, DeleteNotify/ReTrim, cleanup and baseline restoration.
- **M2-V6 — Performance/fault gate:** fixed-vs-dynamic QD1 probe, memory thresholds, injected failure rollback, exact-device cleanup, service restoration.
- **M2-V7 — Release soak/final:** 20 create/read/write/unmap/delete cycles, complete evidence bundle, final M1 regression, acceptance verdict.

Canonical commands use the repository's checked-in presets; agents must not invent aliases:

```powershell
cmake --preset msvc
cmake --build --preset release
ctest --preset release --output-on-failure -L m2-v1
ctest --preset release --output-on-failure -L m2-v2
ctest --preset release --output-on-failure -L m2-v3
ctest --preset release --output-on-failure -L m2-v4
pwsh -NoProfile -File tools/Invoke-M2Verification.ps1 -Tier M2-V5 -Configuration Release
pwsh -NoProfile -File tools/Invoke-M2Verification.ps1 -Tier M2-V6 -Configuration Release
pwsh -NoProfile -File tools/Invoke-M2Verification.ps1 -Tier M2-V7 -Configuration Release
```

Every tier writes a uniquely named JSON result and log directory. Never overwrite prior evidence. JSON validation is part of the tier and must fail closed on a missing field, failed cleanup, unexpected device, unbounded memory sample, checksum mismatch, or skipped required phase.

## 4. Implementation tasks

### Task 0: Freeze the accepted M1 baseline and establish M2's build shell

**Files:**

- Inspect: `docs/validation/m1-proxy-acceptance.md`
- Inspect: M1 public proxy/block-device headers and CMake targets
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/validation/m2-dynamic-block-store-acceptance.md`

**Steps:**

1. Record the current commit, branch, worktree status, compiler version, SDK version, CMake version, generator, functional M1 commit `3eac4f7`, and merged planning baseline `2adf84a` in the M2 acceptance document.
2. Run the fresh M1 local verification required by M1's plan. Stop if it fails.
3. Extend the existing `myrd_storage` target; do not create a duplicate storage library. Add an empty-but-buildable `myrd-m2-e2e` executable target with strict warnings matching M1, without faking storage behavior to make linkage pass.
4. Add CTest labels for `m2-v1` through `m2-v4`; elevated tiers remain script-driven.
5. Configure and build Release. Run the M1 suite again to prove the shell did not regress it.

**Acceptance:** M1 is demonstrably GO, Release builds, the new targets are structurally present, and no M1 source semantics changed.

**Commit:** `build(m2): establish dynamic store targets and baseline`

### Task 1: Define checked geometry, options, public states, and statistics

**Files:**

- Create: `src/storage/block_constants.h`
- Create: `src/storage/block_math.h`
- Create: `src/storage/dynamic_block_types.h`
- Create: `tests/unit/block_math_tests.cpp`
- Modify: storage/test CMake files

**Steps:**

1. Write failing tests for checked add, checked round-up, byte-to-block splitting, last partial block, out-of-range ranges, option relationships, power-of-two stripes, null-backing normalization, and 64 GiB/1 TiB geometry.
2. Implement constexpr constants and checked helpers without narrowing conversions.
3. Define `BackingGeneration`, stable/transient states, initial-state mode, options, create arguments, dirty-record types including `base_generation`, cursor, operation/result counters, `BackingGenerationConflict`, and a snapshot-able `DynamicBlockStoreStats` structure.
4. Include every storage statistic named by the spec with the exact stable meaning: `logical_size_bytes`, `reserved_bytes`, `committed_bytes`, `resident_clean_blocks`, `resident_dirty_blocks`, `zero_clean_blocks`, `zero_dirty_blocks`, `loading_blocks`, `evicted_blocks_total`, `backing_reads_total`, `backing_read_bytes_total`, `uncached_reads_total`, `unmap_bytes_total`, `zero_bytes_total`, `hard_limit_rejections`, `commit_failures`, `decommit_failures`, `snapshot_records_total`, `snapshot_version_conflicts`, `backing_generation`, `backing_view_installs_total`, `backing_generation_conflicts`, and `stale_backing_load_retries`. Additional diagnostics such as waits or pressure passes are permitted but cannot replace these fields.
5. For a `CleanZero` store, compute `zero_clean_blocks` from total blocks minus blocks that diverge from the implicit state. Never allocate leaves merely to count implicit zero blocks.
6. Keep stats observation race-free using atomics or a coherent snapshot lock; no counter may determine correctness.

**Verification:**

```powershell
cmake --build --preset release
ctest --preset release --output-on-failure -R "unit.m2.block_math"
```

**Acceptance:** every invalid boundary is rejected before mutation, and geometry supports 1 TiB without proportional allocation.

**Commit:** `feat(storage): define dynamic block geometry and options`

### Task 2: Establish the backing-store abstraction and deterministic fakes

**Files:**

- Create: `src/storage/backing_store.h`
- Create: `src/storage/zero_backing_store.h`
- Create: `src/storage/zero_backing_store.cpp`
- Create: `tests/support/fake_backing_store.h`
- Create: `tests/unit/backing_store_tests.cpp`
- Modify: storage/test CMake files

**Steps:**

1. Write tests proving destination size validation, `AllZero` behavior, deterministic block data, concurrent const loads, cancellation/deadline propagation, injected failure, per-block load counts, and safe lifetime of an old fake after a newer view is installed elsewhere.
2. Implement the const `IBackingStore::load_block` contract exactly as approved and `ZeroBackingStore` as a stateless thread-safe implementation returning `AllZero` without touching destination bytes.
3. Implement immutable `FakeBackingStore` instances with deterministic data from `(seed, block_index)`, explicit all-zero blocks, a blocking gate for concurrency tests, failure injection, and mutable synchronized load counters. Provide a test builder that creates a new complete fake object for each generation; never mutate published block content. Test support must never enter production targets.
4. Require the store, not the backing, to zero its read destination when `AllZero` is returned.

**Verification:** `ctest ... -R "unit.m2.backing_store"` plus Release build.

**Acceptance:** the interface can model data, zero, wait, cancellation, and failure without filesystem or driver dependencies.

**Commit:** `feat(storage): add immutable block backing contract`

### Task 3: Implement and fault-test the Windows virtual-memory arena

**Files:**

- Create: `src/storage/virtual_memory_arena.h`
- Create: `src/storage/virtual_memory_arena.cpp`
- Create: `tests/support/fake_virtual_memory_api.h`
- Create: `tests/unit/virtual_memory_arena_tests.cpp`
- Modify: storage/test CMake files

**Steps:**

1. Introduce a narrow internal VM API seam around reserve, commit, decommit, release, `VirtualQuery`, and system-error capture. Production uses Win32; unit tests use the fake.
2. Write failing tests for reserve failure, aligned address calculation, commit/decommit success, commit/decommit failure, double transition rejection, full release, move/lifetime behavior, and checked 1 TiB reservation size.
3. Implement one contiguous reservation. Never call `MEM_RELEASE` for a block; decommit individual blocks with the exact size and release only the base address at arena destruction.
4. Track committed bytes with correctness-independent accounting and expose `VirtualQuery`-based verification so integration tests can cross-check committed/decommitted regions.
5. Treat decommit failure as a surfaced storage error and leave metadata conservative: a block must not be announced unresident if its memory transition did not complete.

**Verification:** `ctest ... -R "unit.m2.virtual_memory"` under Release.

**Acceptance:** all Win32 failures are observable and no test can produce an inaccessible state/metadata mismatch.

**Commit:** `feat(storage): add reserved virtual memory arena`

### Task 4: Build the lazy sparse directory and fixed lock stripes

**Files:**

- Create: `src/storage/block_directory.h`
- Create: `src/storage/block_directory.cpp`
- Create: `src/storage/dynamic_block_store_internal.h`
- Create: `tests/unit/block_directory_tests.cpp`
- Modify: storage/test CMake files

**Steps:**

1. Write tests for leaf-index math, lazy allocation, 1,024 entries per leaf, concurrent same-leaf publication, different-leaf publication, stable entry addresses, stripe selection, and metadata scaling at 64 GiB/1 TiB.
2. Define `BlockEntry` and its stable/transient invariants, including `dirty_base_generation` and `clean_generation`. Do not place a mutex or condition variable in every block.
3. Allocate a top-level array of atomic/shared leaf pointers sized by block count. Leaves are allocated only when metadata differs from implicit initial state or a transition needs an entry.
4. Publish exactly one leaf under a short critical section; losing allocations are released safely. Published leaves remain alive until store destruction.
5. Allocate the fixed stripe array during store creation. Each stripe owns a mutex and condition variable; stripe index is `block_index & (count - 1)`.
6. Add debug-only invariant validation callable by tests and stress code.

**Verification:** `ctest ... -R "unit.m2.block_directory"` and a test proving construction does not allocate one entry per logical block.

**Acceptance:** sparse metadata is race-free, stable-addressed, and independent of logical capacity except for the small top-level pointer array.

**Commit:** `feat(storage): add sparse block metadata directory`

### Task 5: Create the store factory, lifecycle, default-zero semantics, and stats snapshot

**Files:**

- Create: `src/storage/dynamic_block_store.h`
- Create: `src/storage/dynamic_block_store_core.cpp`
- Create: `tests/unit/dynamic_block_store_core_tests.cpp`
- Modify: storage/test CMake files

**Steps:**

1. Write failing tests for every create-argument rule, explicit initial generation (including restored non-zero generations), clean-zero implicit blocks logically using that generation without allocating leaves, backing-unresident implicit blocks, exact logical size, rounded reservation size, null backing normalization, destruction after partial construction failure, and coherent initial stats.
2. Make `DynamicBlockStore` implement the accepted M1 `IBlockDevice` directly. Do not add a proxy-specific adapter unless the M1 ABI forces one; document any required adapter.
3. Factory order: validate arguments, normalize backing, create the coherent initial `{generation, shared_ptr<const IBackingStore>}` view, reserve arena, allocate stripe set/top directory, construct eviction infrastructure stopped, then publish the object.
4. Add a closing flag and deterministic destructor ordering: reject new calls, cancel/wake internal waiters, stop worker, wait for pins/transitions to quiesce, destroy metadata, release arena.
5. Expose read-only geometry, options, stats snapshot, invariant-check hook for tests, `backing_generation()`, `install_backing_view()`, dirty-copy API, and persisted-ack API. Keep mutable internals private.
6. Increment no data-operation counters until corresponding effects are committed. Make `features()` report writable storage with UNMAP and ZERO support as defined by M1.

**Verification:** `ctest ... -R "unit.m2.store_core"` under Release and M1 interface compile tests.

**Acceptance:** valid stores are fully initialized, invalid stores have no leaked reservation/thread, and M1 sees only `IBlockDevice`.

**Commit:** `feat(storage): create dynamic block store lifecycle`

### Task 6: Implement zero/resident READ before backing loads

**Files:**

- Create: `src/storage/dynamic_block_store_read.cpp`
- Create: `tests/unit/dynamic_block_store_read_tests.cpp`
- Modify: storage/test CMake files

**Steps:**

1. Write tests for whole/partial/cross-block reads from implicit `CleanZero`, explicit `ZeroClean`, `ZeroDirty`, `ResidentClean`, and `ResidentDirty` states.
2. Add validated range segmentation with checked offsets and no mutation on invalid ranges.
3. For zero states, fill only the requested destination span and allocate neither leaf nor committed memory for implicit clean-zero reads.
4. For resident states, pin the block while under its stripe, release the lock, copy bytes, reacquire, unpin, set reference bit, and wake pressure/destruction waiters as needed.
5. Check cancellation/deadline before each block segment. M1's `Result<void>` has no transferred-byte success field: if cancellation/error occurs after an earlier segment, return the error and never report success; an already copied destination prefix may remain.
6. Use stable arena addresses only after commit/state checks; tests must force concurrent state observation through seams.

**Verification:** `ctest ... -R "unit.m2.read"`.

**Acceptance:** zero and resident reads are byte-correct, bounded, lock-safe, and do not cause backing loads.

**Commit:** `feat(storage): implement zero and resident reads`

### Task 7: Add single-flight lazy backing load and hard-limit read-through

**Files:**

- Modify: `src/storage/dynamic_block_store_read.cpp`
- Modify: `src/storage/dynamic_block_store_internal.h`
- Modify: `tests/unit/dynamic_block_store_read_tests.cpp`
- Create/Modify: `tests/unit/dynamic_block_store_limit_tests.cpp`

**Steps:**

1. Write tests for one `Unresident` load, `AllZero -> ZeroClean`, data `-> ResidentClean`, captured `clean_generation`, same-block readers sharing one load, unrelated blocks progressing in parallel, cancellation while waiting, load failure rollback, commit failure rollback, close while loading, and an old-generation gated load returning after a successor is installed.
2. Under the stripe, elect one owner by transitioning `Unresident -> Loading`, recording prior state and a new owner epoch. Other callers wait in a predicate loop and re-evaluate cancellation/deadline.
3. Atomically capture the current `{generation, shared_ptr}` pair, release all metadata locks before backing I/O, and load into a private 64 KiB aligned buffer rather than the arena address.
4. Before publication, verify owner/epoch and captured generation. If the generation advanced, discard the private result, increment `stale_backing_load_retries`, and retry from the new view under cancellation/deadline bounds. Otherwise publish `ZeroClean` or `ResidentClean` with `clean_generation = captured_generation`; data publication still requires commit budget and copy-before-publish.
5. On any failure, restore the prior stable state only if owner/epoch still matches, save an error generation/result for waiters, wake them, and release pins/budget.
6. If commit budget cannot be obtained under the hard limit, satisfy a read from the private loaded buffer without caching, increment `uncached_reads_total`, publish `Unresident`, and wake waiters. Do not use read-through for writes.
7. Ensure cancellation of a waiter does not cancel the elected owner's useful load; cancellation of the owner follows the backing contract and restores state safely. A retained `shared_ptr` keeps the old backing alive until the in-flight call exits.

**Verification:**

```powershell
ctest --preset release --output-on-failure -R "unit.m2.(read|limits)"
```

**Acceptance:** at most one backing load occurs per simultaneous same-block miss and generation, no lock spans I/O, old-generation data cannot publish over a newer view, and every failure returns to a valid stable state.

**Commit:** `feat(storage): add lazy block loading and read through`

### Task 8: Implement WRITE with copy-on-load, versioning, and atomic block transitions

**Files:**

- Create: `src/storage/dynamic_block_store_write.cpp`
- Create: `tests/unit/dynamic_block_store_write_tests.cpp`
- Modify: `src/storage/dynamic_block_store_internal.h`
- Modify: storage/test CMake files

**Steps:**

1. Write failing tests for full-block and partial writes into every stable state, cross-block writes, identical-byte writes, write at end of device, invalid-range no-op, hard-limit exhaustion, commit failure, backing failure, cancellation, and content-version progression.
2. Full-block writes into `Unresident` or any zero state must not load backing. Obtain commit budget, commit a private/arena block safely, copy all 64 KiB, and publish `ResidentDirty` with one new content version.
3. Partial writes into `Unresident` must single-flight-load the old block into a private buffer, apply the requested bytes, obtain commit budget, commit/copy, and publish `ResidentDirty`. An `AllZero` result starts from a zeroed private buffer.
4. Partial writes into zero states create a zero-filled 64 KiB image before applying bytes. Partial writes into resident states pin and mutate under a per-block mutation protocol that prevents readers and snapshots from observing torn content.
5. A successful operation increments a block version exactly once per block whose logical content was changed and captures the currently installed generation as `dirty_base_generation`. If an identical-byte optimization is implemented, prove it is byte-exact; otherwise treating the write as a new dirty version is acceptable.
6. Never expose `ResidentDirty` until every destination byte is present in committed memory. On failure, either leave the prior logical content untouched or retain a fully valid dirty result and report the precise semantics; prefer copy-then-publish for strong per-block failure atomicity.
7. M1 does not promise device-level atomicity across blocks. Process blocks in ascending order; if a later block is cancelled or fails, already completed prefix blocks remain effective, but the `Result<void>` response is the precise error and must not report success.

**Verification:** `ctest ... -R "unit.m2.write"` plus ASan-equivalent/guard-page configuration if the repository provides it.

**Acceptance:** writes never resurrect backing bytes, never publish partial block images, and dirty/version metadata matches content.

**Commit:** `feat(storage): implement versioned dynamic writes`

### Task 9: Implement UNMAP and ZERO without backing resurrection

**Files:**

- Create: `src/storage/dynamic_block_store_ranges.cpp`
- Create: `tests/unit/dynamic_block_store_range_tests.cpp`
- Modify: `src/storage/dynamic_block_store_internal.h`
- Modify: storage/test CMake files

**Steps:**

1. Write a state-transition matrix for full/partial UNMAP and ZERO from all stable states, with zero backing and non-zero fake backing. Tests come from the matrix before implementation.
2. Every successful full-block operation avoids loading old bytes and publishes `ZeroDirty`: UNMAP uses `DirtyRecordKind::Discard`, ZERO uses `DirtyRecordKind::Zero`, including on a clean-zero/new-disk store. Each new version captures the currently installed generation as `dirty_base_generation`. This preserves the operation for M3 and follows the approved state machine.
3. Full-block zero/discard of resident memory must wait for pins, transition exclusively, decommit before publishing the zero override, and increment version. If decommit fails, retain the exact prior state, version, and bytes and return `DecommitFailed`.
4. Partial UNMAP/ZERO on an already-zero state is a no-op. Otherwise it is a read-modify-write operation: materialize the old image when necessary, zero only the target bytes, publish `ResidentDirty`, and represent it as `Data` because a partial block cannot be represented by a zero-only record.
5. Repeated full-block zero/discard must preserve or intentionally update the operation kind according to the latest caller; document and test the rule. Use the latest operation kind and increment version on each successful logical mutation request.
6. Invalid ranges, commit/decommit failures, backing failures, cancellation, and close races must preserve a valid previous or fully published state.
7. Verify a later read never reloads backing data for `ZeroDirty`, including after memory pressure and repeated reads.

**Verification:** `ctest ... -R "unit.m2.ranges"`.

**Acceptance:** block deallocation releases resident memory where possible and zero overrides are never lost or confused with clean backing.

**Commit:** `feat(storage): implement unmap and zero ranges`

### Task 10: Implement versioned dirty snapshots and persisted acknowledgement

**Files:**

- Create: `src/storage/dynamic_block_store_snapshot.cpp`
- Create: `src/storage/dynamic_block_store_backing.cpp`
- Create: `tests/unit/dynamic_block_store_backing_generation_tests.cpp`
- Create: `tests/unit/dynamic_block_store_snapshot_tests.cpp`
- Modify: `src/storage/dynamic_block_store.h`
- Modify: `src/storage/dynamic_block_store_internal.h`
- Modify: storage/test CMake files

**Steps:**

1. Write tests for empty scans, cursor completion/reset, byte/record caps, data/zero/discard records, exact `base_generation`, deterministic block order, partial final logical block, snapshot copy isolation, stale acknowledgements, uninstalled/same/older generations, exact generation acknowledgements, repeated scans, cancellation, and invalid limits.
2. Scan sparse metadata in ascending block index without materializing implicit leaves. Never hold the leaf-publication lock over the scan.
3. For `ResidentDirty`, acquire the block's mutation exclusion/pin, copy exactly 64 KiB into an owned vector, atomically capture version and `dirty_base_generation`, release, then append the record. Bytes beyond a partial logical last block must be deterministically zero.
4. For `ZeroDirty`, append no payload and preserve `Zero` versus `Discard` kind plus the matching `dirty_base_generation`.
5. Enforce both configured and per-call caps. A data record consumes 64 KiB; zero/discard records consume zero payload bytes but consume one record. Always make progress when a valid dirty record fits the record cap.
6. Implement coherent `backing_generation()` and `install_backing_view()`. Installation accepts only a non-null complete successor whose generation is strictly greater than current; same/lower/wrap/closed failures preserve the old pair and increment `backing_generation_conflicts` where applicable.
7. `mark_persisted` changes `ResidentDirty -> ResidentClean` or `ZeroDirty -> ZeroClean` only when dirty state, expected version, expected kind, `persisted_generation > dirty_base_generation`, and `persisted_generation <= backing_generation()` all hold. Success sets `clean_generation = persisted_generation`.
8. A version/kind mismatch returns `VersionChanged` and increments `snapshot_version_conflicts`; a generation-order mismatch returns `BackingGenerationConflict` and increments `backing_generation_conflicts`. Both leave state/content unchanged.
9. Exact acknowledgement must not race a later write into becoming falsely clean. Install the fake successor first, then lock and compare state/version/kind/generation and publish under one block mutation protocol. Add a negative test proving ack-before-install cannot clean.
10. Force `snapshot -> fake successor install -> exact ack -> eviction -> reload`; the reloaded bytes/zero must equal the acknowledged snapshot. This is the mandatory regression for old-backing resurrection.
11. Snapshot, installation, and acknowledgement failures/cancellation leave dirty state untouched and never claim durability.

**Verification:** `ctest --preset release --output-on-failure -R "unit.m2.(backing_generation|snapshot)"`.

**Acceptance:** M3 can copy bounded immutable dirty batches, install a complete successor generation, and acknowledge exact records without losing concurrent modifications or permitting post-eviction old-data resurrection.

**Commit:** `feat(storage): add versioned dirty snapshots`

### Task 11: Add clean CLOCK eviction and the soft-limit worker

**Files:**

- Create: `src/storage/eviction_manager.h`
- Create: `src/storage/eviction_manager.cpp`
- Create: `tests/unit/dynamic_block_store_eviction_tests.cpp`
- Modify: `src/storage/dynamic_block_store_core.cpp`
- Modify: `src/storage/dynamic_block_store_internal.h`
- Modify: storage/test CMake files

**Steps:**

1. Write tests for second-chance reference-bit behavior, only-clean eligibility, `clean_generation <= backing_generation()` eligibility, pinned/loading/dirty/zero skip behavior, decommit failure, progress to the low watermark, fair scan wraparound, wake/coalescing, close, and no busy loop when no victim exists.
2. Implement a bounded CLOCK/second-chance scan over existing sparse leaves. The cursor persists across passes; implicit leaves are skipped.
3. A candidate is only `ResidentClean` with zero pins, a clear reference bit, and `clean_generation <= backing_generation()`. A clean generation later than the current view is an invariant failure and must never be decommitted.
4. Transition the exact entry to `Evicting`, release metadata lock, decommit, reacquire, verify owner epoch, and publish `Unresident`. On decommit failure restore `ResidentClean`, record failure, and wake waiters.
5. Dirty, zero, loading, committing, and pinned blocks are never victims. A zero state already has no committed data and is not counted as eviction work.
6. Start one background worker after successful store construction. Crossing the soft limit signals it. It evicts until committed bytes are at or below the configured low watermark, no candidate exists, cancellation/close occurs, or a bounded no-progress pass completes.
7. Worker shutdown is deterministic and joined before arena release. Avoid polling sleeps; use condition variables and predicates.

**Verification:** `ctest ... -R "unit.m2.eviction"`.

**Acceptance:** clean memory is reclaimed only when a complete backing generation can reload it, dirty content is untouchable, and pressure with no eligible victim terminates without spin.

**Commit:** `feat(storage): add clean block eviction`

### Task 12: Enforce hard memory limits and complete failure rollback

**Files:**

- Modify: `src/storage/dynamic_block_store_core.cpp`
- Modify: `src/storage/dynamic_block_store_read.cpp`
- Modify: `src/storage/dynamic_block_store_write.cpp`
- Modify: `src/storage/eviction_manager.cpp`
- Create/Modify: `tests/unit/dynamic_block_store_limit_tests.cpp`

**Steps:**

1. Write deterministic tests with tiny block-count limits for exact soft/hard boundaries, simultaneous commit reservations, synchronous eviction, no-victim exhaustion, read-through, new-commit write rejection, overwrite of an already-resident block without a new commit, rollback after reserved-budget failures, and committed-byte counter accuracy.
2. Introduce commit-budget reservation distinct from actual committed bytes so concurrent committers cannot overrun the hard limit.
3. Before any new commit, reserve one block of budget. If the hard limit would be crossed, perform bounded synchronous clean eviction to the low watermark or until one block fits.
4. If no block fits, reads may use uncached read-through; operations requiring a new commit, including writes and partial zero/unmap of uncommitted content, must fail with `MemoryHardLimit` before changing logical content. An overwrite of an already-resident block that requires no new commit may continue.
5. Release reserved budget on every failure path. Convert reservation to committed accounting only after successful `VirtualAlloc(MEM_COMMIT)`.
6. Ensure decommit decrements committed accounting exactly once only after success. Add debug reconciliation against entry states and arena seam counters.
7. Complete counter updates for pressure events, failures, waits, cancellations, deadlines, and rollback paths.

**Verification:** `ctest ... -R "unit.m2.(limits|eviction|read|write|ranges)"`.

**Acceptance:** concurrent operations cannot exceed the configured hard limit by even one unreserved block, and all resource-failure paths preserve content and accounting.

**Commit:** `feat(storage): enforce bounded commit pressure`

### Task 13: Build a deterministic 100,000-operation reference-model test

**Files:**

- Create: `tests/support/reference_block_model.h`
- Create: `tests/unit/dynamic_block_store_model_tests.cpp`
- Modify: test CMake files

**Steps:**

1. Implement an independent byte-accurate model for a deliberately small logical disk. It may use ordinary vectors/maps but must not share production block math or transition code.
2. Generate a reproducible stream with a fixed printed seed containing aligned reads, writes, full/partial ZERO, full/partial UNMAP, dirty scans, immutable successor-view installation, correct/stale/uninstalled generation acknowledgements, pressure triggers, and explicit eviction/reload passes.
3. Include boundaries at block start/end, multi-block ranges, last sector, repeated same block, sparse distant blocks, and rejection of zero-length READ/WRITE.
4. After every operation, compare observable bytes/results with the model. At bounded intervals compare dirty-record base generation, installed generation, clean/dirty generation invariants, post-ack eviction/reload content, memory-limit accounting, and clean-eviction behavior.
5. Run at least 100,000 operations in Release as a normal M2-V2 test. Print seed, operation index, last operation, and compact state on failure so the exact run can be replayed.
6. Add a command-line/environment seed override for diagnosis without replacing the canonical fixed-seed run.

**Verification:** `ctest ... -R "unit.m2.model"` and confirm the result reports exactly 100,000 completed operations.

**Acceptance:** all data and dirty-state behavior matches an implementation-independent model for the entire deterministic run.

**Commit:** `test(storage): add deterministic block store model`

### Task 14: Prove single-flight loading and same-block concurrency

**Files:**

- Create: `tests/unit/dynamic_block_store_concurrency_tests.cpp`
- Modify: production storage files only for defects exposed by tests
- Modify: test CMake files

**Steps:**

1. Use barriers/latches and the fake backing gate, not timing sleeps, to force two or more readers onto one `Loading` block.
2. Prove exactly one backing call per simultaneous miss/generation, every reader receives identical bytes, cancellation of one waiter does not poison others, owner failure wakes all waiters with a stable retryable state, and a retry can succeed.
3. Gate an old-generation load, install a complete successor with different bytes, release the old call, and prove the old bytes are discarded, the load retries against the new view, `stale_backing_load_retries` increments, and the old object remains alive until its call exits.
4. Force read versus full write, read versus partial write, write versus write, full zero/unmap versus read, and close versus load on the same block.
5. Assert results conform to a valid serial order and no reader sees torn 64 KiB content.
6. Repeat with blocks mapping to the same stripe and different stripes to expose accidental global serialization.
7. Add a test-only bounded transition observer or hook only if necessary; compile it out or keep it private to test targets.

**Verification:** run `unit.m2.concurrency` repeatedly (at least 100 iterations for the deterministic race subset) without flakes.

**Acceptance:** same-block transitions are single-owner, waiters always wake, and unrelated blocks retain concurrency.

**Commit:** `test(storage): prove dynamic store transition concurrency`

### Task 15: Prove snapshot, mutation, eviction, cancellation, and close races

**Files:**

- Modify: `tests/unit/dynamic_block_store_concurrency_tests.cpp`
- Modify: production storage files only for defects exposed by tests

**Steps:**

1. Force snapshot-copy versus write and prove either old-version/old-bytes or new-version/new-bytes, never mixed bytes/version.
2. Force ack before successor installation and prove `BackingGenerationConflict`; then install the successor and force `mark_persisted(block, old_version, old_kind, generation)` after a later mutation to prove `VersionChanged`. In both cases the current block remains dirty.
3. Force `snapshot -> install -> exact ack` racing with a new write and prove only the exact version can become clean. Then force eviction/reload and compare bytes against the current logical version and installed view.
4. Force eviction selection versus pin acquisition and prove decommit cannot occur while a reader copies; force an impossible `clean_generation > backing_generation()` and prove fail-closed invariant handling rather than decommit.
5. Force soft-limit worker and synchronous hard-limit eviction to run together; prove budget and victim ownership cannot double-decommit or underflow counters.
6. Cancel or expire requests while waiting on loading, mutation, pressure, and shutdown predicates. Every wait must be bounded by notification/deadline and return the M1 error mapping.
7. Destroy a store with blocked backing, queued waiters, and an active worker using a cooperative fake that observes cancellation. Prove all threads join, old/new view references release, and the arena releases once.

**Verification:** `ctest ... -R "unit.m2.concurrency" --repeat until-fail:100` or the closest supported CTest syntax, with an outer timeout.

**Acceptance:** no race can install a torn view, acknowledge an uninstalled/older generation, mark newer data clean, resurrect old backing bytes after eviction, decommit pinned data, leak a waiter, or hang shutdown.

**Commit:** `test(storage): cover snapshot eviction and shutdown races`

### Task 16: Add long stress, invariant audits, and resource accounting

**Files:**

- Modify: `tests/unit/dynamic_block_store_concurrency_tests.cpp`
- Create/Modify: stress test source according to existing test layout
- Modify: test CMake files

**Steps:**

1. Run a fixed-duration or fixed-operation multi-thread workload with readers, writers, zero/unmap callers, dirty scanners, immutable fake successor installers, generation-bound acknowledgers, and forced memory pressure across sparse blocks.
2. Use a separately synchronized shadow/model for sampled ranges or periodic quiescent full comparison; never accept a crash-only stress test as correctness evidence.
3. At quiescent checkpoints, call debug invariant validation and reconcile committed bytes, pins, transitions, dirty counts, installed generation, every block's clean/dirty generation, backing object lifetime, and VM fake allocations.
4. Capture process handle count, thread count, private bytes, and committed arena bytes before/after repeated store construction/destruction. Define zero leaked store threads/reservations/handles as the pass criterion.
5. Apply finite test timeouts and dump reproducible seed/counters on failure.

**Verification:** the full `m2-v3` label passes twice consecutively in Release.

**Acceptance:** bounded stress preserves model consistency, invariants, memory limits, and baseline resources.

**Commit:** `test(storage): add concurrent stress and resource audits`

### Task 17: Inject DynamicBlockStore into the M1 proxy and local shared-memory path

**Files:**

- Create: `src/m2/m2_provider.h`
- Create: `src/m2/m2_provider.cpp`
- Create: `tests/unit/m2_provider_tests.cpp`
- Create: `tests/integration/m2_shm_proxy_tests.cpp`
- Modify: M2/test CMake files

**Steps:**

1. Write provider lifecycle tests against fake proxy/transport/device factories before implementation.
2. Implement `M2Provider` as composition: construct the store, inject it as `IBlockDevice` into the accepted M1 proxy session, start transport, expose ready only after both are valid, then stop transport before destroying storage.
3. Keep ownership explicit with one lifecycle owner. No detached threads, global singleton store, or proxy downcast to storage internals.
4. Re-run all M1 protocol and transport tests unchanged.
5. Add local shared-memory integration for cross-block read/write/zero/unmap, cancellation, malformed requests, provider disconnect, and restart using `DynamicBlockStore` with `ZeroBackingStore`.
6. Validate protocol-visible status and byte counts, then inspect store stats only from the test owner for corroboration.

**Verification:** `ctest ... -L m2-v4` plus the entire required M1 local suite.

**Acceptance:** M2 is a replaceable `IBlockDevice`; M1 proxy code contains no dynamic-store knowledge and all M1 regressions pass.

**Commit:** `feat(m2): connect dynamic store to proxy provider`

### Task 18: Add real VirtualAlloc integration for 64 GiB and 1 TiB stores

**Files:**

- Create: `tests/integration/m2_virtual_memory_tests.cpp`
- Modify: test CMake files

**Steps:**

1. In a 64-bit Release test process, construct empty 64 GiB and 1 TiB clean-zero stores using the real Win32 VM API.
2. Capture process commit/private working metrics and store stats before construction, after construction, after sparse block writes, after reads, after unmap/eviction, and after destruction.
3. Prove reservation alone does not make pages readable/writable and does not commit proportional physical memory.
4. For the 64 GiB store, assert empty Ready-provider committed memory is at most 128 MiB including fixed metadata as measured by the acceptance method.
5. Write 1 GiB of unique blocks and assert added commit is at most 1.10 GiB plus the fixed metadata delta already measured at construction.
6. Unmap/clean-evict 1 GiB as appropriate and assert at least 90% of that resident data is released within 10 seconds.
7. Destruction must return reservation/commit/handle/thread counts to the platform-tolerant recorded baseline; document any allocator noise allowance separately from arena stats.

**Verification:** `ctest ... -R "integration.m2.virtual_memory"` and M2-V2.

**Acceptance:** dynamic allocation is demonstrated using both store accounting and OS process measurements, not inferred from code structure.

**Commit:** `test(m2): validate large virtual memory behavior`

### Task 19: Implement the M2 CLI, lifecycle runner, report schema, and performance probe

**Files:**

- Create: `src/m2/m2_performance_probe.h`
- Create: `src/m2/m2_performance_probe.cpp`
- Create: `src/m2/m2_volume_workload.h`
- Create: `src/m2/m2_volume_workload.cpp`
- Create: `src/m2/m2_lifecycle_runner.h`
- Create: `src/m2/m2_lifecycle_runner.cpp`
- Create: `src/m2/m2_report.h`
- Create: `src/m2/m2_report.cpp`
- Create: `src/m2/m2_cli.h`
- Create: `src/m2/m2_cli.cpp`
- Create: `src/m2/main.cpp`
- Create: `tests/unit/m2_lifecycle_runner_tests.cpp`
- Create: `tests/unit/m2_report_tests.cpp`
- Create: `tests/unit/m2_performance_probe_tests.cpp`
- Modify: M2/test CMake files

**Steps:**

1. Define a versioned M2 JSON schema before implementation. Required sections: run identity, build/toolchain, M1 baseline, machine/OS, driver/service baseline, options/geometry, initial/current backing generation, view-install and stale-load counters, phase results, checksums, storage stats, OS memory samples, performance samples/ratios, device identity, cleanup actions, final baseline comparison, failures, and verdict.
2. Write report tests for successful serialization, every required field, stable enum strings, integer width, unique output path, atomic temp-to-final publication, and explicit incomplete/failure verdicts.
3. Define an injected `IM2VolumeWorkload` with phase methods so lifecycle behavior is unit-testable without PowerShell or disks.
4. Implement lifecycle order with rollback guards registered immediately after every mutation: capture baseline; construct provider with explicit generation `0`/backing policy; create exact device; wait for one matching disk; run setup/write; sample stats; run delete/retrim; sample; run performance; remove mounts; remove exact device; stop provider; restore service; compare baseline; publish report. Real M2 mode must report that no durable backing generation was installed.
5. Every phase has a finite deadline and cooperative cancellation. Cleanup runs after any failure and records each attempted action/result. Cleanup failure forces final `NO-GO`.
6. Implement a raw QD1 performance probe reusable against the M1 fixed provider baseline and the M2 dynamic device: one 32 MiB aligned sequential region and 10,000 fixed-seed aligned 4 KiB random operations, with a documented warmup excluded from timing.
7. Compute ratios only from same-machine, same-build, same-process-policy measurements. Required thresholds: sequential throughput at least 70% of M0/M1 fixed baseline and 4 KiB random IOPS at least 60%.
8. Add CLI modes for schema/help, local fake lifecycle, real acceptance, and report validation inputs. Reject unknown/destructive defaults. Real mode requires explicit evidence directory, expected driver identity, exact 64 GiB logical size, all store limits/options (or one explicit test-policy flag that derives and reports the SPEC's 25%/50% test limits), and an elevated-safety acknowledgement flag used only after script preflight.

**Verification:**

```powershell
ctest --preset release --output-on-failure -R "unit.m2.(lifecycle|report|performance)"
<release-m2-exe> --help
<release-m2-exe> --print-schema
<release-m2-exe> --local-fake --evidence-dir <temp-dir>
```

**Acceptance:** lifecycle and reporting are fully testable with fakes, generation fields and “no durable backing in M2” semantics are explicit, all rollback paths are explicit, and performance methodology is deterministic.

**Commit:** `feat(m2): add acceptance lifecycle and evidence model`

### Task 20: Build a fail-closed elevated NTFS workload and verification orchestrator

**Files:**

- Create: `tests/e2e/Invoke-M2NtfsWorkload.ps1`
- Create: `tests/e2e/run-m2-dynamic-disk.ps1`
- Create: `tests/e2e/test-m2-harness-preflight.ps1`
- Create: `tools/Verify-M2Json.ps1`
- Create: `tools/Invoke-M2Verification.ps1`

**Steps:**

1. Write Pester or script-level preflight tests using mocked inventory before any real execution path. Cover non-admin, missing binary/driver, extra candidate disk, disk 0, boot/system disk, wrong size, pre-existing mount collision, missing output directory, and unexpected service baseline.
2. The outer orchestrator captures baseline physical disks, volumes, mount points, service state/start type, process resources, and existing M2 devices before starting the provider.
3. After provider readiness, identify the new disk by set difference plus expected bus/vendor/product/device evidence. Require exactly one match, exact 64 GiB size, not disk 0, not boot/system/pagefile/clustered, no pre-existing partitions, and exact provider-created device identity. Any mismatch aborts before initialization.
4. Prove the real M2 process does not load or depend on `aimapi.dll`, `aim_ll`, AIM GUI, or RamDyn. Only this project's binaries and the accepted `phdskmnt.sys` driver are permitted in the device path.
5. Keep the C++ provider/lifecycle process alive while the injected PowerShell workload executes. Do not create a provider and exit before NTFS operations.
6. Setup/write phase: initialize GPT, create one partition, quick-format NTFS, mount at a run-unique empty directory, verify normal file operations, write deterministic files totaling at least 4 GiB, flush/close, read back and verify hashes, modify data, rename files/directories, and verify again.
7. Delete/ReTrim phase: query that NTFS DeleteNotify is enabled, delete a deterministic subset large enough to cause useful trim, flush, invoke `Optimize-Volume -ReTrim` or the accepted equivalent, and wait for storage counters to observe UNMAP with a finite deadline.
8. Cleanup phase: close handles, remove mount/drive letter, offline/clear only if required, remove the exact SCSI device through M1 ownership, stop provider, restore the exact prior service state/start type, and compare complete baseline inventory.
9. `Verify-M2Json.ps1` validates schema and semantic gates, including 4 GiB minimum, hash equality, UNMAP count/bytes, memory thresholds, performance thresholds, 20 cycles for V7, and cleanup baseline. Missing data is failure, never skip.
10. `Invoke-M2Verification.ps1` maps tier names to commands, creates unique timestamp/GUID evidence directories, logs stdout/stderr/exit code, verifies JSON, and emits one terminal verdict.
11. Include `-WhatIf`/preflight-only mode that performs no disk/service mutation and is a mandatory local test.

**Verification:** run all preflight tests and `Invoke-M2Verification.ps1 -Tier M2-V5 -WhatIf`; inspect that no baseline mutation occurred.

**Acceptance:** the harness cannot initialize, format, mount, or remove a disk unless exact identity and safety predicates all hold, and every mutation has a tested rollback.

**Commit:** `test(m2): add safe NTFS acceptance harness`

### Task 21: Execute M2-A, M2-B, and the real M2-C functional gate

**Files:**

- Modify only defects uncovered by tests, with matching regression tests first
- Append evidence/results: `docs/validation/m2-dynamic-block-store-acceptance.md`

**Steps:**

1. Run M2-V1 and record that all core unit tests pass. This is **M2-A** only when state, model-independent operation tests, snapshot behavior, and limits are all included.
2. Run M2-V2. Preserve the 100,000-operation seed/result and real 64 GiB/1 TiB VM samples.
3. Run M2-V3 twice. This is **M2-B** only when concurrency, cancellation/deadline, pressure, shutdown, invariant, and resource checks all pass without retrying away a failure.
4. Run M2-V4 and all M1 local regression tests. Stop before the driver if any fail.
5. Run M2-V5 once on the exact real device. Preserve full logs, report, hashes, storage counters, disk/volume identity, and before/after inventory. This proves the functional and cleanup portion of **M2-C**; M2-C cannot be marked PASS until Task 22 also passes the resource and performance requirements.
6. If a correctness, hang, dirty-loss, backing-resurrection, wrong-device, or cleanup defect appears, stop the gate, retain evidence, add a deterministic lower-level regression where possible, fix, and restart the affected gate from its beginning.
7. Never edit JSON evidence to turn failure into success. New attempts produce new evidence directories linked from the acceptance document.

**Acceptance:** M2-A and M2-B have reproducible passing evidence, and M2-C has passing functional/cleanup evidence with no hidden skip/retry. M2-C remains pending Task 22.

**Commit:** `test(m2): complete core concurrency and ntfs gates`

### Task 22: Measure performance/fault behavior, run the 20-cycle soak, and issue the verdict

**Files:**

- Modify only defects uncovered by tests, with regression tests first
- Finalize: `docs/validation/m2-dynamic-block-store-acceptance.md`

**Steps:**

1. Run M2-V6. Capture empty-provider memory, 1 GiB resident growth, 1 GiB release within 10 seconds, fixed-provider baseline metrics, dynamic-provider metrics, ratios, and injected failure cleanup.
2. Required memory gates:
   - empty 64 GiB Ready provider committed memory `<= 128 MiB`;
   - writing 1 GiB adds `<= 1.10 GiB + measured fixed metadata`;
   - releasing/unmapping 1 GiB reclaims `>= 90%` within 10 seconds.
3. Required performance gates:
   - sequential throughput `>= 70%` of the accepted M0/M1 fixed-provider baseline;
   - 4 KiB random QD1 IOPS `>= 60%` of that baseline;
   - request-path allocation metrics/tests show no unbounded per-request allocation.
4. Run M2-V7 in Release: 20 complete create/read/write/unmap/delete/cleanup cycles. The soak may use a 64 MiB deterministic data region per cycle; the separate M2-V5 run remains responsible for the at-least-4-GiB NTFS workload.
5. Compare process handle/thread/private-byte and service/device/disk/volume baselines after every cycle. Zero leaked provider devices, store threads, arena reservations, mount points, and service-state changes are allowed.
6. Run the final complete M1 regression suite after the soak.
7. When the functional/cleanup evidence from Task 21 and all resource/performance evidence in this task pass, mark **M2-C PASS**. Fill every acceptance-document field with command, exit code, evidence path, observed value, threshold, pass/fail, and defect/commit links. State exactly one verdict: `M2 GO` or `M2 NO-GO`.
8. `M2 GO` requires every gate. A performance-only miss is still `M2 NO-GO` with root-cause analysis. Data corruption, cleanup ambiguity, dirty-data loss, or backing resurrection is an immediate stop condition and cannot be waived.
9. Record the stable M3 handoff: `IBlockDevice`, immutable `IBackingStore`, coherent `BackingView`, dirty snapshot records/cursor with base generation, install-before-ack ordering, exact version/kind/generation acknowledgement, stats, and limits. Do not begin M3 implementation in this task.

**Verification:** run M2-V6, M2-V7, `Verify-M2Json.ps1` over all canonical reports, then `git diff --check` and inspect final worktree status.

**Acceptance:** the report says `M2 GO` only if all functional, memory, performance, resource, cleanup, and regression criteria have fresh evidence.

**Commit:** `docs(m2): issue dynamic block store acceptance verdict`

## 5. State-transition acceptance table

The implementation may use additional private sub-states, but these externally meaningful transitions must hold:

| Operation | Prior stable state | Required successful result |
|---|---|---|
| Read | Clean-zero implicit / `ZeroClean` / `ZeroDirty` | Return zeros; no backing load; no commit required |
| Read | `Unresident` + backing `AllZero` | Return zeros; publish `ZeroClean` with captured `clean_generation` |
| Read | `Unresident` + backing data | Return data; publish `ResidentClean` with captured `clean_generation`, or read-through and return to `Unresident` under hard pressure |
| Read publication | Captured load generation is older than current view | Discard old result, increment retry counter, reload from current view |
| Read | `ResidentClean` / `ResidentDirty` | Pin, copy coherent bytes, set reference bit, preserve state |
| Full write | Any state | Publish complete `ResidentDirty` image and new version; no backing load |
| Partial write | `Unresident` | Load old image once, patch, publish `ResidentDirty` and new version |
| Partial write | Zero state | Start from zeros, patch, publish `ResidentDirty` and new version |
| Full ZERO | Any stable state | Publish `ZeroDirty(Zero)` and new version; decommit old resident data safely |
| Full UNMAP | Any stable state | Publish `ZeroDirty(Discard)` and new version; do not allow backing resurrection |
| Partial ZERO/UNMAP | Existing zero state | No-op; remain zero without commit/backing load |
| Partial ZERO/UNMAP | Unresident or resident state | Read-modify-write, publish `ResidentDirty` full image and new version |
| Install view | Non-null complete successor, generation greater than current | Atomically replace pointer/generation; old object lives through retained readers |
| Invalid view install | Null, same/lower/wrapped generation or closed Store | Return error; preserve current pointer/generation |
| Exact persisted ack | Dirty state/version/kind equal, generation installed and later than dirty base | Transition to Clean and set `clean_generation = persisted_generation` |
| Stale persisted ack | Dirty state, version or kind differs | Return `VersionChanged`; no state/content change; conflict counter increments |
| Invalid-generation ack | Generation uninstalled or not later than dirty base | Return `BackingGenerationConflict`; remain Dirty |
| Evict | `ResidentClean`, unpinned, second chance cleared, clean generation installed | Decommit then publish `Unresident` |
| Evict | Dirty/zero/transient/pinned | Skip; no state/content/memory change |

Any transient failure returns to the recorded prior stable state unless a complete newer logical state was already atomically published. All waiters are notified on every transient exit.

### 5.1 Generation-safety coverage matrix

| Required invariant | Implementation task | Mandatory red/acceptance test | Tier |
|---|---:|---|---|
| Published `IBackingStore` content is immutable and old object lifetime is retained | 2 | const interface, new object per generation, in-flight old-view lifetime | M2-V1 |
| Pointer and generation publish as one coherent pair and never regress | 5, 10 | concurrent readers see only old pair or new pair; null/same/lower/wrap fail atomically | M2-V1/V3 |
| Old-generation load cannot publish after a newer view is visible | 7, 14 | gate G load, install G+1 with different data, release G, require retry/G+1 bytes | M2-V1/V3 |
| Every mutation records the installed generation it is dirty against | 8, 9 | WRITE/ZERO/UNMAP version and `dirty_base_generation` publish together | M2-V1 |
| Dirty cannot become Clean before its successor generation is installed | 10, 15 | ack-before-install and generation-not-later-than-base both fail and preserve Dirty | M2-V1/V3 |
| Concurrent mutation cannot be cleared by an older snapshot | 10, 15 | install old snapshot, mutate, old version/kind ack returns `VersionChanged` | M2-V1/V3 |
| Clean eviction always has a complete reload source | 10, 11, 13, 15 | snapshot/install/ack/evict/reload returns acknowledged Data/Zero, never predecessor data | M2-V1/V2/V3 |
| Evidence reports generation behavior without claiming M2 durability | 19, 22 | schema requires generation/counters and explicitly states no durable successor in real M2 run | M2-V4/V7 |

## 6. Required test inventory and stable names

Register stable test names so automation and future agents do not guess:

```text
unit.m2.block_math
unit.m2.backing_store
unit.m2.virtual_memory
unit.m2.block_directory
unit.m2.store_core
unit.m2.read
unit.m2.write
unit.m2.ranges
unit.m2.backing_generation
unit.m2.snapshot
unit.m2.eviction
unit.m2.limits
unit.m2.model
unit.m2.concurrency
unit.m2.provider
unit.m2.lifecycle
unit.m2.report
unit.m2.performance
integration.m2.virtual_memory
integration.m2.shm
script.m2.harness_preflight
```

If parameterized tests create suffixes, preserve these strings as prefixes. Assign every test to at least one verification-tier label and make V1-V4 selection deterministic.

## 7. Stop conditions and defect protocol

Stop immediately and preserve evidence when any of the following occurs:

- byte mismatch, torn read, checksum mismatch, or non-serializable race;
- dirty block evicted, dirty version cleaned by stale/uninstalled-generation acknowledgement, old-generation load published after a view switch, post-ack eviction reloading old data, or zero override reloading backing data;
- wrong/ambiguous physical disk selection or cleanup against anything except the exact created device;
- service, disk, volume, mount, thread, handle, or reservation baseline cannot be restored;
- transition waiter hangs or shutdown exceeds its deadline;
- committed accounting exceeds hard limit without a matching reserved budget;
- evidence is missing, overwritten, internally inconsistent, or manually altered.

For each defect:

1. mark the current task blocked in `M2_TASKLIST.md`;
2. retain the failing seed, command, output, report, and environment;
3. identify the smallest failing layer;
4. write a deterministic failing test there before modifying production code;
5. implement the smallest in-scope fix;
6. run the focused test, the task tier, and every earlier affected tier;
7. append the attempt and commit to the acceptance document;
8. resume the task only after evidence passes.

Do not lower thresholds, add retries, ignore status codes, broaden cleanup targets, or relabel a skip as a pass.

## 8. Definition of done

M2 is complete only when all statements below are true:

- M1 remains accepted and its required regression suite passes on the final M2 commit.
- A 64 GiB and 1 TiB store reserve address space without proportional committed memory or per-block metadata.
- Correct reads/writes/ZERO/UNMAP operate through the M1 proxy and real NTFS.
- Backing data loads on first access only, same-block misses are single-flight, and all-zero loads commit no data pages.
- clean data is evictable; dirty data and zero overrides cannot be lost or resurrected from backing.
- soft and hard limits, read-through, cancellation, deadlines, faults, shutdown, and resource accounting are proven.
- versioned dirty snapshots and exact persisted acknowledgements are race-safe and ready for M3 without contract redesign.
- immutable successor views install atomically; ack-before-install fails; ack-then-evict/reload returns the acknowledged bytes/zero; old-generation loads cannot publish after a newer view is visible.
- the deterministic 100,000-operation model, concurrency/resource gate, real 4 GiB NTFS gate, memory/performance gates, and 20-cycle soak all pass in Release.
- exact device cleanup and service/baseline restoration succeed on every canonical run.
- `docs/validation/m2-dynamic-block-store-acceptance.md` contains fresh, machine-readable evidence and the verdict `M2 GO`.

Until then, M2 is in progress or `M2 NO-GO`; completion of source code alone is not completion of the milestone.
