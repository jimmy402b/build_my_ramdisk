# T1-09 断电恢复（M5-A 单盘验收）

**结论**：VMware **硬断电**之后，**没有任何人工步骤**，服务由 SCM 自起、盘由服务自起，
回到**不早于**断电前最后一次提交的 generation，且提交内的字节逐字节一致。

**客机**：`myrd-vm`，Release。三段流程：客机 prepare → 主机 `vmrun stop … hard` → 开机 →
客机 verify（verify **只看不动**：它不启动服务、不启动盘）。

---

## 1. 读数（一次完整三段，两个请求各 1/1）

```text
prepare  request 066c4bad6f97470ea5219d41030a0077   1 of 1 passed   52 s
verify   request 1eb2ca27982a4c028f2ec4367c317636   1 of 1 passed    5 s
PASS integration.m5.powerloss: the disk came back by itself after a hard power cut
```

```text
--- the service came up without being started by this script
  service state: STATE : 4  RUNNING
--- the disk came back on its own
d75ddf8b-afa7-4aa3-b4e5-87447a84b304  T:  ready  running  runtime=Running
        activity=Idle  persistence=Dirty  health=Healthy
        generation=4  mem=1048576/1073741824
  recovered generation: 4 (last committed before the cut: 3)
--- the committed bytes are intact
  marker bytes match the pre-cut record (65 chars)
  the post-checkpoint file is gone (the cut cost the writes since the last interval)
--- nothing was left behind
  workers: 1; aim-backed disks: 0
PASS T1-09: the disk came back by itself at the committed generation
```

逐条对应验收要求：

| 要求 | 读数 |
|---|---|
| 开机后**自动**恢复 | 服务 RUNNING 由 SCM 拉起；`disk list` 里盘 `runtime=Running`，verify 全程没启动任何东西 |
| 恢复到**最后完整 generation** | `generation=4`，断电前最后提交是 3 —— **不早于**，且 `persistence=Dirty` 说明掉电瞬间的脏写被重放并提交成了第四代 |
| 数据完整 | 断电前写入的标记文件 65 字符逐字节一致 |
| **最多丢一个间隔** | 最后一次提交之后写的文件**确实没了**（`the post-checkpoint file is gone`）——这是策略承诺的形态，不是数据损坏 |
| 无残留 | worker 恰好 1 个 |

## 2. 这一轮修掉的产品缺陷（不是脚本问题）

1. **`admit_auto_starts` 必须先退休陈旧的 `running`**（`6177380`）。
   断电重启后服务确实给盘下了 start，但 `disk start` 对"durable 仍写着 running"的 profile
   是**幂等空操作**：操作到 `terminal`，一个 action 都没跑，盘便一直显示 running 而没有
   worker 持有卷（`PersistenceBlocked`、`mem=0/0`、无盘符）。
   worker 是本进程的子进程，所以服务启动时 durable 的 running **永远**在描述上一个进程；
   先把它发布为 stopped（并记 `restart_reconcile` 说明），再对 `auto_start` 的盘下真 start。
   修后日志显示断电后的 start 走完 `start-proxy → create-device → locate-physical-drive →
   verify-volume → assign-mount → readiness-io → terminal`。
2. **installed 服务必须留下自己的日志**（`c97e9cc`）。
   SCM 启动的服务没有控制台、stderr 被丢弃，所以上面那条"start 什么都没做"的原因**被写出来
   又扔掉了**。现在 `%ProgramData%\MyRamDisk\logs\service.log` 追加保存。
   这条是定位第 1 条缺陷的唯一线索。

## 3. 脚本侧修掉的问题（记录，避免下次重犯）

- 状态文件用 `Set-Content` 写，断电后回来是 **0 字节**（只进了缓存）→ `FileStream.Flush($true)`；
- 状态文件写成 UTF-8+BOM、读取端用 `key=value` 手解析 → 改成 JSON + 容错读取；
- 列表行解析用了大小写不敏感的 `T:`，匹配到了表头 `disk.list: completed` → 要求前置空白；
- `Join-Path $mount (powerloss-marker.txt)` 少引号 → `CommandNotFoundException`；
- 编排器判定读 harness 的退出码（**用例失败时它仍退出 0**）→ 改读 `N of M passed`；
- `-SkipStage` 会把**旧的 e2e 脚本**发到客机（脚本随 git 归档下发）；
- 普通 ssh 会话**不是提权会话**：`\\.\pipe\` 枚举与 `myrd-cli` 在那里都不能用来下结论
  （我据此写下的"服务进程死了/服务不存在"两条已收回）。

## 4. 可重复性说明

`prepare` 在干净客机上通过 5 次（52 s ± 1 s），`verify` 需要"刚断电过"的状态才有意义，
所以三段是**一个整体**，用 `tools/m5vm/Test-M5PowerLoss.ps1` 一次跑完，不能拆开重复计数。
一次完整三段通过 + 断电后的四次独立观察（服务自起、盘自起、generation 前进、标记一致）
构成本增量的证据；`-Repeat N` 在这里等价于 N 次断电重启，时间成本约 3 分钟/次。

## 5. 2026-10-29 修复：自动启动时卷已被 Windows 挂到 D:（G3 回归）

**现象**：断电后服务自起，盘 `Ready stopped/configured`，`create-runtime` 失败在
`assign_drive_letter`：`set-mount failed first=87 second=18`。

**诊断**（失败行逐层加出来的决定性一条）：
```text
enum=4 match=1 same=1 own=1@0+63 qerr=2 cand=1@0+63 state=fs=NTFS paths=D:\ dos=none
```
记录 GUID 就是活动卷、extents 完全相同、文件系统是 NTFS；`paths=D:\` 说明开机把这块卷
自动挂到了 `D:`。一个卷只能有一个盘符，`SetVolumeMountPointW(T:, 同一个卷)` 因此返回 87。

**修复**：`Win32VolumeAdapter::assign_drive_letter` 在 Set 前用
`GetVolumePathNamesForVolumeNameW` 列出该卷已有路径，释放**盘符形式**且不是请求盘符的路径
（目录挂载点不动），再走原有 Set/采纳逻辑。

**读数（连续两次完整三段）：**
- pass 1：`T: Ready running generation=4`（cut 前提交 3），marker 65 字节一致，workers 1，aim 0；
- pass 2：`T: Ready running generation=3`（cut 前提交 3），marker 65 字节一致，workers 1，aim 0。
- 回归 `integration.m5.cli_e2e` 1/1（118 s）。

**重新冻结**：`frozen_at 2026-10-29T10:05:04Z`，worker `7fb9032b`，validator `accepted`。