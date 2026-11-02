# STAB-05：支持平台与驱动前置（R-05 / R-06）

**目标**：支持矩阵（每格“通过/不支持/未测”）、`phdskmnt` 版本下限检查、缺驱动时**可操作**的提示；
按决策 4 只支持 Win11，Win10/Server 标“不支持”且不做冒烟。

## 1. 支持矩阵（`docs/operations/m5-support-matrix.md`、`m5-support.md`）

| 项 | 结论 | 依据 |
|---|---|---|
| Windows 11 x64（26200） | **支持** | 本文件与 STAB-01/03/10/11/16 的客机读数都在该平台取得 |
| Windows 10 / Server / ARM64 | **不支持**（决策 4） | `m5-support.md` 的矩阵行写明；不分配冒烟 |
| 驱动 `phdskmnt` | **≥ 1.2.28.93**，安装器只检查、不安装/不替换/不降级 | `Install-MyRamDisk.ps1` 的 `$driverFloor` 与本页 §2 |

## 2. 驱动前置：客机实测（2026-11-02，VM `myrd-vm`）

```text
driver_file=True
driver_version=1.2.28.93
service=RUNNING
--- check the AIM driver, do not install it
  phdskmnt 1.2.28.93 present and running (floor 1.2.28.93)
WOULD INSTALL from <build>\out\build\msvc\src\Release to C:\Program Files\MyRamDisk
--- exit=0 ---
```

即：安装器的三件事——**存在**（`sc query phdskmnt`）、**文件在**（`System32\drivers\phdskmnt.sys`）、
**版本不低于 1.2.28.93**——在客机上全部通过，`-WhatIfOnly` 不写任何文件就给出结论。

## 3. 缺驱动/旧驱动的拒绝路径（代码级 + 提示文本）

`Install-MyRamDisk.ps1` 在写任何文件之前拒绝，且每条都把用户指向 `docs/operations/m5-support.md`：

| 情况 | 提示（摘要） |
|---|---|
| 服务不存在（`sc query` 返回 1060） | `the AIM driver (phdskmnt) is not installed. ... this installer will not install it for you; the driver requirement is on docs/operations/m5-support.md.` |
| 服务在但驱动文件缺失 | `the AIM driver service exists but <path> is missing; the driver installation is incomplete and will be refused; see docs/operations/m5-support.md.` |
| 版本低于下限 | `the AIM driver is phdskmnt <version>, and this release requires 1.2.28.93 or newer. Update the driver package before installing; this installer will not replace it. See docs/operations/m5-support.md.` |

**没有在客机上做的部分（诚实记录）**：缺失/旧驱动两条拒绝路径**没有**被破坏性验证——那需要卸载或
替换 `phdskmnt`，而工作区约束明确禁止。它们以代码路径 + 提示文本核对，且与 R-05 之前那批“5 条驱动
拒绝信息指向支持页”的改动一致。**不写成“客机验证过”**。

## 4. Defender 冒烟

由 STAB-16 的 `integration.m5.compatibility` 覆盖（Defender 开启时 create/start/checkpoint/stop 全通过），
见 `docs/validation/m5-stab-16-compatibility.md`。

## 5. 结论

**PASSED**：支持矩阵成文（Win11 支持、Win10/Server/ARM64 不支持）、驱动下限 1.2.28.93 在客机上
被安装器正确判定（读数见 §2）、三条拒绝信息都可操作并指向支持页；缺失/旧驱动的破坏性验证按约束
不做，已在 §3 如实标注。