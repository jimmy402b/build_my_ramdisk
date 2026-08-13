# Deferred work: online compaction

> Status: DEFERRED OUTSIDE M4 V1
> Recorded: 2026-08-13
> Source decision: R1 in the contracted M4 product milestone

## Scope

This item covers compaction of a bundle while `myrd run` remains online. It is
not part of M4 v1, is not an M5 milestone, and does not block M4 v1 GO. M4 v1
ships the supported offline command:

```text
myrd compact --bundle <path>
```

No placeholder online-compaction interface, option or scheduler hook may be
added to v1 for this deferred work.

## R1 result and root cause

The bounded R1 spike reached a prepared compaction transaction but could not
finish it through the live provider without one of two prohibited changes:

1. copy the M3 manifest mutation, publication-disposition, commit-resolution
   and reconciliation sequence into the provider; or
2. extract a shared M3 transaction-finalization path used by both the offline
   lifecycle runner and live provider.

`PreparedCheckpointTransaction` exposes publication, resolution and
reconciliation as separate operations. The existing provider resume path begins
after manifest publication and therefore cannot safely finish a freshly
prepared compaction transaction. Copying the sequence would create a second
durability implementation; extracting it is an independent M3 refactor beyond
the bounded M4 spike. The experiment was removed and retained no production
delta.

## Conditions for reconsideration

Online compaction may be reconsidered only as an independently approved M3
change that includes:

- a design for one shared transaction-finalization owner;
- unchanged M3 commit dispositions and recovery semantics;
- checkpoint and compaction fault coverage for publication, resolution and
  reconciliation;
- live-provider compaction and offline lifecycle tests using the same
  finalization path;
- full M3 persistence regression and real recovery evidence;
- a separate product decision about whether its benefit justifies the added
  persistence-core risk.

Until those conditions are met, M4 v1 monitors journal capacity, warns at 80%
of the fixed 2048 MiB ceiling, and directs the operator to the offline product
command.
