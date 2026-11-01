# R-19 发布演练手册（同 commit → 冻结 → 干净 VM → 两次全量 → 8 条门禁）

> 目标：所有产物可追溯到**同一个 commit**，8 条 Release Gate 全部打勾，`release-manifest.json`
> 由 `tools/New-M5ReleaseManifest.ps1` 冻结，`tools/Verify-M5Evidence.ps1` 输出 **accepted**。

## 0. 前置

- 工作树干净（`git status --porcelain` 为空），分支 `feature/m5-lifecycle`。
- 客机空闲、已重置（`C:\m5\Reset-M5Guest.ps1` → `profiles: 0 entries left`、`RESET`）。
- 24h soak（R-13）已完成并留下 `C:\tmp\m5-soak\...\summary.txt`。

## 1. 记录 commit 并做干净构建

```powershell
cd .worktrees\m5-lifecycle
git rev-parse HEAD | Tee-Object -Variable commit
git status --porcelain            # 必须为空
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build out/build/msvc --config Release --clean-first
# 记录四个二进制的 SHA-256（这就是发布校验和）
Get-FileHash out\build\msvc\src\Release\myrd-{service,cli,gui,m5-worker}.exe -Algorithm SHA256
```

`/Brepro` 已开启，同一 commit 的两次干净构建应给出相同哈希（R-10 已证）。

## 2. 客机两次全量（连续两次一致）

```powershell
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'
$env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
$env:M5VM_VMX='C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx'
$env:M5VM_PASSWORD='12345678'

# 重置客机（每次全量前都做）
# C:\m5\Reset-M5Guest.ps1  →  profiles: 0 entries left / RESET

tools\m5vm\Invoke-M5Vm.ps1 -Stage -Configuration Release
$run1 = tools\m5vm\Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 1800 2>&1 | Out-String
$run1 | Select-String 'passed|FAIL'
# 记录 request id 与 results 路径（输出里 `results: C:\m5\control\<id>.results.json`）
# 重置客机，再跑第二次，结果必须与第一次逐项一致
$run2 = tools\m5vm\Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 1800 2>&1 | Out-String
```

“一致”的判据：两次的 `passed` 数相同、失败集合为空、`results.json` 的 case 名单相同。

把两次 results 拉回宿主机证据目录：

```powershell
New-Item -ItemType Directory -Force out\m5-evidence | Out-Null
scp.exe -i $env:M5VM_KEY "usr@myrd-vm:C:/m5/control/<id1>.results.json" out\m5-evidence\full-run-1.results.json
scp.exe -i $env:M5VM_KEY "usr@myrd-vm:C:/m5/control/<id2>.results.json" out\m5-evidence\full-run-2.results.json
```

## 3. 干净 VM 的安装 / 冒烟 / 升级 / 重装 / 卸载（R-08 + R-19）

全量里已包含 `integration.m5.install_uninstall` 与 `integration.m5.upgrade`，它们各自给出：

- 安装 → 服务 RUNNING → `--version` 三处一致 → ACL 读回 → 卸载保留数据；
- 覆盖安装 → 保留数据卸载 → 重装 → 运行中卸载（脏卸载）。

R-19 额外要求“干净 VM 上的读数”，因此**在两次全量之前**先重置客机，再单独跑这两例并把
results 一并放进证据目录（它们会在全量里再各跑一次，两次都通过才算数）：

```powershell
tools\m5vm\Invoke-M5Vm.ps1 -Run -Filter '^integration\.m5\.(install_uninstall|upgrade)$' -MinTimeoutSeconds 900
scp.exe ... "C:/m5/control/<id>.results.json" out\m5-evidence\install-upgrade.results.json
```

## 4. 冻结 manifest

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\New-M5ReleaseManifest.ps1 `
  -BinariesDirectory out\build\msvc\src\Release `
  -EvidenceDirectory out\m5-evidence `
  -OutputPath out\m5-evidence\release-manifest.json
# 期望：manifest: wrote ... (4 binaries, N readings)
```

## 5. 校验 accepted

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\Verify-M5Evidence.ps1 `
  -EvidenceDirectory out\m5-evidence
# 期望最后一行：accepted: ... meets the evidence requirements
# 先跑一次自检，证明它会拒绝：
powershell -NoProfile -ExecutionPolicy Bypass -File tools\Verify-M5Evidence.ps1 -SelfTest
```

## 6. 八条 Release Gate 与证据对应

| # | 条件 | 证据（本轮之前已完成） |
|---|---|---|
| 1 | 受支持平台按文档安装；驱动缺失/过旧有可操作提示 | `m5-stab-04`（R-05/R-06）、`install_uninstall` 读数；**驱动缺失/过旧分支未实测**（客机不允许停用驱动） |
| 2 | 单盘在正常停止/硬断电/worker 崩溃/service 崩溃/bundle 损坏/磁盘满/权限错误下可预测 | `m5-stab-03a`（P4/P5 断电）、`03b`（损坏/ACL/卷满）、`03c`（process kills） |
| 3 | 第二块盘被明确拒绝 | H1 + `integration.m5.*` 单盘拒绝读数 |
| 4 | `--accept-volatile-data-loss` 两侧语义一致 | `m5-stab-03b`、`cli_e2e` 的易失盘读数 |
| 5 | 一条命令全量、连续两次一致、manifest 冻结、干净 VM 安装/卸载/升级读数 | 本手册 §2–§5 |
| 6 | 已知限制写进文档 | `docs/operations/m5-support.md`、`m5-support-matrix.md`（R-14/R-16） |
| 7 | 升级/覆盖/重装/脏盘卸载读数 | `integration.m5.upgrade` 1/1（§3） |
| 8 | 安全边界审计：阻断项已修、接受风险已文档化 | `m5-stab-07-security-audit.md`（R-09） |

## 7. 收尾

- 更新 tasklist（R-13 / R-19 行）、INDEX、handoff §1，写 `docs/validation/m5-stab-11-soak.md`
  与 `m5-r19-release-rehearsal.md`（commit、哈希、两次全量读数、validator 输出）。
- 提交信息写明发布状态：commit、四个哈希、soak 结果、validator `accepted`、未签名声明。
- 工作树保持干净。

## 8. 已知例外（必须写进发布说明）

- 驱动缺失/过旧的两个拒绝分支**未实测**（`AGENTS.md` §4 禁止停用/降级驱动）。
- R-11 的客机进程级非提权读数**未取得**（见 `m5-stab-09-nonadmin.md`）；产品侧授权有单测。
- 4Kn 介质、BitLocker、SMB 上的 bundle 未测（`m5-stab-16`）。
