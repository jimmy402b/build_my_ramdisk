# MyRamDisk 1.0.0 发布说明（2026-11-03 定稿）

**产品**：Windows 11 x64 单盘 RAM disk（service + SYSTEM worker + AIM 虚拟盘 + NTFS 卷）。
**版本**：`1.0.0`（四个二进制自报 `1.0.0`）。
**分支/提交**：`feature/m5-lifecycle`；构建对应的产品提交为 **`5381402`**（`fix(m5): protect the workers leaf on reset/install and name the offending ACL path`），
其后只有文档提交。
**状态**：发布候选已冻结，证据可达；**未签名**。

## 1. 交付物

```text
out/release/MyRamDisk-1.0.0-win11-x64/
  bin/      myrd-service.exe  myrd-cli.exe  myrd-gui.exe  myrd-m5-worker.exe
  install/  Install-MyRamDisk.ps1  Uninstall-MyRamDisk.ps1
  docs/     m5-support.md  m5-support-matrix.md
  README.txt
  release-manifest.json      (frozen_at = 2026-11-03T08:35:31Z)
  SHA256SUMS                 (10 个文件的 SHA-256)
```

二进制哈希（与冻结 manifest 一致）：

```text
myrd-service.exe    c2b441387c173cdbc4c49550e4f4361e97963bc87df106e5b69a93b4c897a440
myrd-cli.exe        a00678e1318e4080ffcd451a5a7b765188b9abd2178ed340dc0dd93cdff40c35
myrd-gui.exe        f54a12fcc5cad4726b0b0bae76647860c36d455eaff583968fef0eb2377ab3cb
myrd-m5-worker.exe  7b689347e79f30afbe8a9fb33b0528d05b85948c308b4fddac0e3d8b3c63fb69
```

校验：`tools/Verify-M5Evidence.ps1 -EvidenceDirectory out/m5-evidence` → **`accepted`**（9 份读数，
每个 case `exit_code=0`）。

## 2. 发布门禁与证据（2026-11-03 修复后复跑）

| 项 | 结果 |
|---|---|
| 主套件 A1（158 例） | **158/158**（`a17909af…`） |
| 主套件 A2（158 例） | **158/158**（`8090d832…`） |
| 后置 B（性能/簇大小/兼容性/soak/support-bundle） | **5/5**（`ef8020c1…`） |
| fault 组（6 例） | **单命令 6/6**（`Invoke-M5FaultGroup.ps1`，2026-11-03T08:04–08:34Z；每例前硬重启 VM） |
| soak | **19 h 14 min / 526 轮 / 13 次硬断电**，0 失败；句柄 166/119 不变；worker 33.7 MB 恒定；service 工作集 9.1–12.9 MB（有界漂移，重启归零） |
| 非提权 | 5/5（只读可用、越权 exit 4） |
| 盘符 | create 指定；stop/start、服务重启、VM 硬重启后均为同一字母 |

演练细节：`docs/operations/m5-release-rehearsal.md` §9；盘符：`docs/validation/m5-drive-letter.md`。

## 3. 本轮修复（发布前）

1. **DACL 保护顺序**（`Reset-M5Guest.ps1`）：目录创建后**立刻**设 protected DACL，而不是只在 reset
   末尾统一设置。实测：reset 在挂起时最后一步不执行，`profiles\workers` 留成 unprotected，服务随后
   拒绝启动（`profile_store.directory_acl`），依赖服务的用例确定性失败。
2. **安装器自愈**（`Install-MyRamDisk.ps1`）：若 `profiles\workers` 已存在且未保护，重装时一并修复；
   实测：未保护 → 运行安装器 → `repaired the existing workers ACL` → 服务 `RUNNING`。
3. **错误信息点名目录**（`profile_store.cpp`）：拒绝信息从
   `A profile directory ACL is not protected` 改为带上具体路径与期望形状，实测输出
   `... not protected: \\?\c:\programdata\myramdisk\profiles\workers (expected a protected DACL owned by SYSTEM or Administrators)`。
4. **卡死（fault 注入后）**：根因是 Windows 的 WMI/PowerShell 在 fault 用例后被拖住——实测此时
   `cmd`、`myrd-cli` 等**原生程序仍正常**，`powershell -Command` 挂起；驱动服务不支持 `sc stop`
   （1052），只能重启 VM。因此 fault 组运行器改为**每例前硬重启 VM + PowerShell 就绪探针**（`cmd`
   探针会在挂起窗口里误判就绪），并把该流程写进 §9；产品本身不受影响，未做产品代码改动。

## 4. 支持与限制（照 `docs/operations/m5-support.md`）

- **支持**：Windows 11 x64；单盘；NTFS（4/8/16/32/64 KiB，create-only）；`phdskmnt ≥ 1.2.28.93`
  （安装器只检查）；管理员可修改、非管理员只读；`disk create/start/stop/checkpoint/compact/export/
  import/repair-profile/rebind/forget`、`operation show/wait/cancel`。
- **不支持**：Win10/Server/ARM64；第二块盘；卷标（`--volume-label` exit 2）；in-place 回滚（只能重装）；
  over-commit（`0x560A`）；BitLocker/SMB 上的 bundle（未测）；运行中 export；改簇大小。
- **未签名**：无代码签名证书，SmartScreen 可能告警；以 SHA-256 作为完整性校验。

## 5. 安装 / 升级 / 卸载

```powershell
# 安装（管理员）
powershell -ExecutionPolicy Bypass -File install\Install-MyRamDisk.ps1
# 升级：覆盖安装（数据保留）
# 卸载（保留数据，可重装后继续用）
powershell -ExecutionPolicy Bypass -File install\Uninstall-MyRamDisk.ps1
```

卸载/升级前先 `myrd-cli disk stop <disk>`；运行中卸载会先停服务并清理 worker。

## 6. 发布时已知的未完成项（不隐藏）

1. `unit.m3.vhdx_reader`（`current_cache_entries == 0`）偶发失败，单独跑 5/5 通过——**定性安排在发布后**。
2. fault 注入后 Windows WMI/PowerShell 会被拖住，fault 组必须每例一次硬重启；这是环境限制，
   不是产品数据路径失败（原生 CLI 在同样状态下正常）。
3. soak 为 19 h 14 min（任务清单要求 24 h，用户裁定 19 h 级已足够）。
4. 驱动缺失/旧驱动分支、start 时字母被**卷**占用的分支未做破坏性实测（约束/环境原因）。
5. service 工作集在单个生命周期内 +1–2 MB、重启归零（有界漂移，接受风险）。

## 7. 回滚/重装

没有版本回滚：卸载（保留数据）后安装新版本即为升级路径；重装后盘可继续使用
（`integration.m5.upgrade` 读数）。
