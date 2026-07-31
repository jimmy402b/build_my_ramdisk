# M0 Environment Ledger

- Normative SPEC: `docs/superpowers/specs/2026-08-01-m0-driver-only-operational-spec.md`
- Agent plan: `docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md`
- AIM source commit: `d379caa027d21902e45fce8c9daf02fd116c2dda`
- Working branch: `rewrite/driver-only`
- Real-driver tests authorized only after V0-V3 pass: yes
- Toolkit uninstall authorized: no

## Machine Facts (2026-08-01)

### Build Toolchain

| Tool | Version / Path |
|---|---|
| CMake | 4.2.2 |
| Ninja | 1.13.2 |
| Visual Studio | 2022 BuildTools at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| Windows SDK | `10.0.26100.0` (also present: `10.0.22621.0`) |

### Driver and Service State

| Service | Status | StartType |
|---|---|---|
| `phdskmnt` | Running | Automatic |
| `deviosvc` | Running | Manual |

### Driver Binary

| Property | Value |
|---|---|
| Path | `C:\Windows\System32\drivers\phdskmnt.sys` |
| FileVersion | 1.2.28.93 |
| ProductVersion | 1.2.28.93 |
| CompanyName | Arsenal Recon |

### Git State at Start

```
commit 2794db5 (HEAD -> rewrite/driver-only)
docs: add M0 execution tasklist and drift reviews
```
