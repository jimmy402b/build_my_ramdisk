# MyRamDisk M5 支持矩阵（1.0.0）

**口径**：本表只写**已裁定**或**有实测读数**的条目。没有读数的格子写“未测”，不写“应该支持”。
状态来源：`docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`；裁定记录见 handoff §2 与 §3.204。

## 1. 操作系统

| 项 | 支持 | 证据 |
|---|---|---|
| Windows 11 x64 | **支持** | 客机 `myrd-vm` 实测：Windows 11 x64 **26200**，AIM 驱动 Running；H1 的 STAB-01/02/04 与 R-01/R-03/R-04/R-07 都在这个版本上跑过 |
| Windows 11 ARM64 | 不支持 | 未构建、未测 |
| Windows 10 | **不支持**（裁定 4） | 不在发布范围；未测 |
| Windows Server | **不支持**（裁定 4） | 不在发布范围；未测 |

**声明下限**：Windows 11 x64。**已实测的唯一版本**是 26200；其他 Win11 build 未逐版本实测，不写成
“已验证”。

## 2. 内存与 commit

| 项 | 规则 | 证据 |
|---|---|---|
| 自动内存策略 | `min(物理内存总量 / 4, 逻辑容量)` | `service_core.cpp` `materialize_memory_policy` |
| 固定内存策略 | 请求值向下对齐到 64 KiB，必须 ≤ 逻辑容量 | 同上（`hard > logical_capacity_bytes` → `invalid_request`） |
| 固定预算的 commit 上限 | 请求值 **> 机器 commit limit 时在 create 拒绝**，错误码 `0x560A`（`memory_budget_exceeds_commit_limit`），消息 `M5 does not over-commit` | R-01：64 GiB/64 GiB → `0x560A`、无残留；`disk show` 的 `budget_rejections=` 计数 |
| commit limit 读不到 | **fail-closed**（拒绝固定预算），不猜 | `service_core.cpp` `refuse_over_commit_budget` |
| 最小逻辑容量 | 4,259,840 B（65 × 64 KiB，`kAutomaticSnapshotCowLimitBytes`） | `service_core.h` |
| 实测机器 | RAM 8,588,775,424 B（约 8 GiB），commit limit 9,125,646,336 B（8,911,764 KB） | 客机 `myrd-vm` |

**没有“最低内存”硬门槛**：能不能跑取决于“固定预算 ≤ commit limit”这条判定；自动策略本身取物理内存
的四分之一。小于最小逻辑容量的请求会被 `invalid_request` 拒绝。

## 3. AIM 驱动

| 项 | 值 |
|---|---|
| 必需驱动 | `phdskmnt`（AIM），服务名 `phdskmnt`，必须 Running |
| 实测/冻结版本 | **1.2.28.93**（`phdskmnt.sys` FileVersion，M0 冻结证据 `docs/validation/m0-environment.md`） |
| 版本下限 | **1.2.28.93**；低于此版本不支持 |
| 缺失/未运行 | **安装器明确拒绝**：服务不存在 → “the AIM driver (phdskmnt) is not installed …”；存在但未运行 → “installed (x.y.z) but not running. Start it with `sc start phdskmnt`”。服务本身没有启动前预检——**安装器是边界**（R-06） |
| 低于下限 | **安装器明确拒绝**：“the AIM driver is phdskmnt $parsed, and this release requires 1.2.28.93 or newer” |

**纪律**：本仓库不安装/替换/升级/卸载 `phdskmnt`，也不分发 AIM 二进制（`AGENTS.md` §4）。
**未复现的分支**：驱动缺失/过旧/未运行这三个拒绝分支在本客机上无法复现（不允许卸载或降级驱动），
它们的判定逻辑在 `tools/install/Install-MyRamDisk.ps1` 里是三个显式条件；正路径（1.2.28.93 通过）
由每次安装读数证明。

## 4. 卷与磁盘

| 项 | 规则 |
|---|---|
| 文件系统 | NTFS |
| 簇大小 | 1.0.0 **固定为 Windows 默认（4 KiB）**；用户可选簇大小是 R-17a，**未实现** |
| 卷标 | **不支持**（裁定 3）：`disk create --label …` 在入口被拒，消息 `--label is not supported: this release does not set volume labels`（`m5.cli` 单测已覆盖；客机 e2e 步骤已加，读数待跑） |
| 盘符 | 每次 start 重新分配，不保证跨重启不变 |
| 数据格式版本 | 1（bundle：`disk.rdm` 8 KiB + `base-0000.vhdx` 4 MiB + `journal-e0001.rdj`） |

## 5. 安装、升级、签名

| 项 | 规则 |
|---|---|
| 升级 | **覆盖安装/重装**；没有 in-place 版本回滚（裁定 5） |
| 卸载 | 保留数据；重装后可继续使用（R-08 待补读数） |
| 代码签名 | **未签名**（裁定 2）；发布物只提供 SHA-256 校验，SmartScreen 会提示“未知发布者” |
| 安装包 | MSI（见 `installer/`） |

## 6. 不在支持范围

- 物理内存耗尽（over-commit）：裁定 1，固定预算超 commit limit 在 create 拒绝。
- 卷标、Windows 10、Windows Server、ARM64、in-place 回滚、用户可选簇大小（R-17a 之前）。
- 非管理员运行 GUI：GUI 以只读方式工作（R-11）；CLI 管理命令需要管理员。

## 7. 这张表怎么用

发布说明直接引用本表；任何“支持”字样都必须能落到上表某一行。新增支持项时，**先有客机读数再改表**，
不要反过来。
