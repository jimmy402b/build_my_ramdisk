# M5 发布前任务清单（H2 + H3）

**日期**：2026-10-30
**起点**：`3a13308`，H1（STAB-01/02/04）全部 PASSED；全量 160 例连续两次
**158 passed / 2 skipped / 0 failed**；manifest 4 binaries + 9 readings、`accepted`。
**定位**：这是从“单盘试点可用”走到“可以对外承诺稳定”的执行清单。
**状态不写在这里**——状态在 `2026-10-14-m5-v3-tasklist.md` 的 STAB 表；下一步在 `m5-handoff.md`。
**总门禁**：`2026-10-29-m5-stability-plan.md` §0 的 8 条 + §4 发布门禁。

---

## 0. 起点：已经证明 vs 还没证明

**已经证明（有客机读数，不要重做）**

- 单盘完整生命周期：create/start/stop/checkpoint/restart/export/forget；Load Only；易失盘保护。
- 第二块盘在准入阶段被拒（`0x5609`），第一块不受影响，`device_absent=1`。
- 内存预算生效；预算耗尽时写失败、checkpoint 可完成、重启恢复到最后一提交代、UNMAP 后恢复。
- 一条命令 160/160，两次一致；`vswhere` 缺失是 SKIP；powerloss 三段自动。

**还没证明（本清单要关闭的）**

- 故障矩阵（bundle 损坏/卷满/权限/worker 被杀/service 被杀/连续断电/P4-P5 定点断电）。
- 第二平台与驱动版本；缺驱动时的可操作提示。
- 覆盖安装、升级、**重装**（回滚明确不支持）、数据格式版本。
- 安全对抗结论。
- 版本号/changelog/签名/校验和/发布说明。
- 非提权 GUI、性能基线、24h soak、卷标决策、支持包、一页纸文档、§8 收尾、兼容性。
- H1 留下的具体债：物理 RAM 耗尽未测；耗尽错误是泛化 I/O 错误且无计数；journal 增长只有单次读数；
  “安装后立刻断电”只做了测试装置的 `RegFlushKey`，产品侧未结论。

---

## 0.1 六项决策已定（2026-10-30，用户裁定）

| # | 决策 | 对清单的影响 |
|---|---|---|
| 1 | **不允许** fixed 预算超过机器提交上限 | R-01 改为“创建时拒绝 + 可观测”，不再做物理 RAM 耗尽实验；文档写“M5 不 over-commit” |
| 2 | **暂时没有签名证书** | R-10 明确“未签名发布”，保留 SHA-256 与可重复构建；发布说明写 SmartScreen 后果 |
| 3 | 卷标**直接不支持** | R-14 缩小为“入口拒绝并说明原因 + 文档写明 + 死代码隔离”，不做换路实现 |
| 4 | **只支持 Win11** | R-06 取消第二平台测试；矩阵把 Win10/Server 标“不支持”，不做冒烟 |
| 5 | 回滚：**代码里没有**版本回滚（只有事务性 rollback）⇒ 明确只支持重装 | R-08 去掉回滚执行，改为“只支持重装”的文档 + 覆盖安装/升级/脏卸载 |
| 6 | 簇大小**做** | R-17 拆成 R-17a 实现（跨 profile/命令/worker/格式化）+ R-17b 测试路径；量级上调 |

**决策 5 的代码依据**：全树 `rollback|downgrade|previous version` 命中只存在于
storage/stop 的**事务回滚**（`release_commit_reservation(rollback)`、stop 失败回滚状态、
`rollback_metadata_preimage`），没有任何“旧版本安装包 / 数据格式降级 / 版本回退”的路径；
profile/bundle 没有格式版本字段，安装器也没有回滚动作。因此发布承诺只能是“**只支持重装**”，
并且要测“重装后数据仍可读”。

**决策 6 的代码依据**：旧文档说“全树没有格式化代码”，**已过时**——`provision_volume.ps1`
在 create 的 `provision_new` 路径里调用 `Format-Volume -FileSystem NTFS`（带默认簇大小）。
所以簇大小可以在 create 时通过 `Format-Volume -ClusterSize` 实现，属于“跨层加字段”，不是
“从零写格式化引擎”。范围见 R-17a。

## 1. 完成定义（Release Gate）

全部满足才算“可以对外承诺稳定”：

| # | 条件 | 由哪些任务关闭 |
|---|---|---|
| 1 | 受支持平台上按文档安装；驱动缺失/过旧有可操作提示 | R-05、R-06 |
| 2 | 单盘在正常停止、硬断电、worker 崩溃、service 崩溃、bundle 损坏、磁盘满、权限错误下行为可预测；已提交数据不丢；不挂死、不残留 | R-02、R-03、R-04 |
| 3 | 第二块盘被明确拒绝 | ✅ H1 |
| 4 | `--accept-volatile-data-loss` 两侧语义一致；脏盘卸载/强停可预期 | ✅ T4-02b，R-03 复测 |
| 5 | 一条命令全量；连续两次一致；manifest 冻结；干净 VM 的安装/卸载/升级读数 | ✅ H1 + R-08 + R-19 |
| 6 | 已知限制写进文档（单盘、卷标、簇大小、导出仅停机、驱动前置） | R-14、R-16 |
| 7 | 升级/覆盖安装/**重装**（回滚不支持，见决策 5）/脏盘卸载各一条客机读数 | R-08 |
| 8 | 安全边界对抗性审计：阻断项已修、接受风险已文档化 | R-09 |

---

## 2. 任务总表

> 量级是“一人 + 一台客机”的粗略人日，用于排期，不是承诺。所有验收都必须有客机读数 + validation 文档。

### H2：不关闭不得对外

| ID | 对应 | 任务 | 依赖 | 量级 | 验收读数 |
|---|---|---|---|---|---|
| R-01 | STAB-02R | 关闭 H1 技术债（决策 1）：**fixed 预算 > 机器提交上限在创建时拒绝**（明确错误、不落地 worker/bundle）；`hard_limit_rejections` 可观测；耗尽实验入一条命令 | — | 1–2 | 客机：over-commit 请求被拒且无残留；`disk show`/日志能解释一次预算拒绝 |
| R-02 | STAB-03.1 | P4/P5 之间**定点**断电注入（可重复注入点，非随机时刻） | R-04 的注入装置 | 3–5 | 每次注入后恢复到最后提交代；N 次同形态 |
| R-03 | STAB-03.2–4 | bundle 损坏/截断、bundle 卷满、ProgramData 权限被改坏 | — | 3–5 | 明确 `needs-attention`/明确失败；不重格式化、不猜、不静默降级 |
| R-04 | STAB-03.5–7 | worker 中途被杀、service 中途被杀、连续 3 次断电 | — | 2–3 | operation 终态；不挂死；每次回到最后提交代；无累积残留 |
| R-05 | STAB-05 | 支持矩阵 + 驱动版本下限检查 + 缺驱动可操作提示 | — | 1–2 | 矩阵每格“通过/不支持/未测”；缺驱动信息可操作 |
| R-06 | STAB-05 | ~~第二平台冒烟~~ **取消（决策 4：只支持 Win11）**：矩阵把 Win10/Server 标“不支持”，不分配测试 | R-05 | 0.5 | 矩阵写明“仅 Win11”；文档不再暗示其他系统 |
| R-07 | STAB-08 | 语义化版本 + `--version`（CLI/GUI/service 一致）+ changelog | — | 1 | 三处版本一致；changelog 对应 commit |
| R-08 | STAB-06 | 覆盖安装、旧→新升级、数据格式版本检查、脏盘卸载两条路径；**回滚不做（决策 5：代码里没有版本回滚）→ 明确只支持重装，并验证重装后数据可读** | R-07 | 3–4 | 干净 VM：install → create/write → 覆盖安装 → 升级 → 重装 → 数据/generation 一致；不兼容时拒绝并说明 |
| R-09 | STAB-07 | 安全对抗审计（pipe 冒充/重放、路径/重解析点、worker 权限/job 逃逸、二进制/配置 ACL、日志泄露） | — | 3–5 | 每条威胁“已阻断/接受风险”；阻断项有测试 |
| R-10 | STAB-08 | **未签名发布（决策 2：无证书）** + SHA-256 校验和 + 可重复构建；发布说明写 SmartScreen 后果 | R-07 | 1 | 发布物与 manifest 一一对应；校验和可复核；明确“未签名” |

### H3：GA 前收口

| ID | 对应 | 任务 | 依赖 | 量级 | 验收读数 |
|---|---|---|---|---|---|
| R-11 | STAB-09 | 非提权 GUI 只读 e2e | — | 1 | 非提权 list/show 可用；修改被拒且提示明确；不崩溃 |
| R-12 | STAB-10 | 性能基线（1 GiB 盘 create/start/checkpoint/stop、顺序/随机吞吐、checkpoint 停顿、每盘内存、worker 启动） | — | 2–3 | 典型值 + 最坏值；文档写“不承诺 SLA”或明确 SLA |
| R-13 | STAB-11 | 24h soak + 每 N 轮一次硬断电；handle/worker/内存/代增长监控 | R-04 | 3–5（含 24h 墙钟） | 无泄漏；内存回基线；每轮恢复正确 |
| R-14 | STAB-12 | 卷标**正式不支持（决策 3）**：`--volume-label` 明确拒并说明原因、文档写明、死代码隔离；**不**做换路实现 | — | 0.5–1 | 帮助/错误/文档一致；入口不再报“unknown option” |
| R-15 | STAB-13 | `support-bundle`（版本/OS/驱动/service/profiles/最近日志/operations/manifest + 脱敏） | R-07 | 2–3 | 陌生用户能生成；包能解释一次真实失败 |
| R-16 | STAB-14 | 一页“支持范围/已知限制/数据丢失上限/备份导出/驱动要求/卸载注意”；错误信息指向它 | R-05、R-14 | 1–2 | 文档评审通过；每条限制有测试或“未测” |
| R-17a | STAB-15 | **实现簇大小（决策 6）**：CLI `--cluster-size` → command/IPC → intent/durable → profile 字段 → worker launch → `provision_volume.ps1` 的 `Format-Volume -ClusterSize`；`disk show` 显示；create-only | — | 4–6 | 客机：create 带 64K 后 `fsutil fsinfo ntfsinfo` 的每簇字节 == 配置，`disk show` 一致；默认 4K 不变；恢复后不变 |
| R-17b | STAB-15 | §8 另一项：测试里的源码树绝对路径改环境变量/相对路径 | — | 1–2 | 非镜像布局也能跑 |
| R-18 | STAB-16 | 兼容性扩展（**Win10 已移出，决策 4**）：NTFS 压缩/加密目录上的 bundle、Defender 开启、非系统盘/不同扇区（若支持） | R-05 | 2–4 | 矩阵更新；不支持项有明确错误而不是挂死 |
| R-19 | **PASSED** | 发布演练：同一 commit 构建 → 冻结 manifest/校验和 → 干净 VM 安装/冒烟/升级/重装/卸载 → 两次全量一致 → validator accepted | 2026-11-02：构建 `41ed3de`（文档 `26c0578`）；门禁 4 条命令、每条前 reset 客机：**A1 158/158**（`d38fe444…`）、**A2 158/158**（`c0af55f6…`；第一次 `afc36e3f…` 157/158 为 `m5.startup_sweep` 间歇，整轮重跑通过）、**B 5/5**（`5785e2fd…`）、**fault 6/6**（逐例 reset：`50663eb2…`/`765f5091…`/`38045f04…`/`7160e9c7…`/`08ae3e67…`/`4efba65e…`）。manifest `out/m5-evidence/release-manifest.json` `frozen_at=2026-11-02T10:21:08Z`：4 个二进制 + 9 份读数；`Verify-M5Evidence.ps1` → **`accepted`**。已知间歇：`m5.startup_sweep`、`unit.m3.vhdx_reader`；`bundle_corruption` 需干净 reset（否则卡住）→ `docs/operations/m5-release-rehearsal.md` §9 |

**合计约 38–60 人日**（一人 + 一台客机；不含等待证书的时间）。

---

## 3. H2 任务说明（为什么/做什么/产出）

### R-01 关闭 H1 技术债

**为什么**：H1 的“内存耗尽”只在预算路径上测过；用户真正可能撞的是“机器没内存了”，而当前失败表现为泛化
I/O 错误，`disk show` 也没有拒绝计数，出了问题无法自证。

**做什么**
1. **决策**：fixed 预算允许接近机器提交上限吗？
   - 若“允许”：在 8 GiB 客机上用 6 GiB 预算 + 6 GiB 写入做一次“物理 RAM 接近耗尽”的读数，记录失败形态、
     checkpoint、重启恢复；失败形态写进文档。
   - 若“不允许”：在创建时拒绝“预算 > 机器可用提交”的请求（明确错误），文档写“不支持”。
2. **可观测**：把 `hard_limit_rejections` 从 store stats 贯通到 worker 状态 → 投影 → 线格式 → `disk show`，
   或在服务日志按阈值记一次事件；至少让“这次失败是预算拒绝”可被本地确认。
3. **入套件**：把 `tests/e2e/test-m5-memory-exhaustion.ps1` 变成可重复的 gate 步骤（ctest 或
   `Invoke-M5FullSuite.ps1` 的一个可选组），不要停留在一次性脚本。

**验收**：客机读数（决策为允许时）；`disk show` 或日志能解释一次预算拒绝；一条命令能重跑该实验。
**产出**：`docs/validation/m5-stab-02b-memory-limits.md`；usage 文档补“失败长什么样/最多丢多少”。

### R-02 P4/P5 定点断电

**为什么**：随机时刻断电只证明“大多数时候能恢复”；发布门禁要求“在提交边界上断电”也能恢复。
**做什么**：在 checkpoint 的 P4（commit 落盘）/P5（epoch 发布）之间放一个**可重复**的注入点（环境变量/测试
hook + 断言到达），在该点硬断电；重复 N 次，记录每次恢复到的 generation 与 operation 终态。
**验收**：每次都在“最后提交代”或“候选代”上，且不会出现“既没提交又回滚”的第三种状态；读数为 N/N。
**产出**：`docs/validation/m5-stab-03a-p4-p5-power-cut.md`。

### R-03 bundle 损坏 / 卷满 / 权限

**为什么**：真实用户会遇到的三种“不是产品 bug 的坏环境”，现在没有产品级读数。
**做什么**
1. 截断/翻转 bundle 的 manifest、journal、base；期望：`needs-attention`、明确错误、不重新格式化、不静默重建；
2. bundle 所在卷写满（可用一个小 VHD/配额卷模拟）；期望：create/checkpoint 明确失败，已提交代仍可恢复；
3. 改坏 ProgramData ACL；期望：服务启动明确报错，不静默降级。
**验收**：三种各一条客机读数 + 明确错误码/文案；恢复后数据代正确。
**产出**：`docs/validation/m5-stab-03b-hostile-environment.md`。

### R-04 worker / service 被杀、连续 3 次断电

**为什么**：进程被杀是运维常态；“连续三次”能暴露累积残留。
**做什么**
1. checkpoint 中途 kill worker：service 不挂死，operation 到终态，重启可恢复；
2. operation 中途 kill service：重启 reconcile 到确定状态；
3. 连续 3 次硬断电重启：每次都回到最后提交代，worker/设备/盘符无累积。
**验收**：每条一条客机读数；连续 3 次 3/3。
**产出**：`docs/validation/m5-stab-03c-process-kills.md`。

### R-05 / R-06 支持平台与驱动

**做什么**
1. 支持矩阵：Win11 22H2/23H2（已在 26200 上验过）、Win10 22H2、Server 2022，每格“通过/不支持/未测”；
2. 驱动：`phdskmnt` 版本下限检查（安装器 + 服务启动时），缺失/过旧给可操作提示 + 文档链接；
3. 第二平台镜像：install + `cli_e2e` 冒烟；Defender 开启冒烟；记录已知 AV/EDR 不兼容组合。

**验收**：矩阵无空格；缺驱动时的信息能直接照做。
**产出**：`docs/operations/m5-supported-platforms.md`、`docs/validation/m5-stab-05-platforms.md`。

### R-07 / R-08 版本、升级、重装

**决策 5 已定**：代码里**没有**版本回滚（只有事务性 rollback），所以不承诺回滚；支持的路径是
**重装**（uninstall → install），并要求重装后旧数据仍可读。文档、安装器提示、支持页必须一致地说
“不支持回滚，只支持重装”。

**做什么**
1. 语义化版本：service/CLI/GUI/worker 一致，`--version`，changelog（按 commit）；
2. 数据格式版本：profile/bundle 格式版本字段；不兼容时拒绝启动并说明，不静默迁移；
3. 升级：旧→新、同版本覆盖安装；profile/bundle/generation 不变；
4. **重装**：卸载（保留数据）→ 安装新版本 → start/verify，数据与 generation 可读；在安装器/文档写明
   “不支持版本回滚，只支持重装”；
5. 脏盘卸载两条路径（`-AcceptVolatileDataLoss` / 拒绝）各一条读数。

**验收**：干净 VM 上 install → create/write/checkpoint → 覆盖安装 → start/verify → 升级 → **重装** → 卸载，
数据与 generation 一致；文档与安装器都明确“无回滚”。
**产出**：`docs/validation/m5-stab-06-upgrade-reinstall.md`、`docs/operations/m5-upgrade.md`。

### R-09 安全对抗审计

按威胁列（不按代码列）：
- pipe 冒充/重放：非授权用户能否发命令、能否拿到别人的 operation；
- 路径：bundle 路径的 `..`、符号链接/junction/重解析点，能否让服务写错位置；
- 服务/worker 权限：worker 是否过高权限、job object 能否逃逸；
- 二进制/配置 ACL：非管理员能否替换 service/worker/配置；
- 日志/诊断是否泄露路径或令牌。
每条给“已阻断（测试/代码证据）”或“接受风险（文档化）”。
**产出**：`docs/operations/m5-security-audit.md`。

### R-10 发布工程

**决策 2 已定**：没有签名证书 ⇒ **未签名发布**。发布说明必须写明“未签名，Windows SmartScreen 可能
拦截，需用户确认”；不把“签名”作为发布阻塞项。保留 SHA-256 校验和、发布说明模板、可重复构建结论
（同 commit 两次构建哈希一致，或记录不可重复的原因）。
**产出**：`docs/operations/m5-release-process.md`（含“未签名”决定与校验和清单）。

---

## 4. H3 任务说明（要点）

- **R-11 非提权 GUI**：非提权启动 → list/show → 修改被拒且提示清楚 → 不崩溃；截图或日志。
- **R-12 性能基线**：1 GiB 盘的 create/start/checkpoint/stop 延迟；顺序/随机写吞吐；checkpoint 期间 I/O 停顿；
  每盘内存开销；worker 启动时间；给“典型值 + 最坏值”，文档写“不承诺 SLA”。
- **R-13 soak**：24h 循环 + 每 N 轮硬断电；监控 handle/worker/内存/代增长；无泄漏、每轮恢复正确。
- **R-14 卷标（决策 3：不支持）**：`--volume-label` 明确拒绝并说明“AIM 卷不完成 SetVolumeLabel”；
  文档写明；死代码（profile 字段/worker 应用路径）隔离并标注，不做换路实现。
- **R-15 support bundle**：版本、OS/驱动版本、服务状态、profile 列表、最近日志、operation 历史、manifest；
  自动脱敏路径/令牌；用它解释一次真实失败。
- **R-16 一页纸**：支持范围、已知限制、数据丢失上限、备份与导出、驱动要求、卸载注意；安装器与 CLI 错误指向它。
- **R-17a 簇大小（决策 6：做）**：create 时通过 `Format-Volume -ClusterSize` 生效；链路是
  CLI `--cluster-size` → `DiskCreateCommand` → `ipc_codec` → `LifecycleOperationIntent`（含 durable
  reservation 编码）→ `profile::Profile` 追加字段 + 编解码 → worker launch spec → `WorkerRuntimeConfig`
  → `ProductionRuntimeAdapter` → `provision_volume.ps1`。默认 4096 保持不变；只对 create 生效
  （已格式化的卷不重排簇），`disk show` 显示，恢复后沿用镜像里的值。
- **R-17b 测试路径**：测试里的源码树绝对路径改环境变量/相对路径。
- **R-18 兼容性（决策 4：只支持 Win11）**：Win10 冒烟移出；保留 NTFS 压缩/加密目录上的 bundle、
  Defender 开启、非系统盘/不同扇区（若支持）；每个“不支持”有明确错误，不是挂死。

---

## 5. 发布演练（R-19）

在干净 VM 上按顺序做一遍，全程留读数：

1. 同一 commit 构建 → `New-M5ReleaseManifest.ps1` → `Verify-M5Evidence.ps1` accepted；
2. 校验和 + **“未签名”声明**（决策 2）；
3. 干净 VM 安装 → 驱动检查 → 冒烟；
4. 跑两次一键全量（连续两次一致）；
5. 覆盖安装 / 升级 / **重装**（不做回滚，决策 5）；
6. 卸载（含脏盘两条路径）；
7. 对照 §1 的 8 条门禁逐项打勾，形成发布说明（支持平台=**仅 Win11**、驱动要求、已知限制、
   数据丢失上限、**无回滚/只支持重装**）。

**只有这一步通过，才允许改口为“稳定版”。**

---

## 6. 顺序、关键路径、可并行项

**关键路径（必须串行）**

```
R-01 ─┬─ R-03 ─┐
      ├─ R-04 ─┼─ R-02 ─ R-13 ─ R-19
      ├─ R-09 ─┤
R-05 ──────────┤
R-07 ─ R-08 ───┘
R-10 ──────────┘
```

- **先做 R-01**（1–2 天）：按决策 1，把“超过机器提交上限”的请求挡在创建时，并让预算拒绝可观测。
- **R-02/R-03/R-04 是最大的一块**（故障矩阵），建议并行探索、串行落产品代码（AGENTS §2）。
- **R-07 必须早于 R-08**：没有版本号就谈不上“旧→新升级”。
- **R-06 已降为文档项**（决策 4：只支持 Win11），不再等第二平台镜像；证书不再阻塞（决策 2）。
- **可并行**：R-09（安全）、R-10（发布工程）、R-12（性能）、R-15（支持包）、R-16（文档）、R-17a（簇大小）。

**建议的四个里程碑**

| 里程碑 | 内容 | 出口 |
|---|---|---|
| M1 可重复的故障结论 | R-01、R-02、R-03、R-04 | “坏环境下不丢已提交数据、不挂死、不残留”有读数；超上限请求被拒 |
| M2 可交付 | R-05、R-06、R-07、R-08、R-10、R-17a | 干净 VM 安装/升级/重装全通；有版本/校验和；支持簇大小 |
| M3 可审计 | R-09、R-12、R-15、R-16 | 安全结论 + 支持包 + 一页纸 |
| M4 可承诺 | R-11、R-13、R-14、R-17b、R-18、R-19 | 24h soak + 发布演练 + 门禁全勾 |

---

## 7. 决策记录（2026-10-30 已定，见 §0.1）

| # | 问题 | 裁定 | 落到哪 |
|---|---|---|---|
| 1 | 物理 RAM 耗尽 / 超提交上限 | **不允许**；创建时拒绝 | R-01 |
| 2 | 签名证书 | **没有证书 → 未签名发布**，写 SmartScreen 后果 | R-10 |
| 3 | 卷标 | **直接不支持**，入口拒绝并说明 | R-14 |
| 4 | 第二平台 | **只支持 Win11**，Win10/Server 标“不支持” | R-05、R-06、R-18 |
| 5 | 回滚 | 代码无版本回滚 → **只支持重装** | R-08 |
| 6 | 簇大小/格式化 | **做簇大小**（create-only） | R-17a、R-17b |

**本轮只记录裁定，没有改产品代码**；下一次会话从本文档的 R-01 开始执行。

---

## 8. 明确不做（避免范围蔓延）

- G4 多盘同时运行与延迟隔离（`AGENTS.md` §2.1 已裁定延后）。
- P2 功能：`disk release-memory`、在线扩容/缩容、热导出、VHDX 导入、快照/克隆、网络共享/压缩。
- 驱动分发/安装/替换（`phdskmnt` 只检查，不安装）。
- M6 级设计（跨代回滚、VSS 等）。

---

## 9. 一页纸排期（参考）

| 周 | 任务 |
|---|---|
| W1 | R-01、R-04、R-07；R-03/R-02 的注入装置设计与首轮读数 |
| W2 | R-02、R-03 收口；R-05、R-09 开始；R-17a 设计/落地 |
| W3 | R-08、R-10；R-12、R-15 开始；R-17a 客机读数 |
| W4 | R-09 收口；R-11、R-14、R-16、R-17b |
| W5 | R-13（24h soak）、R-18、R-19 演练 |
| 缓冲 | 回归、重新冻结、发布说明 |

（按“一人 + 一台客机”估算；有第二人时 R-09/R-12/R-15 可提前并行。）
