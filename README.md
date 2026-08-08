# MyRamDisk

基于已安装的 `phdskmnt.sys` 内核驱动、由本项目自行实现 Shared Memory Proxy 和内存块后端的 Windows RAM Disk 项目。

## 当前状态

- M0：driver-only 可行性验证，机器范围内 GO。
- M1：Shared Memory Proxy 产品化里程碑，机器范围内 GO。
- M2/M2.1：动态按块分配、首次访问加载和一致性快照基础已完成。
- M3：VHDX + journal + 双槽 manifest 持久化里程碑已通过最终验收（证据冻结提交 `4ece675`）；Task41 真实 AIM+NTFS、Task42 崩溃矩阵、Task43 Formal 和 Task44 漂移审计均为 GO。

M1 已能直接调用内核驱动创建 64 MiB RAM disk，并通过 Windows 原始磁盘路径完成真实 READ、WRITE、ZERO、UNMAP、flush 和精确删除。Release 可执行文件已连续完成 20/20 次实机生命周期；完整证据见 `docs/validation/m1-proxy-acceptance.md`。

M3 可以创建和重开镜像 bundle，在第一次访问块时从已提交的 journal/VHDX 按需加载，手动执行 checkpoint 和 compaction，并在异常退出或断电后恢复最近一次完整、已确认提交的 checkpoint。最终真实 AIM + NTFS、Windows 原生 VHDX、8 次进程终止、2 次受控断电和 100-seed Formal soak 证据见 `docs/validation/m3-persistence-acceptance.md` 与 `docs/validation/m3-final-audit.md`。

M3 仍不是最终桌面产品：定时 checkpoint、Windows 服务、开机自动恢复、关机策略、自动压缩、GUI 和多磁盘策略属于 M4。普通写入进入 RAM 后即可完成；只有成功发布的 checkpoint 才有断电恢复承诺。

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
