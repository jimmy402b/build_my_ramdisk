# M3 persistence operator contract

This document describes the operational boundary accepted by M3. It is not a
service or GUI manual. M4 will add scheduling, policy and product lifecycle
orchestration on top of these contracts.

## Durability promise

Normal writes complete after reaching the RAM-backed dynamic block store. They
are not individually write-through to the image bundle. After the target
Windows volume has been flushed, a successful checkpoint captures one logical
cut and publishes one complete successor generation. A process or machine
failure may lose every change after the most recent successful checkpoint, but
recovery must select either the complete old generation or the complete new
generation and must never expose a mixed generation.

M3 provides explicit checkpoint and compaction operations. It does not provide
a timer, Windows service, startup recovery policy, shutdown checkpoint policy,
automatic compaction or GUI. Those are M4 responsibilities.

## Supported VHDX profile

M3 creates and accepts the frozen project profile:

- dynamic, non-differencing VHDX;
- clean/inactive VHDX log;
- 2 MiB payload blocks;
- 512-byte or 4096-byte logical sectors and supported physical-sector geometry;
- structurally valid duplicate headers, region tables, metadata and BAT;
- project-created sparse payload allocation inside a normal non-sparse
  container file;
- read-only Windows native attach for compatibility verification.

M3 rejects fixed or differencing disks, parent chains, active logs, unknown
mandatory metadata, unsupported geometry, malformed/overlapping regions,
invalid checksums and identities, and compressed, encrypted or filesystem-
sparse container files. Arbitrary third-party VHDX import compatibility is not
an M3 claim. A rejected profile is reported as `UnsupportedVhdx` or a more
specific corruption/format error; it is never silently interpreted as zeroes.

## Bundle contents and ownership

A bundle is one validated directory containing only safe single-component
names. The initial local lifecycle uses:

- the operator-selected manifest name, an exactly 8192-byte dual-slot commit
  selector;
- `base-0000.vhdx`, the generation-zero immutable base;
- `journal-e0001.rdj`, the append-only journal for committed deltas;
- temporary create/compaction files while an operation is in progress;
- later immutable `base-g0000-c<generation>.vhdx` and
  `journal-e0002-c<generation>.rdj` files produced by compaction.

The manifest, referenced bases and referenced journals form one unit. Do not
rename, edit, replace, deduplicate or delete individual files. Writer ownership
is enforced by validated file identities and a cross-process writer lock.
Cleanup may delete an artifact only after proving it is unreferenced by both
manifest slots, live backing views and in-flight recovery/reconciliation state.

## Create, open and lazy loading

Generation-zero creation publishes the base, journal and manifest only after
their exact writes and flushes succeed. Opening selects the newest complete
valid manifest slot, validates all referenced identities and builds an
immutable backing view. Logical capacity does not imply equal committed RAM:
clean blocks are read from the journal-first composite backing store only on
first access and may later be evicted under the configured memory limits.

The engineering CLI exposes `create`, `inspect`, `verify`, `checkpoint`,
`compact`, `recover-test` and `fault-matrix`; run `myrd-m3-e2e --help` for the
mandatory finite limits. The real AIM runtime accepts `status`,
`checkpoint volume-flushed`, `checkpoint raw-test` and `stop` on standard
input. `raw-test` is a test-only durability boundary and cannot support a
filesystem durability claim.

## Checkpoint procedure

For an NTFS-backed device, an orchestrator must:

1. stop or coordinate external mutations as required by its policy;
2. call `FlushFileBuffers()` on the target Windows volume and wait for success;
3. request a checkpoint with the `WindowsVolumeFlushed` barrier;
4. inspect the structured commit disposition;
5. if required, finish commit resolution and in-memory reconciliation before
   starting another checkpoint;
6. record the committed generation and retain the complete bundle.

The existing IMDPROXY wire protocol has no independent flush request. M3 does
not invent one. The volume flush covers data that has reached the Windows
filesystem boundary; M2.1 snapshot/COW semantics provide the subsequent
consistent logical cut.

## Commit outcomes and recovery action

| Disposition | Meaning | Required action |
|---|---|---|
| `NotCommitted` | Publication was not attempted or the old generation was proven intact. | Keep serving the old durable generation; fix the reported cause before retrying. |
| `CommitIndeterminate` | Manifest publication was attempted but its durable result is not yet known. | Do not start another checkpoint, compact, or delete files. Run commit resolution/recovery against the same bundle. |
| `CommittedNeedsReconciliation` | The new generation is durably proven, but the live RAM view/dirty acknowledgement is not yet reconciled. | Reinstall the exact successor backing view and reconcile exact epoch/version/kind before another persistence mutation. |
| `Committed` | The new generation is durable and the live state has been reconciled. | Record success; this generation is the new recovery point. |

Corrupt manifest, journal or backing data fails closed. Recovery never converts
corruption to absent/zero data and never falls back block-by-block to an older
generation. Use `inspect` for slot diagnostics and `verify --full` for complete
content verification before reopening a suspect or restored bundle.

## Compaction

Compaction reads one immutable committed backing view, creates a new sparse
dynamic VHDX plus a fresh journal, verifies the artifacts offline, then
publishes them as one new checkpoint generation. It does not rewrite the
currently referenced files in place. M3 exposes the mechanism and policy
metrics; M4 decides when to invoke it.

## Backup and restore

M3 accepts only an offline whole-bundle backup procedure:

1. stop new mutations and persistence operations;
2. resolve any `CommitIndeterminate` or
   `CommittedNeedsReconciliation` state;
3. close the provider/runtime and release the bundle writer lock;
4. copy the entire bundle directory, preserving every file byte and name;
5. hash the copied files and retain the source manifest name and geometry.

Do not copy only the active manifest slot's files while the provider is live;
publication or retention may race the copy. M3 does not claim VSS/application-
consistent online backup.

Restore into a new empty, non-reparse directory on a supported local Windows
filesystem. Do not merge a backup into an existing bundle. Run read-only
`inspect`, then `verify --full`; only after both pass may the provider create a
new AIM device from the restored bundle. If verification fails, preserve the
bundle unchanged for diagnosis.

## Shutdown and abnormal termination

`stop` rejects new external mutations, drains active requests, closes the
provider and device in dependency order, and releases validated file and writer
capabilities. Closing does not automatically create a checkpoint. An M4
shutdown policy must flush the Windows volume, checkpoint, resolve/reconcile the
outcome, then stop; if that sequence cannot finish, recovery is limited to the
previous successful checkpoint.

After process termination or power loss, recovery begins read-only, validates
both manifest slots and all referenced immutable artifacts, and selects the
newest complete generation. The real Task42 matrix verifies eight process-kill
and two controlled-power-cut scenarios.

## Known limitations

- no periodic write-back, Windows service, startup/shutdown scheduler or GUI;
- no guarantee for writes newer than the last successful checkpoint;
- no per-write synchronous persistence;
- no VSS provider or application-consistent database snapshot;
- no encryption, compression, deduplication, remote replication or parent VHDX
  chain;
- no authorization to redistribute `phdskmnt.sys` or AIM Toolkit binaries;
- the current engineering CLI/harness requires explicit finite safety limits
  and is not the final personal-product interface.

Formal environment and evidence commands are documented in
`docs/testing/m3-e2e.md`. The contract-to-evidence audit is in
`docs/validation/m3-final-audit.md`.
