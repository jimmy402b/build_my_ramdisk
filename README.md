# MyRamDisk

## M4 offline evidence verification

After the manual Windows VM cases complete, run the read-only evidence verifier:

```powershell
.\tools\Verify-M4SingleMultiDiskEvidence.ps1 `
  -EvidenceRoot E:\m4\evidence\qualification `
  -ExpectedSourceCommit <40-hex-commit> `
  -ExpectedReleaseSha256 <64-hex-release-hash> `
  -ExpectedDebugSha256 <64-hex-debug-hash>
```

It reads only the explicitly supplied evidence root: it does not start the
product, query devices, change services, or control VMware. Only exit code `0`
with the literal `M4 VM EVIDENCE PASS` output qualifies the evidence set.

## M4 single-disk VM runbook

Run this observer from an elevated PowerShell in the approved Windows 11 VMware
guest. It never controls VMware or changes device state. It records VM UUID and
MAC addresses, Windows/AIM/phdskmnt versions, administrator-token status,
NTFS bundle-parent and `R:`/`S:` preconditions, process/AIM/volume observations,
command output and exit code, bundle generation files and SHA-256 hashes.

```powershell
.\tests\e2e\m4_vm_lifecycle.ps1 `
  -Executable .\out\build\msvc\src\Release\myrd.exe `
  -Bundle E:\m4\disk-a `
  -DriveLetter R: -SpareDriveLetter S: `
  -EvidenceDirectory E:\m4\evidence-preflight
```

Review `preflight.json` and require `administrator`, NTFS parent, free target
and spare letters, no existing `myrd` process/AIM disk, and the approved driver
before running the product command. After the operator runs the explicitly
approved command manually, capture its literal command line and output files
without adding any VM-control operation:

```powershell
.\tests\e2e\m4_vm_lifecycle.ps1 `
  -Executable .\out\build\msvc\src\Release\myrd.exe `
  -Bundle E:\m4\disk-a -EvidenceDirectory E:\m4\evidence-run `
  -CommandLine 'myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R: --checkpoint-seconds 10 --checkpoint-dirty-mb 1' `
  -CommandStdout E:\m4\command.stdout.log -CommandStderr E:\m4\command.stderr.log -CommandExitCode 0
```

The observer reports exactly one classification: `PRODUCT_FAIL` (the approved
environment was ready but the product command violated its contract),
`ENVIRONMENT_BLOCKED` (preconditions were not met), `HARNESS_FAIL` (observer or
evidence failure), or `USER_ABORTED` (manual VM action did not follow the
runbook). It performs no automated power, remote, driver, or reboot operation;
manual VMware Reset/Power Off→Power On and post-boot rerun are operator steps.

## M4 前台运行模型

M4 的运行核心是单进程、单盘的 `DiskRuntime`。每次 `myrd run` 只拥有一个 bundle、一个 AIM 设备和一个 Windows 卷；本阶段不启动 Windows Service、worker 或集中式 IPC 管理层。

```powershell
myrd run --bundle E:\myrd\disk-a --size 64M --drive-letter R:
myrd run --bundle E:\myrd\disk-b --size 64M --drive-letter S:
```

启动两个命令即可运行两个相互隔离的磁盘。每个实例必须使用不同的 bundle 和盘符；同一个 bundle 的第二个实例会因 writer lock 拒绝，已运行的实例不会受到影响。M4 当前没有 `disk list`、`disk start` 或 `disk stop` 等集中式管理命令。

运行期间会按策略自动提交 checkpoint。只有成功提交的 checkpoint 对异常退出、重启和断电恢复负责；最近一次 checkpoint 之后的写入允许丢失。恢复已有 bundle 时只重新打开最后一个完整 generation、发现并重新挂载其卷，不会重新格式化或创建新的卷。

正常按 Ctrl+C 退出时，程序依次 flush 卷、提交最终 checkpoint、卸载卷、移除本次分配的盘符、停止 AIM 运行时并释放 bundle lock。失败路径只清理本实例实际取得的资源，不会删除其他实例的设备、卷或 bundle。

基于已安装的 `phdskmnt.sys` 内核驱动、由本项目自行实现 Shared Memory Proxy 和内存块后端的 Windows RAM Disk 项目。

## 当前状态

| 里程碑 | 状态 |
|---|---|
| M0 | driver-only 可行性验证：GO |
| M1 | Shared Memory Proxy 产品化：GO |
| M2 | 动态按块分配、首次访问加载、一致性快照基础：已完成 |
| M3 | VHDX + journal + 双槽 manifest 持久化：**已最终验收**（证据冻结提交 `4ece675`） |
| M4 | 前台单盘运行时 `DiskRuntime`：**已验收**（VM 真实设备） |
| **M5** | **进行中**：Windows 服务 + 每 profile worker + typed IPC + CLI；GUI 与产品化在后 |

**M5 的进度、下一步与全部读数不在本文件。** 入口是
[`docs/operations/m5-handoff.md`](docs/operations/m5-handoff.md)，
状态以 [`docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`](docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md) 为准。
下手之前先扫一遍 [`docs/operations/m5-lessons.md`](docs/operations/m5-lessons.md)。

`main` 是冻结的 M4 基线；M5 的全部工作在同仓库的工作树 `.worktrees/m5-lifecycle`（分支
`feature/m5-lifecycle`）。

M1 已能直接调用内核驱动创建 64 MiB RAM disk，并通过 Windows 原始磁盘路径完成真实 READ、WRITE、ZERO、UNMAP、flush 和精确删除。Release 可执行文件已连续完成 20/20 次实机生命周期；完整证据见 `docs/validation/m1-proxy-acceptance.md`。

M3 可以创建和重开镜像 bundle，在第一次访问块时从已提交的 journal/VHDX 按需加载，手动执行 checkpoint 和 compaction，并在异常退出或断电后恢复最近一次完整、已确认提交的 checkpoint。最终真实 AIM + NTFS、Windows 原生 VHDX、8 次进程终止、2 次受控断电和 100-seed Formal soak 证据见 `docs/validation/m3-persistence-acceptance.md` 与 `docs/validation/m3-final-audit.md`。

普通写入进入 RAM 后即可完成；**只有成功发布的 checkpoint 才有断电恢复承诺**。
定时 checkpoint、Windows 服务、开机自动恢复、关机策略、GUI 与多盘策略属于 M4/M5——
M4 已验收，M5 进行中（进度入口见上方）。

## 环境要求

- Windows 11 x64
- Visual Studio 2022 Build Tools 与 MSVC x64
- CMake >= 3.25
- Ninja >= 1.11
- 已安装并运行的签名 `phdskmnt.sys`（本机验收版本 1.2.28.93）
- 实机驱动测试需要管理员权限

## 构建与本地测试

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --build --preset release
ctest --preset release --output-on-failure
```

## M3 操作边界与验证

- M3 bundle、支持的 VHDX profile、备份/恢复、checkpoint、compaction、关闭和故障处置：`docs/operations/m3-persistence.md`
- M3 真实环境测试：`docs/testing/m3-e2e.md`
- M3 实现与验收证据：`docs/validation/m3-persistence-acceptance.md`
- M3 最终 Task 44 审计：`docs/validation/m3-final-audit.md`

列出 M3 验证层级不会改变系统：

```powershell
.\tools\Invoke-M3Verification.ps1 -ListCommands
```

本地 Debug/Release 回归不创建真实 AIM 设备：

```powershell
.\tools\Invoke-M3Verification.ps1 -Tier Local -Configuration Debug
.\tools\Invoke-M3Verification.ps1 -Tier Local -Configuration Release
```

`RealFunctional`、`Crash Full` 和 `Soak Formal` 是需要显式证据路径、安全确认或管理员来宾机的正式门禁。不要把 `Preflight`、`Crash Simulated`、`Soak Quick` 或 required `SKIP/BLOCKED` 解释为 M3 GO。

## M1 验证

列出将执行的门，不改变系统：

```powershell
.\tools\Invoke-M1Verification.ps1 -Tier M1-V6 -ListCommands
```

在管理员 PowerShell 中执行完整 M1 V0-V6：

```powershell
.\tools\Invoke-M1Verification.ps1 -Tier M1-V6 -Configuration Debug
```

完整流程会构建 Debug/Release，运行 M1 单元测试、10,000-case fuzz、50 次本地 SHM、Release import audit、M0 只读驱动查询、8 个故障注入和 20 次 Release 实机生命周期。

E2E harness 会串行化真实运行，只在 `try/finally` 保护的测试窗口停止 `deviosvc`，随后恢复原状态并确认 StartMode 未改变。报告写入被忽略的 `reports/` 目录，child report 原子发布后会被哈希锁定并再次校验。

## M0 回归

```powershell
.\tools\Invoke-M0Verification.ps1 -Tier V4
```

M1 provider 公布 ZERO/UNMAP 能力；M0 provider 保持冻结的旧 INFO flags=0。M0 实机回归必须持续通过。

## 安全与授权边界

M0/M1 GO 不授权卸载 AIM Toolkit、删除 `deviosvc`、替换驱动或分发独立驱动安装包。当前代码只验证在本机已有签名驱动上的 driver-only 运行时路径。独立安装和 Toolkit 移除仍需在一次性虚拟机中单独验证。

## 许可证

本项目使用 GNU Affero General Public License v3.0，详见 `LICENSE`。

固定 AIM 协议参考来自 Arsenal Image Mounter 的 AGPL-3.0 源码，固定提交为 `d379caa027d21902e45fce8c9daf02fd116c2dda`。
