# M4 Negative Optional-Field Repair Mini-Plan

## Frozen failure

Formal12 stopped in the sole `missing_driver` Negative invocation after the
second hard reset. The lifecycle runner returned child exit 1 under
`Set-StrictMode -Version Latest` because the shared pre-switch setup read
`$entry.expected_size` even though the valid `missing_driver` manifest entry
does not define that optional property. No missing-driver evidence, aggregate
Negative evidence, R7 closure, or R8 work was accepted. The Formal12 root and
all prior immutable evidence remain preserved.

The same defect also affects the valid `unknown_nonempty` and
`occupied_letter` entries. This is a harness defect, not a product refusal or
disk-layout defect.

## Exact repair scope

Only these tracked files may change:

- `tests/e2e/Invoke-M4RunLifecycle.ps1`
- `M4_TASKLIST.md`
- this mini-plan

No product C++, provisioning script, verifier, fixture schema, scenario list,
VM disk, driver installation, or reset policy changes are allowed.

## TDD sequence

1. Extend the existing `MYRD_M4_NEGATIVE_SELFTEST` production-seam test so a
   valid entry without `expected_size` must resolve to the frozen `64M`
   fallback, while a valid sized entry must preserve its explicit value.
2. Run the registered negative-contract selftest and preserve the RED caused
   by the missing resolver/unsafe production seam.
3. Add one small resolver that checks
   `PSObject.Properties['expected_size']` before reading the value, and use it
   at the single shared pre-switch size assignment.
4. Run the focused negative-contract test, Debug and Release `m4-v3`, the full
   sequential local verifier, PowerShell parsing, line count, and diff checks.
5. Obtain fresh specification and quality approval. Commit only the frozen
   repair scope.

## Formal13 boundary

After local qualification, build and freeze a new byte-exact candidate. Do not
reuse the Formal12 root. Formal13 is the single owner-approved full rerun and
retains the same eight cases, two lifecycle hard resets, disk prohibitions,
and fail-closed gates. It may begin only after the operator restores a clean
VM baseline in which `phdskmnt` is `DEMAND_START`/Running and the query-only
DirectAim helper returns `ids=[]`. No third Formal12 reset, alternate loader,
driver reinstall, registry edit, or scenario expansion is authorized.
