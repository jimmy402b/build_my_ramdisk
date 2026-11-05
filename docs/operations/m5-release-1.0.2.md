# MyRamDisk 1.0.2 发布说明（2026-11-05）

**产品**：Windows 11 x64 单盘 RAM disk（service + SYSTEM worker + AIM 虚拟盘 + NTFS 卷）。
**版本**：`1.0.2`（四个二进制自报 `1.0.2`）。
**分支/提交**：`feature/m5-lifecycle`；产品修复提交 `4d219f2`，其后为文档提交。
**状态**：发布候选；**未签名**。
**范围**：只修 checkpoint 的恢复性——一个中止的 checkpoint 会把 journal 写长，之后这块盘永远无法再 checkpoint。

## 1. 交付物

```text
out/release/MyRamDisk-1.0.2-win11-x64/
  bin/      myrd-service.exe  myrd-cli.exe  myrd-gui.exe  myrd-m5-worker.exe
  install/  Install-MyRamDisk.ps1  Uninstall-MyRamDisk.ps1
  docs/     m5-support.md  m5-support-matrix.md
  README.txt
  release-manifest.json
  SHA256SUMS
```

二进制哈希（与冻结 manifest 一致）：

```text
myrd-service.exe    86E10B2C8D5EAB7E96E48BB4F39B2F248E074F923911FD8D22BC14522737EE7B
myrd-cli.exe        5C9B265CD7FBAEC644D34D643E7D44531EC0E9E2F3A84C75DBAD7E449BC00957
myrd-gui.exe        4C07693CE11EC1860BB94F89A11A247130EE5EEA5E6DBAF34BB948E889A334CD
myrd-m5-worker.exe  8948FCB1A5B43DF2B2E0A2EC035B0077EB3F8D92E1FF44DF285E000AA841D67A
```

**可重复构建**：同一 commit 两次 `cmake --build out/build/msvc --config Release --clean-first`
产出四个二进制 SHA-256 完全一致（见 §7）。

## 2. 修了什么

**触发**：宿主机上写 10 GiB → 在下一个 checkpoint 前删除 → 写 1 GiB → checkpoint。

**缺陷**：checkpoint 在切快照时中止（`SnapshotResourceExhausted`，"snapshot was aborted"），
但 journal writer 已经把 490 MB 记录写进 journal 文件；没有任何 manifest 引用这段尾巴，
而下一次打开 journal 时比较的是**完整的 Windows 文件 identity（含 size）**：

```text
action=flush-volume code=15 step=checkpoint.prepare
  detail=opened bundle file identity differs from the expected identity
```

于是每次重试都失败；重启后走到 publish 再失败：

```text
action=checkpoint code=8 step=checkpoint.publish
  detail=reopened journal identity changed before publish
```

profile 进入 `blocked / needs-attention / CommitIndeterminate`，而中止前接受的写入只在 RAM 里
（实测：重启后 1 GiB 文件消失）。

**修复**（`4d219f2`）：所有不提交的退出路径都把 journal 截回**已提交长度**：

- `M3Provider::checkpoint_prepare_impl`：打开 journal 后先裁掉超长尾巴；准备过程若没有走到
  pending transaction，由回滚守卫再裁一次；
- `PreparedCheckpointTransaction::abort()`：裁回自己的 `previous_committed_length_`
  （compaction 的新 journal 产物没有尾巴可裁，保持不动）。

## 3. 验收读数（宿主机，16 GiB / automatic 15.92 GiB 预算）

| 步骤 | 读数 |
|---|---|
| 写 10 GiB write-through | **14 s**，期间没有 checkpoint（generation 不变） |
| 删除 | **1 s**，文件消失 |
| 写 1 GiB | **2 s**，SHA-256 `1460863F2443528BFE2EFECA062F8AC69CCD789EC9AEBF5508EFCE5B281D092F` |
| checkpoint | 先中止 2 次（`SnapshotResourceExhausted`），第 3 次提交 gen 12；再经 interval 到 `Idle/Clean`；journal 2,945,921,024 → 4,841,418,752 B（已提交） |
| stop + start | 新 worker 从 bundle 恢复，generation 13；`Z:\one.bin` **1,073,741,824 B**、SHA-256 **一致**；10 GiB 文件不存在 |

**含义**：大删除造成的 snapshot 自动中止仍然会发生（会先花 1–2 次重试），但 checkpoint 现在能
从它恢复，而不是把 journal 永久卡死、把 RAM 里的数据静默丢掉。

## 4. 全量门禁与证据

| 运行 | 请求 id | 例数 | 结果 |
|---|---|---|---|
| A1（主套件） | `6c1d321a532b4ed18d822de4459754cd` | 158 | **158/158 passed**（08:17–08:29Z） |
| A2（主套件复跑） | `058c041868cc40ccaccff6ac21a6e953` | 158 | **158/158 passed**（08:30–08:40Z） |
| B（性能/簇大小/兼容性/soak/support-bundle） | 逐例，见下 | 5 | **5/5 passed** |
| fault（6 例，逐例硬重启） | 逐例，见下 | 6 | **6/6 passed** |

**B 组为什么逐例跑**：一条命令跑 5 例时，runner 在每个用例结束后会 kill 所有 `myrd*`（包括
service），SCM 5 s 后重启服务，启动扫描把上一个用例留下的 `running` profile auto-start 起来；
下一个用例（compatibility / soak）的 create 因此被**正确地**以单盘规则拒绝（`0x5609`）。
两次单命令复跑都是同样的 3/5（performance、cluster_size、support_bundle 过；compatibility、soak
被上一个用例的 auto-start 挡住）。改为**每例前 hard reset + reset guest、单用例提交**后全部通过——
这是 harness 的跨用例清理 artifact，不是产品回归（单盘拒绝本身是 1.0.0 起就有的正确行为）。

B 各例请求：`performance` `26dbdc8b…`（83 s）、`cluster_size` `0a27c053…`（96 s）、
`compatibility` `61dfa32f…`（99 s）、`soak` `6b1d4cb0…`（123 s）、`support_bundle` `f73b4eb3…`（76 s）。

fault 各例：`process_kills` `8fb8bd53…`（18.5 s）、`service_kill` `04eebce7…`（20.9 s）、
`bundle_corruption` `3ac18204…`（42.4 s）、`stop_after_restore` `9cfa7d25…`（16.9 s）、
`hostile_acl` `2232885e…`（81.9 s）、`volume_full` `c8c5eeb5…`（89.9 s）。
`service_kill` 与 `hostile_acl` 第一次因硬重启后 8 分钟内 SSH 未就绪被跳过，加长启动等待后重跑通过。

**冻结**：`out/m5-evidence-1.0.2/release-manifest.json`，`frozen_at=2026-11-05T09:57:51Z`，
4 个二进制 + **13 份读数**（A1、A2、B 五份、fault 六份；每份 `failed=0`）。
`Verify-M5Evidence.ps1` → **`accepted: out/m5-evidence-1.0.2 meets the evidence requirements`**。

**打包**：`out/release/MyRamDisk-1.0.2-win11-x64/`，11 个文件；`SHA256SUMS` 10 条逐条复算 **0 mismatch**。

## 5. 支持与限制

与 1.0.1 相同：Windows 11 x64、单盘、NTFS（4–64 KiB，create-only）、`phdskmnt ≥ 1.2.28.93`
（安装器只检查）；不支持 Win10/Server/ARM64、第二块盘、卷标、in-place 回滚、over-commit、
BitLocker/SMB 上的 bundle、运行中 export、改簇大小。未签名，以 SHA-256 作为完整性校验。

## 6. 已知限制（不隐藏）

1. 大删除（例如一次删掉 10 GiB）会让紧接着的 checkpoint 先自动中止 1–2 次
   （`SnapshotResourceExhausted`），之后才成功；这是**重试代价**，不是数据丢失。
2. 断电打断**正在执行**的操作时操作不续跑（start 标记失败并自动重来，checkpoint 走 reconcile）。
3. 启动时 auto-start 可能被其它 reconciled 操作的 gate 拒绝一次；profile 退成 stopped，可显式
   `disk start`。
4. `unit.m3.vhdx_reader` 偶发失败、fault 后 Windows WMI/PowerShell 拖住、驱动缺失/旧驱动分支未实测
   ——与 1.0.0/1.0.1 相同。

## 7. 构建与提交

**构建**：`feature/m5-lifecycle` @ `4d219f2`（产品修复与版本），其后只有文档提交。
四个二进制由同一工作树两次 `--clean-first` 构建，SHA-256 完全一致（见 §1）。
**冻结**：`frozen_at=2026-11-05T09:57:51Z`；13 份读数；validator `accepted`。
**发布物**：`out/release/MyRamDisk-1.0.2-win11-x64/`，`SHA256SUMS` 10 条复算一致。
