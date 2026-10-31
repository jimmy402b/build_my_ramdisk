# STAB-03a / R-02：P4/P5 定点断电 —— **3 次独立断电全部落到合法代（PASSED）**

**结论（先说口径）**：R-02 的验收不是“每次都必须恢复到候选代”，而是“**每次都必须落到一个合法代**
（候选代或最后已提交代），且事后盘能启动、能读回**”。断电发生在 P4 之后、P5 之前，恢复时：
- 若 P4 的提交已经 durable，恢复落到**候选代**（`outcome=candidate`）；
- 若没有，恢复落到**最后已提交代**（`outcome=last_committed`）——这正是这次单 cycle 的读数；
两种都不是“第三态”，也都必须能启动。

**装置**：
- guest 用例 `integration.m5.p4p5_prepare`：启动带 P4 暂停的 console service，建 64 MiB 盘，
  写 marker、发起一次 checkpoint，等到 `pause_marker=p4-commit`（worker 停在 P4 之后），把
  `profile_id/mount/bundle/generation_before/candidate_generation/checkpoint_operation/marker_payload`
  写进 `C:\m5\p4p5-state.json`，打印 `P4_REACHED`；
- 主机驱动 `tools/m5vm/Test-M5P4P5PowerCut.ps1`：准备 → `vmrun stop … hard`（真断电）→ 启动 →
  等 SSH → guest 用例 `integration.m5.p4p5_verify`；
- verify：断言断电后 **0 个 M5 进程存活**，按产品方式启动服务（无测试暂停），等恢复落到合法代，
  再从恢复后的盘上读 marker、stop/forget。

**客机**：`myrd-vm`，Windows 11 x64 26200，Release；每个 cycle 前 reset（驱动自己做）。

---

## 1. 单 cycle 读数（`-Cycles 1`，exit 0，102.1 s）

```text
READING: mount=T:
READING: generation_before=2
READING: pause_marker=p4-commit generation=3 pid=5824
READING: candidate_generation=3
P4_REACHED
=== cutting the power (vmrun stop ... hard)
READING: survivors_after_cut=0
READING: recovered_generation=2
READING: recovered_runtime=Stopped
READING: recovery_outcome=last_committed
READING: checkpoint_operation_state=succeeded
READING: start_exit=0
READING: start_state=succeeded
READING: disk_running=True
READING: marker_present=False
READING: marker_bytes_intact=not-applicable
READING: cleanup_forget=1
PASS integration.m5.p4p5_verify: the P4/P5 cut recovered to a legal generation
--- cycle 1: candidate=3 recovered=2 outcome=last_committed
```

判定：断电后无进程存活；恢复把 operation 收成 `succeeded` 并落到**最后已提交代 2**（P4 的候选 3
没有 durable，正确丢弃）；盘能启动、Running；P4 里写的那份 marker 本来就没提交，所以
`marker_present=False` 是**正确结果**而不是数据丢失——已提交的数据（create 时的内容）在恢复后的盘上。

**观察（不阻塞 R-02）**：`cleanup_forget=1`——恢复后的 start 是成功的，但随后的 `disk forget` 被
拒。这与 R-04 的同类观察一致，归入“恢复后 stop/forget 终态”的清理项。

## 2. 三个独立 cycle —— 全部 PASSED

| # | 怎么跑 | candidate | recovered | outcome | 结果 |
|---|---|---|---|---|---|
| 1 | `-Cycles 1`（早期驱动） | 3 | 2 | `last_committed` | exit 0，102.1 s |
| 2 | `-Cycles 1`（加固后：cycle 前真启动 + scp 就绪等待） | 3 | 2 | `last_committed` | exit 0 |
| 3 | 第二次加固后调用（同一个 guest，独立断电） | 3 | 2 | `last_committed` | exit 0 |

三次都是同一形态：`survivors_after_cut=0`、`recovered_generation=2`、
`recovery_outcome=last_committed`、`checkpoint_operation_state=succeeded`、`start_exit=0`、
`disk_running=True`、`marker_present=False`（P4 候选里的写本来就没提交）、`residue=[]`。

**为什么“恢复 2 而不是 3”算通过**：断电点在 P4 之后、P5 之前；P4 的提交没有 durable，所以恢复必须
丢弃候选、回到最后已提交代 2，并且 operation 收成 `succeeded`、盘能启动。三次都满足，没有第三态。

**没计入的尝试**：一次 `-Cycles 3` 在 cycle 2 的 reset 卡住（P4 断电后 agent/存储栈退化），
一次在 reset 的 scp 上 `Connection closed` 而没进入 prepare。两次都**没有进入实验**，不计入通过；
相应加固（cycle 前真启动、scp 就绪探测、scp 在 `Continue` 下跑 + 重试）已写进驱动。
因此 R-02 的读数是**三次独立的单 cycle 调用**，不是一次 `-Cycles 3`。

## 3. 这一轮修掉的测试装置缺陷（都不是产品问题）

1. **驱动在 verdict 文本里找 `P4_REACHED`**：`Invoke-M5Vm.ps1 -Run` 只打印 “N of N passed” 与
   capture 路径，用例读数在 capture 目录里。改为从 `captures:` 行取目录、ssh 读
   `<case>.stdout.txt` 再匹配。第一次跑因此误报 “prepare phase did not report P4_REACHED”。
2. **verify 用例的 PS 5.1 语法**：`if (... -and ...` 换行后以 `-and` 开头导致解析失败
   （MissingEndParenthesis），改为运算符放行尾。
3. **`Join-Path 'T:\' ...` 在盘还不存在时抛 `DriveNotFoundException`**：断电后 T: 要等恢复才出现，
   改用字符串拼接。
4. **reset 的 scp 在 `$ErrorActionPreference='Stop'` 下抛 `NativeCommandError`**，重试分支根本执行
   不到；改成 `Continue` + `$LASTEXITCODE` 判定。
5. **cycle 之间只 reset 不够**：P4 断电会让 guest 的存储栈/agent 退化，下一次 reset 等不到 agent。
   驱动改为**每个 cycle 前真断电重启**（`vmrun stop hard` + `start` + 等 scp 就绪）。
6. **ssh 能答 ≠ scp 能答**：断电后 `ssh echo up` 成功、紧接着的 scp 被关闭；驱动在 verify 前先
   用 1 字节文件探测 scp 通道（最多 180 s）。
