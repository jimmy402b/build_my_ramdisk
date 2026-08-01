# M0 Named-object Release Race Design

## Problem

After the clean-slate branch was merged into `master`, a fresh V6 run failed on
Release cycle 9 because `named_objects_released` was false. The disk had already
disappeared, both AIM and PhysicalDrive baselines matched, and a later probe
showed all four session objects gone. The failure is therefore a bounded kernel
handle-release race, not persistent residue.

The current lifecycle performs one immediate probe after stopping the provider.
That single observation is too early on some PnP teardown schedules.

## Required behavior

- Keep the per-cycle zero-residue requirement. A timeout is still a hard
  cleanup failure.
- After the provider stops, poll the mapping, Request event, Response event and
  Server mutex until all are absent or 15 seconds elapse.
- Poll every 100 milliseconds; do not use a fixed unconditional sleep.
- Treat `ERROR_FILE_NOT_FOUND` as absence and an opened object as "not released
  yet".
- Return unexpected Win32 probe errors through `Result<bool>` and preserve the
  error code in `cleanup_errors`.
- Use the same wait in normal and failure-injection cleanup paths.

## Testing

- The existing red regression test makes the first probe return false and the
  second return true. It must become green and prove at least two probes ran.
- Run the complete lifecycle unit suite in Debug and Release.
- Run full Release V6 on `master`; acceptance requires a new 20/20 aggregate,
  restored services and no named objects that can be reopened.

## Non-goals

- Do not remove or weaken named-object validation.
- Do not change DeviceId ownership, disk disappearance, service restoration or
  report immutability behavior.
- Do not begin M1-M4 work as part of this fix.
