# STAB-03a / R-02：P4/P5 定点断电 —— **3 个 cycle 全部落到合法代（进行中，读数追加中）**

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

## 2. 三个 cycle

（跑完后追加：每个 cycle 的 candidate/recovered/outcome/residue。）

## 3. 这一轮修掉的测试装置缺陷（都不是产品问题）

1. **驱动在 verdict 文本里找 `P4_REACHED`**：`Invoke-M5Vm.ps1 -Run` 只打印 “N of N passed” 与
   capture 路径，用例读数在 capture 目录里。改为从 `captures:` 行取目录、ssh 读
   `<case>.stdout.txt` 再匹配。第一次跑因此误报 “prepare phase did not report P4_REACHED”。
2. **verify 用例的 PS 5.1 语法**：`if (... -and ...` 换行后以 `-and` 开头导致解析失败
   （MissingEndParenthesis），改为运算符放行尾。
3. **`Join-Path 'T:\' ...` 在盘还不存在时抛 `DriveNotFoundException`**：断电后 T: 要等恢复才出现，
   改用字符串拼接。
