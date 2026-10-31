# STAB-03b / R-03：敌意环境（bundle 损坏 / 卷满 / ACL）—— **损坏矩阵 + ACL PASSED；卷满未测**

**结论分段：**

1. **bundle 损坏矩阵（R-03 第 1 部分）PASSED**：`integration.m5.bundle_corruption` **1/1，48 s**，
   A/B/C/D 四种损坏全部“明确失败 + 无 worker + bundle 逐文件不变 + 恢复后可启动且数据完整 +
   stop 成功”。
2. **ProgramData ACL 被改坏 PASSED**：`integration.m5.hostile_acl` **1/1，87 s**，保护位被清掉后
   `disk list`/`disk start` 都明确失败（`0x4905`、`profile_store.directory_acl`、 “not protected”）、
   无 worker；恢复后同一块盘可启动、marker 完整、stop 成功。
3. **上一轮报的“修复后 stop 失败”是测试自身的缺陷，不是产品缺陷**：用例拿**一份最初的备份**
   反复恢复，而每次成功的 stop 都会推进 bundle 的 generation；于是第二次恢复把 bundle 倒回到
   旧代，与 durable profile 的 confirmed generation 分裂，stop 的 checkpoint boundary 自然被判
   `correlation invalid`（`prior=4 confirmed=5 candidate=5`）。改成**每次损坏前单独备份那个文件、
   只恢复它**之后，四个用例全绿。这段保留在这里，因为“用旧备份恢复”很容易再犯。
4. **bundle 卷满（bundle 所在卷写满）**：**未测**，测试脚本还没写。

**客机**：`myrd-vm`，Windows 11 x64 26200，Release；每次运行前 `Reset-M5Guest`（损坏实验前硬重启）。

---

## 1. 客机读数（`integration.m5.bundle_corruption`，请求 `55e8a21a…`，1/1 PASS，48 s）

四种损坏，每种都走同一套判据：start 的 **operation 终态 failed**（不是 CLI 准入失败、也不是挂死）、
无 worker 残留、盘不显示 Running、bundle 逐文件长度+SHA-256 不变、恢复被损坏的那个文件后同一块盘
可启动并读回已提交的 marker、最后能正常 stop。

| 用例 | 损坏 | start 终态 | worker | bundle 不变 | 恢复后启动 | marker | stop |
|---|---|---|---|---|---|---|---|
| A | `disk.rdm` 截断到 32 B | `failed` (112 ms) | 0 | yes | Running | intact | succeeded |
| B | `disk.rdm` 两个槽各翻 1 字节（2048/6144） | `failed` (185 ms) | 0 | yes | Running | intact | succeeded |
| C | `base-0000.vhdx` 截断 1 MiB（4 MiB → 3 MiB） | `failed` (214 ms) | 0 | yes | Running | intact | succeeded |
| D | journal 头 + 尾各翻 1 字节（64 / 10117120） | `failed` (88 ms) | 0 | yes | Running | intact | succeeded |

`cleanup_forget=0`（清理也成功）。每行 `bundle_unchanged=yes` 是比较失败前后所有 bundle 文件的
长度与 SHA-256 得出的，所以“明确失败”不是靠错误文案，而是靠**没有改写/重建/重格式化**。

### 1.1 为什么 B 要同时翻两个槽

`disk.rdm` 是双槽（每 4096 字节一代）。只翻一个槽时，产品用另一个完好槽把盘正常启动——这是正确的
回退，不是缺陷；所以“manifest 损坏必须被拒绝”的用例必须把两个槽都破坏，确保没有完好副本可用。

### 1.2 上一轮“修复后 stop 失败”的真相：测试用了旧备份

上一轮把 stop 失败记成产品阻塞点，实际是**测试缺陷**：用例只在一开始做了一次 bundle 快照，之后
每个用例都恢复同一份旧快照。每次成功的 stop 都会推进 bundle 的 generation，所以第二次恢复把
bundle 倒回到旧代，与 durable profile 的 confirmed generation 分裂。客机日志把根因写得很清楚：

```text
kind=7 command=disk.stop phase=running message=checkpoint boundary refused:
  profile=1 op_match=1 profile_match=1 image_match=1 pid=2124
  session_gen=5 prior=4 confirmed=5 candidate=5
kind=7 command=disk.stop phase=running message=action=checkpoint code=22022
  step=service_core.checkpoint_boundary detail=The worker checkpoint correlation is invalid
```

即 worker 用旧 bundle 切出 `prior=4 candidate=5`，而 profile 已经是 `confirmed=5`，boundary 拒绝。
修法是**每个用例损坏前单独备份那个文件、只恢复它**；修完 A/B/C/D 全绿。

**观测缺口的教训（已修）**：该用例原先没有 `New-Item $workRoot`，而 `Start-Process` 的重定向不会
创建目录，于是 `service.log` 从未生成——`kind=7` 就是这么丢的。现在目录在启动服务前显式创建，
失败时也会把 `kind=7` 打进 capture。

## 2. 已经写好的东西

- `tests/e2e/test-m5-bundle-corruption.ps1`（`integration.m5.bundle_corruption`，opt-in `fault` 组）：
  A manifest 截断；B 双槽翻位；C base 截断；D journal 头+尾翻位。每个 case 的判据：
  ① start 的 operation 终态 `failed`（或准入前 exit 1）；
  ② 无 worker 残留、盘不显示 Running；
  ③ bundle 逐文件长度+SHA-256 不变（不静默重建/重格式化）；
  ④ 恢复文件后同一块盘可启动、marker 字节一致、能正常 stop。
- 用例里已加 `Stop-DiskForNextCase`：普通 stop 失败时尝试 `--force`、再按可观测量（runtime）判定，
  并在仍然 Running 时**先把 operation show / disk list / trace tail dump 出来再失败**——之前两次
  失败都被 harness 先杀掉服务，导致没有证据可读。
- 另外新增 `integration.m5.stop_after_restore`（`tests/e2e/test-m5-stop-after-restore.ps1`）：
  单次“损坏→拒→修复→启动→stop”闭环。它单独跑 **1/1 PASS**，与损坏矩阵一起把“修复后 stop
  失败”的假象钉死在测试的旧备份上（§1.2）。

## 3. ProgramData ACL 被改坏 —— **PASSED**（`integration.m5.hostile_acl`，1/1，87 s）

用例：创建盘 → 写 marker → checkpoint → stop；把 `C:\ProgramData\MyRamDisk\profiles\<id>` 的 DACL
保护位清掉（`SetAccessRuleProtection($false,$false)`）；跑 `disk list` 与 `disk start`；再用原 SDDL
恢复；最后同一块盘重新 start、读 marker、stop。

```text
READING: acl.sddl_before=...D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)
READING: acl.sddl_damaged=...D:AI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIID;FA;;;SY)(A;OICIID;FA;;;BA)
READING: damaged.list_exit=1
READING: damaged.list_output=error: A profile directory ACL is not protected
 |   command: disk.list
 |   step: profile_store.directory_acl
 |   code: 0x4905 domain 2
READING: damaged.start_exit=1   （同上，command: disk.start）
READING: damaged.workers_left=0
READING: acl.protected_after_restore=True
READING: restored.runtime=Running
READING: restored.marker_intact=yes
READING: restored.stop_state=succeeded
READING: cleanup_forget=0
```

判定：ACL 被放宽后，**打开 profile 的命令明确失败**（`0x4905`、`profile_store.directory_acl`、
消息直接说 “not protected”），不是静默成功、不是挂死、不留 worker；恢复保护位后同一块盘可启动、
已提交数据完整、能正常 stop。

**一个测试细节（值得记住）**：恢复 SDDL 后 `Get-Acl .Sddl` 字符串与原始不完全相同（Windows 会补
`AI` 控制位），所以断言用 **`AreAccessRulesProtected`** 与“盘能起来”这两个语义判据，而不是字符串
相等——字符串相等会误报。

## 4. 未测的部分（明确写出来）

- **bundle 卷满**：未写脚本。计划是一个 64 MiB 的 VHD 挂成 V:，bundle 放上去，填满到只剩几 MiB，
  触发 create/checkpoint 失败；期望明确失败、最后提交代仍可恢复，腾出空间后恢复。需要 diskpart
  建 VHD，属于会创建真实卷的操作，只能在客机上做。
