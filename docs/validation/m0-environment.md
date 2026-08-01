# M0 Environment Ledger

- Normative SPEC: `docs/superpowers/specs/2026-08-01-m0-driver-only-operational-spec.md`
- Agent plan: `docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md`
- AIM source commit: `d379caa027d21902e45fce8c9daf02fd116c2dda`
- Working branch: `rewrite/driver-only`
- Real-driver tests authorized only after V0-V3 pass: yes
- Toolkit uninstall authorized: no

## Machine Facts (2026-08-01)

| Property | Value |
|---|---|
| Windows version/build | 10.0 / 26200 |
| OS and PowerShell architecture | x64 |
| Real-test privilege | Elevated administrator |
| System disk baseline | `PhysicalDrive0` only |
| AIM device baseline | Empty |

### Build Toolchain

| Tool | Version / Path |
|---|---|
| CMake | 4.2.2 |
| Ninja | 1.13.2 |
| MSVC (cl) | 19.44.35228 (x64) |
| Visual Studio | 2022 BuildTools at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| Windows SDK | `10.0.26100.0` (also present: `10.0.22621.0`) |

### Driver and Service State

| Service | Status | StartType |
|---|---|---|
| `phdskmnt` | Running | Automatic |
| `deviosvc` | Running | Manual |

The harness records the original `deviosvc` PID and service path in every
summary, stops it during query/lifecycle work, and verifies exact state and
StartMode restoration before accepting a run.

### Driver Binary

| Property | Value |
|---|---|
| Path | `C:\Windows\System32\drivers\phdskmnt.sys` |
| FileVersion | 1.2.28.93 |
| ProductVersion | 1.2.28.93 |
| CompanyName | Arsenal Recon |
| SHA-256 | `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9` |
| Authenticode | `0x0` (trusted) |
| Service | `phdskmnt`, Running/Automatic |
| Adapter interface | `\\?\root#scsiadapter#0000#{2accfe60-c130-11d2-b082-00a0c91efb8b}` |
| QUERY_VERSION | API 257, sub-version `0x01021C5D` |

### Accepted evidence

| Tier | Evidence |
|---|---|
| V3 | `reports/m0-v3-verify-20260801-233314.json` |
| V4 | `reports/m0-v4/summary-20260801-233319-6446216.json` |
| V5 | `docs/validation/m0-v5-fault-cleanup.md` |
| V6 | `reports/m0-v6/summary-20260801-233330-5281463-aggregate.json` |

### Historical Git State at Initial M0 Planning

```
commit 2794db5 (HEAD -> rewrite/driver-only)
docs: add M0 execution tasklist and drift reviews
```

The final handoff is based on the later Task24-29 implementation and evidence,
not on this historical start commit.
