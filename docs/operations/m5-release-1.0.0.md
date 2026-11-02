# MyRamDisk 1.0.0 发布说明（2026-11-02）

**产品**：Windows 11 x64 单盘 RAM disk（service + SYSTEM worker + AIM 虚拟盘 + NTFS 卷）。
**版本**：`1.0.0`（四个二进制自报 `1.0.0`）。
**分支/提交**：`feature/m5-lifecycle`，构建于 `41ed3de`（其后只有文档提交；发布记录提交见文末）。
**状态**：发布候选已冻结，证据可达；**未签名**。

## 1. 交付物

```text
out/release/MyRamDisk-1.0.0-win11-x64/
  bin/      myrd-service.exe  myrd-cli.exe  myrd-gui.exe  myrd-m5-worker.exe
  install/  Install-MyRamDisk.ps1  Uninstall-MyRamDisk.ps1
  docs/     m5-support.md  m5-support-matrix.md
  README.txt
  release-manifest.json      (frozen_at = 2026-11-02T10:21:08Z)
  SHA256SUMS                 (10 个文件的 SHA-256)
```

二进制哈希（与冻结 manifest 一致）：

```text
myrd-service.exe    69d9166aa9a2a325b5120f7f11f92942751948a2b01b67acc6156642a4adcc73
myrd-cli.exe        a00678e1318e4080ffcd451a5a7b765188b9abd2178ed340dc0dd93cdff40c35
myrd-gui.exe        f54a12fcc5cad4726b0b0bae76647860c36d455eaff583968fef0eb2377ab3cb
myrd-m5-worker.exe  7b689347e79f30afbe8a9fb33b0528d05b85948c308b4fddac0e3d8b3c63fb69
```

校验：`tools/Verify-M5Evidence.ps1 -EvidenceDirectory out/m5-evidence` → **`accepted`**（9 份读数，
每个 case `exit_code=0`）。

## 2. 发布门禁与证据

| 项 | 结果 |
|---|---|
| 主套件 A（158 例） | **158/158**，跑两遍（`d38fe444…`、`c0af55f6…`；首次 `afc36e3f…` 157/158 为 `m5.startup_sweep` 间歇，整轮重跑通过） |
| 后置 B（性能/簇大小/兼容性/soak/support-bundle） | **5/5**（`5785e2fd…`） |
| fault 组（6 例） | 逐例 reset 后 **6/6**（`50663eb2…`、`765f5091…`、`38045f04…`、`7160e9c7…`、`08ae3e67…`、`4efba65e…`）；单命令分组运行器 `tools/m5vm/Invoke-M5FaultGroup.ps1` 已实现，端到端执行受客机不稳影响未完成 |
| soak | **19 h 14 min / 526 轮 / 13 次硬断电**，0 失败；句柄 166/119 不变；worker 33.7 MB 恒定；service 工作集 9.1–12.9 MB（有界漂移，重启归零） |
| 非提权 | 5/5（只读可用、越权 exit 4） |
| 盘符 | create 指定；stop/start、服务重启、VM 硬重启后均为同一字母 |

演练细节：`docs/operations/m5-release-rehearsal.md` §9；盘符：`docs/validation/m5-drive-letter.md`。

## 3. 支持与限制（照 `docs/operations/m5-support.md`）

- **支持**：Windows 11 x64；单盘；NTFS（4/8/16/32/64 KiB，create-only）；`phdskmnt ≥ 1.2.28.93`
  （安装器只检查）；管理员可修改、非管理员只读；`disk create/start/stop/checkpoint/compact/export/
  import/repair-profile/rebind/forget`、`operation show/wait/cancel`。
- **不支持**：Win10/Server/ARM64；第二块盘；卷标（`--volume-label` exit 2）；in-place 回滚（只能重装）；
  over-commit（`0x560A`）；BitLocker/SMB 上的 bundle（未测）；运行中 export；改簇大小。
- **未签名**：无代码签名证书，SmartScreen 可能告警；以 SHA-256 作为完整性校验。

## 4. 安装 / 升级 / 卸载

```powershell
# 安装（管理员）
powershell -ExecutionPolicy Bypass -File install\Install-MyRamDisk.ps1
# 升级：覆盖安装（数据保留）
# 卸载（保留数据，可重装后继续用）
powershell -ExecutionPolicy Bypass -File install\Uninstall-MyRamDisk.ps1
```

卸载/升级前先 `myrd-cli disk stop <disk>`；运行中卸载会先停服务并清理 worker。

## 5. 发布时已知的未完成项（不隐藏）

1. `m5.startup_sweep`（`orphans_observed == 1`）与 `unit.m3.vhdx_reader`（`current_cache_entries == 0`）
   偶发失败，单独/重跑通过——**定性工作安排在发布后**。
2. fault 组的**单命令端到端运行**尚未在稳定客机上完成（逐例读数已 6/6）；运行器已交付。
3. soak 为 19 h 14 min（任务清单要求 24 h，用户裁定 19 h 级已足够）。
4. 驱动缺失/旧驱动分支、start 时字母被**卷**占用的分支未做破坏性实测（约束/环境原因）。
5. service 工作集在单个生命周期内 +1–2 MB、重启归零（有界漂移，接受风险）。

## 6. 回滚/重装

没有版本回滚：卸载（保留数据）后安装新版本即为升级路径；重装后盘可继续使用
（`integration.m5.upgrade` 读数）。