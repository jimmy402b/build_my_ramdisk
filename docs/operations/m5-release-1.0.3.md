# MyRamDisk 1.0.3 发布说明（2026-11-06）

**产品**：Windows 11 x64 单盘 RAM disk（service + SYSTEM worker + AIM 虚拟盘 + NTFS 卷）。
**版本**：`1.0.3`（四个二进制自报 `1.0.3`）。
**分支/提交**：`feature/m5-lifecycle`；发布提交以 `git log` 为准。
**状态**：已冻结、已打包；**未签名，未发布**（发布前等用户明确确认）。
**范围**：补齐 T4-03 的在线 compaction——此前 `disk compact` 与自动 compaction 都会被调度，
但生产 M5 worker 只返回 `The worker does not implement the compaction boundary protocol`，
journal 是 append-only，空闲时也持续增长，最终会到上限。1.0.3 让 worker 真正执行折叠、
发布新 manifest、切换 backing 并删除旧 journal。

## 1. 交付物

```text
out/release/MyRamDisk-1.0.3-win11-x64/
  bin/      myrd-service.exe  myrd-cli.exe  myrd-gui.exe  myrd-m5-worker.exe
  install/  Install-MyRamDisk.ps1  Uninstall-MyRamDisk.ps1
  docs/     m5-support.md  m5-support-matrix.md
  README.txt
  release-manifest.json
  SHA256SUMS
```

二进制哈希（与冻结 manifest 一致）：

```text
myrd-service.exe    553f858d8ee699695c1bf6dec2675027bd2d2c69232073fd1b4ff29a49c5eaf5
myrd-cli.exe        a5eb34fa3379765b4fe485400e85a2791ffc862364550d15b4e65decb631b7ec
myrd-gui.exe        55a5ffde8557fe1dc3c855ba592ec032af16c60822822fb161a32304de9e4a12
myrd-m5-worker.exe  022e105a212f484e17c77d595ccb714997a3c94d416aadca04823d0a02738bc4
```

**可重复构建**：同一提交两次 `cmake --build out/build/msvc --config Release --clean-first`
产出四个二进制 SHA-256 完全一致（见 §6）。

## 2. 改了什么

**数据流**：worker 在盘运行中直接对 bundle 做 M3 折叠，不卸载卷、不停盘。新增四个 worker 命令：

- `compaction_prepare`：preflight → 构建新 base/journal → 一致快照与真实 cut epoch →
  把 delta 写进新 journal，但**不发布 manifest**；未发布事务留在 worker 内。
- `compaction_commit`：发布新 manifest、install 新 backing、reconcile；成功后删除旧
  base/journal，journal 命名沿用 `journal-e0002-c<generation>.rdj`。
- `compaction_abort`：丢弃未发布事务，committed generation 不变。
- `inspect_compaction`：重启后只读比较 generation 与 image uuid，回答 source / candidate /
  unprovable。

**服务侧 durable phase**：`admitted/checkpointing →（prepare）→ mutation_boundary（cut 未知）
→ cut（写入真实 epoch）→ commit → evidence_recorded → committed_pending_epoch_confirmation
→（status 读 dirty epoch）→ stable`。boundary 放在 prepare 之后是安全的：prepare 写的 delta
不是权威状态，只有 commit 的 manifest 发布才改变已提交镜像；在 commit 之前取消/中止，盘仍停在
原 generation。

**恢复**：commit 前任何失败都 abort 未发布事务；重启后 `reconcile_compaction` 用只读
inspection 判定 source/candidate。source=not_committed（回到 stable、可重试），
candidate+cut已知=committed（保守记 dirty，避免丢 post-cut 写），其余=indeterminate 并 park。
operation gate 会由终态释放，不会永久卡死。

**空间 preflight**：M5 split 路径按 **durable source + store dirty bound** 计算峰值，而不是
把整个 `virtual_bytes` 当成必然写入量；16 GiB 稀疏盘写 1 GiB 后 compact 不再被误判为
`DiskFull`。空间真不足时仍返回 `DiskFull`/deferred，不会半发布。

## 3. 验收读数（客机 myrd-vm，全部 Release 二进制）

| 场景 | 请求 id | 读数 |
|---|---|---|
| 手动 16 GiB：写 1 GiB → checkpoint → compact | `74a113d740694c6481dc40714759d88a` | journal `journal-e0001.rdj` **2,260,230,144 B** → `journal-e0002-c6.rdj` **12,288 B**；旧 journal 消失；文件 SHA-256 `A83FAAC5…D052AF` 在 compact 后与 stop/start 后均不变 |
| 自动：200 代 checkpoint，阈值 16 | `1d9babe0cbf144fbb2f9f8159f059799` | generation **213**、`compactions=6`、journal **20,480 B**；日志有 `maintenance: compaction dispatched (checkpoint-count)` 与 `compaction: completed` |
| Churn：10 GiB 写→删 ×3 | `bf50f8eeb9ac4cdf84d1866d025e16ab` | 每轮 checkpoint 成功（大删除后最多 1 次 `SnapshotResourceExhausted` 重试），每轮 compact 成功，journal 每轮 **8,192 B**；最终 stop/start 后 marker SHA-256 `86846F2A…E5269` 不变 |
| 1.0.2 回归：10 GiB→删→1 GiB→checkpoint→stop/start | `a922671cdbfe40be9a3bde87395a3d87` | checkpoint 第 6 次成功（前 5 次为 `SnapshotResourceExhausted` 重试），journal 2,944,077,824 B；1 GiB SHA-256 `A83FAAC5…D052AF` 在 stop/start 后不变 |
| 发布后杀 worker：`compaction-committed` 暂停点 | `d30b8f9040c04fbd8f6bcd9346d12dc8` | 暂停 marker `generation=5`；杀 worker 后 operation `reconciling`；重启服务后 `disk start` 成功，SHA-256 `09ECAC33…7D30C` 不变 |

**含义**：`disk compact` 现在是真正的在线折叠；journal 从 2.26 GB 降到 12 KB（手动）且自动
触发在 200 代内稳定工作；compact 期间/之后的数据哈希不变；发布后杀 worker 不会丢已提交数据，
也不会把 profile 永久卡住。

## 4. 全量门禁与证据

| 运行 | 请求 id | 例数 | 结果 |
|---|---|---|---|
| A1 全量 | unit `08c541be…`、model `3feaffd0…`、m1m4 `91a109a4…`、m2 `bc41d853…`、m3 `3403ad75…`、m5 `acc153ce…`、structural `fc4e2b46…` | 164 | **160 passed、2 skipped（structural 的 vswhere）、0 failed（产品）；powerloss 两例在 full-suite 的 1200 s 请求窗口内超时**。powerloss 单独运行 **PASS**（prepare `4f883cc9…` / verify `407514b7…`，cut + boot + auto-start 验证通过） |
| A2 分组 | unit `d0331917…`、model `25f30b35…`、m1m4 `f4c18390…`、m2 `0b0fae4f…`、m3 `f6462c90…`、m5 `129bd9dc…` | 159 | **全部 PASSED**；m5 7/7（cli_e2e/checkpoint_inspection/gui/install/upgrade/state_format/drive_letter） |
| A1 m5 逐例 | 同 A1 全量的 `acc153ce…`；nonadmin 用 `Test-M5NonAdmin.ps1` | 7 + 1 | **7/7 + nonadmin PASS**；`bigdisk` 是 1.0.2 起排除的已知 16 GiB create 卡死项 |
| B（5 例逐例） | performance `99e32072…`、cluster_size `62c8a7e5…`、compatibility `84ada316…`、support_bundle `d5ac6c13…`、soak `eb4af8ef…` | 5 | **5/5 PASSED**（每例前 reset 客机） |
| fault（6 例逐例） | `Invoke-M5FaultGroup.ps1`，finished_at `2026-11-06T00:13:44Z` | 6 | **6/6 PASSED**（process_kills/service_kill/bundle_corruption/stop_after_restore/hostile_acl/volume_full，均 attempts 1） |
| compaction 五项 | 见 §3 的五个请求 id；1.0.3 清洁构建后 manual 复跑 `ee32da8b…` | 5+ | **全部 PASSED**（manual/auto/churn/regression/kill） |
| powerloss | prepare `4f883cc9…`、verify `407514b7…` | 2 | **PASS**：cut 后自动恢复、盘自启动、数据验证通过 |

**门禁结论**：A1 全量 160 passed / 2 skipped / 0 product failures；A2 分组全绿；B 5/5、
fault 6/6、compaction 5/5、powerloss 2/2 有终态读数。唯一未在 full-suite 窗口内完成的是
powerloss，已用 `Test-M5PowerLoss.ps1` 单独跑通。下一步：第二次 `--clean-first` 构建、
manifest 冻结、validator accepted、打包 `SHA256SUMS`；**发布前问用户**。

## 5. 已知限制（如实记录）

- compaction 成功后会主动删除**上一代** base/journal；不支持回到上一代（只有最新已提交代被
  保证）。这是让 journal 有界的设计选择，不是缺陷。
- 大删除后的第一个 checkpoint 仍可能先 `SnapshotResourceExhausted` 中止 1–2 次再成功
  （1.0.2 已有行为）；compaction 不改这一点。
- 断电打断正在执行的操作不续跑；启动时 auto-start 可能被 reconciled 操作的 gate 拒一次
  ——与 1.0.1/1.0.2 相同。
- `unit.m3.vhdx_reader` 偶发、fault 后 PowerShell/WMI 拖住等已知项不变。
- 未签名发布；SmartScreen 后果见 `m5-release.md`。

## 6. 可重复构建与冻结

```text
两次 cmake --build out/build/msvc --config Release --clean-first 均 453/453；
四个二进制两次 SHA-256 完全一致：
  myrd-service.exe    553f858d8ee699695c1bf6dec2675027bd2d2c69232073fd1b4ff29a49c5eaf5
  myrd-cli.exe        a5eb34fa3379765b4fe485400e85a2791ffc862364550d15b4e65decb631b7ec
  myrd-gui.exe        55a5ffde8557fe1dc3c855ba592ec032af16c60822822fb161a32304de9e4a12
  myrd-m5-worker.exe  022e105a212f484e17c77d595ccb714997a3c94d416aadca04823d0a02738bc4

manifest: out/m5-evidence-1.0.3/release-manifest.json
frozen_at: 2026-11-06T08:45:27Z
readings: 22
validator: accepted
package: out/release/MyRamDisk-1.0.3-win11-x64（11 files）
SHA256SUMS: 10 条，复算 0 mismatch
```

**状态：已冻结、已打包、未发布。** 按约定，发布 1.0.3 前必须由用户明确确认；当前没有
tag、没有 push、没有发布。
