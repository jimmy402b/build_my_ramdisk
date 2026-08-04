# M3 Persistence Engine TASKLIST

这是一份给执行型 Agent（包括 Claude Code）使用的执行控制文件。它不是需求来源；执行者必须同时服从：

1. `docs/superpowers/specs/2026-08-04-m3-vhdx-journal-checkpoint-design.md`
2. `docs/superpowers/plans/2026-08-04-m3-persistence-engine.md`
3. 本文件
4. `M3_MICROTASKS.md` 中当前父任务的唯一微卡
5. 已验收的 M0/M1/M2 合同

设计基线为 `80d1ae3`，其父提交 `a37ce86` 是 M2 GO。Task 00 必须重新证明当前代码包含该基线并通过新鲜的完整 Release 回归，不能只引用旧报告。

---

## A. 每个 Agent 必须遵守的闭环

### A1. 开始任何任务之前

- [ ] 运行 `git status --short`，记录并保护用户已有修改；不清理、不覆盖、不顺手格式化无关文件。
- [ ] 阅读 SPEC 的目标、排除项、对应章节和验收矩阵。
- [ ] 阅读 PLAN 的第 0 节、当前编号任务、依赖任务和 Stop conditions。
- [ ] 阅读本文件总状态表、当前任务全部检查项和下一任务标题。
- [ ] 阅读 `M3_MICROTASKS.md` 的通用事务、当前微卡、推理等级和暂停条件；一次
  只允许执行一张微卡。
- [ ] 核实所有依赖均为 `DONE` 且证据可访问；缺失时将当前任务置为 `BLOCKED`。
- [ ] 核实当前工作树只存在一个 `DOING`；将当前任务由 `PENDING` 改为 `DOING`。
- [ ] 在执行记录中写明开始提交、分支、执行者、时间、允许修改路径、假设和依赖证据。
- [ ] 先添加能观察到预期失败的测试，再写产品实现。必须记录红灯命令、失败原因和退出码。
- [ ] 若任务为 Class S 或 O，先冻结独立测试期望；生产逻辑作者不得随后把 oracle 改成迎合实现。

### A2. 完成任何任务之前

- [ ] 运行 PLAN 当前任务列出的聚焦验证，并记录完整命令、退出码和摘要。
- [ ] 运行本文件要求的累计门禁；不得只跑新测试。
- [ ] 对并发/恢复/持久化任务运行固定种子至少一次，并保存失败种子。
- [ ] 运行 `git diff --check` 与 `git status --short`，确认没有越界文件或用户修改。
- [ ] 回读 SPEC 对应章节、PLAN 同编号任务和本任务，逐项写“偏航检查”。
- [ ] 明确回答：是否改变 M1 协议、AIM 合同、M2 block 语义、durability 顺序、错误分类、删除权限或 M4 边界。
- [ ] 填写任务执行记录中的文件、红灯、实现摘要、测试、证据、风险和建议提交。
- [ ] 所有检查满足后才把状态改为 `DONE`；失败为 `BLOCKED`，尚在处理保持 `DOING`。
- [ ] 只提交当前微卡允许的文件。一张微卡一个可审查提交；父任务可以是一组按序
  微卡提交，但不得 squash 掉独立 oracle/fault fixture 的冻结点，也不得把下一父
  任务一起塞入。

### A3. 发现失败或设计冲突时

- [ ] 保留首次失败的命令、输出、退出码、种子、环境、输入文件哈希和故障注入点。
- [ ] 不通过重试、扩大 timeout、跳过、删除断言、编辑 evidence 或回退到零数据来制造绿灯。
- [ ] 在最低可复现层添加确定性红灯；修复后按“聚焦 → 当前层 → 所有受影响旧层”重跑。
- [ ] SPEC 与实现计划冲突时，停止改代码，先修低权威文档并单独评审。
- [ ] 混代恢复、脏数据早确认、损坏当作 Absent/Zero、错误磁盘、越界删除和超界 COW 都是立即停止条件。

### A4. 状态值

- `PENDING`：未开始。
- `DOING`：正在执行；全表最多一个。
- `BLOCKED`：存在具体阻塞和可审计证据。
- `DONE`：实现、累计验证、证据、提交和偏航检查均完成。
- `PAUSED_FOR_HIGH_REASONING`：当前微卡为 H/X 或需要高风险决策；保持工作树和
  RED 证据，等待 High/XHigh 逻辑负责人，不得自行降级推理等级或继续下一卡。

---

## B. Claude Code 委派边界

| 类别 | Claude Code 可以做什么 | 必须由逻辑负责人做什么 |
|---|---|---|
| C — Claude-primary | 完整实现固定格式、checked helper、fixture、CLI/parser、测试脚本等机械工作 | 逐文件审查 diff，重跑命令，确认没有扩大合同 |
| S — Split | 写红灯测试、fake、fixture、接口壳和无争议分支 | 实现或逐行审查并发、状态、durability、损坏处理等敏感逻辑 |
| O — Logic-owner | 只能准备测试、诊断和最小 adapter，除非负责人逐段接管 | 编写/签署线性化、恢复选择、发布、ack、删除与最终 verdict |

`M3_MICROTASKS.md` 的 `M/H/X` 等级比本表更细：Medium/DeepSeek 类执行者可以完整
执行 `M` 卡；对 `H` 卡只能准备卡片明确允许的 RED/fixture/接口壳；遇到 `X` 卡必须
写 `PAUSED_FOR_HIGH_REASONING` 并停止。父任务类别不能覆盖微卡暂停门。

Claude Code 一次只能收到一个任务。禁止提示“完成剩余 M3”“自行继续下一项”或允许跨任务改文件。推荐提示词骨架：

```text
你只执行 M3 Task <ID>。先完整阅读 M3 SPEC、M3 PLAN 的第 0 节和 Task <ID>、M3_TASKLIST 的 A/B/C 与 Task <ID>。
执行类别：<C/S/O>。允许修改：<从任务复制精确路径>。禁止修改其他文件，禁止开始 Task <next>。
先运行并记录基线；先添加红灯并展示失败，再实现本任务允许的范围。
按 Task <ID> 运行聚焦和累计验证，执行 git diff --check，并回读三份文档写偏航检查。
不要提交；最后给出修改文件、红灯、测试命令/退出码、风险和建议 commit。若合同冲突，停止并标记 BLOCKED。
```

Class S/O 还要追加：

```text
不要实现 TASKLIST 标为“逻辑负责人”的生产算法。只提交红灯、fixture、fake、诊断或明确列出的机械壳；保持测试期望冻结，等待接管。
```

---

## C. 构建、测试和证据约定

统一使用仓库 preset：

```powershell
cmake --preset msvc
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --build --preset release
ctest --preset release --output-on-failure
```

聚焦标签：

```powershell
ctest --preset release -L m2.1-v0 --output-on-failure
ctest --preset release -L m2.1-v1 --output-on-failure
ctest --preset release -L m2.1-v2 --output-on-failure
ctest --preset release -L m2.1-v3 --output-on-failure
ctest --preset release -L m2.1-v4 --output-on-failure
ctest --preset release -L m2.1-v5 --output-on-failure
ctest --preset release -L m3-v0 --output-on-failure
ctest --preset release -L m3-v1 --output-on-failure
ctest --preset release -L m3-v2 --output-on-failure
ctest --preset release -L m3-v3 --output-on-failure
ctest --preset release -L m3-v4 --output-on-failure
ctest --preset release -L m3-v5 --output-on-failure
powershell -NoProfile -File tools/Invoke-M3Verification.ps1 -Tier RealFunctional
powershell -NoProfile -File tools/Invoke-M3Verification.ps1 -Tier Soak
```

`m3-v0` 与 `m2.1-v0..v5` 是为了缩短反馈周期的内部标签：`m3-v0` 只表示规划基线，六个 `m2.1-*` 标签共同汇入官方 **M3-V3**。SPEC 冻结的正式门禁仍是 M3-V1（格式）到 M3-V7（soak/performance），不得重新编号。

任务记录格式：

```text
Task / status:
Executor / timestamp:
Start commit / branch / end commit:
Allowed files / actual files:
Dependencies and evidence:
Red test command / observed failure / exit code:
Implementation summary:
Focused verification / exit code:
Cumulative verification / exit code:
Evidence paths and hashes:
SPEC/PLAN/TASKLIST drift audit:
Open risks / follow-up explicitly excluded:
Suggested commit:
```

证据必须由命令生成并保留原始结果；不能手改失败 JSON。环境依赖测试的 `SKIPPED` 必须包含稳定原因，但在最终必需门禁中不等于通过。

---

## D. 总状态表

| ID | 任务 | 类别 | 状态 | 依赖 | 门禁 |
|---:|---|---|---|---|---|
| 00 | 冻结 M2 GO 与建立 M3 targets | C | DONE | 无 | M3-V0 |
| 01 | M2.1 类型、选项、错误、统计 | C | DONE | 00 | M3-V3 / m2.1-v0 |
| 02 | Mutation admission 与一致切点 | O | DONE | 01 | M3-V3 / m2.1-v1 |
| 03 | 有界 snapshot coordinator 与元数据 COW | S | DONE | 02 | M3-V3 / m2.1-v1,v3 |
| 04 | DATA pre-image | O | DONE | 03 | M3-V3 / m2.1-v2 |
| 05 | ZERO/DISCARD pre-image | O | DONE | 04 | M3-V3 / m2.1-v2 |
| 06 | Token batch、finish、abort | O | PENDING | 05 | M3-V3 / m2.1-v2 |
| 07 | 压力、stall、close 与资源清理 | O | PENDING | 06 | M3-V3 / m2.1-v3,v5 |
| 08 | 独立整盘快照参考模型 | S | PENDING | 07 | M3-V3 / m2.1-v4 |
| 09 | M2.1 前置验收 verdict | O | PENDING | 08 | M3-V3 prerequisite |
| 10 | CRC32C、streaming SHA、byte codec | C | DONE | 00 | M3-V1 |
| 11 | Exact file I/O 与 sparse fake | C | DONE | 10 | M3-V1 |
| 12 | 路径、文件身份、writer lock | C | READY_FOR_REVIEW | 11 | M3-V1 |
| 13 | Persistence 类型、错误、outcome | C | PENDING | 10,11,12 | M3-V1 |
| 14 | 4096-byte manifest codec | C | PENDING | 13 | M3-V1 |
| 15 | 完整双槽 candidate 选择 | O | PENDING | 14,20,22 | M3-V2 |
| 16 | Journal wire format 与 digest | C | PENDING | 13 | M3-V1 |
| 17 | Immutable B+tree reader | S | PENDING | 11,16 | M3-V1 |
| 18 | COW index builder | S | PENDING | 17 | M3-V1 |
| 19 | Journal checkpoint append/readback | S | PENDING | 11,16,18 | M3-V1/V2 |
| 20 | JournalReader block resolution | S | PENDING | 17,19 | M3-V1 |
| 21 | Strict VHDX parser | C | PENDING | 10,11,13 | M3-V1 |
| 22 | Lazy BAT reader | S | PENDING | 21 | M3-V1 |
| 23 | Sparse dynamic VHDX writer | S | PENDING | 11,21,22 | M3-V1 |
| 24 | Windows native VHDX gate | C | PENDING | 23 | M3-V5 |
| 25 | Journal-first composite backing | O | PENDING | 20,22 | M3-V2 |
| 26 | Recovery engine 与 BackingView | O | PENDING | 15,25 | M3-V2 |
| 27 | Generation-zero create/reopen | S | PENDING | 12,14,16,19,23,25,26 | M3-V2 |
| 28 | M2 lazy-load integration | O | PENDING | 09,27 | M3-V3/V7 |
| 29 | Image session/outcome state machine | O | PENDING | 13,27,28 | M3-V2 |
| 30 | Checkpoint pre-manifest pipeline | O | PENDING | 09,19,29 | M3-V2 |
| 31 | Manifest publish 与 indeterminate resolver | O | PENDING | 15,30 | M3-V2 |
| 32 | Install backing、ack、reconcile | O | PENDING | 06,28,31 | M3-V2/V3 |
| 33 | 独立 checkpoint crash matrix | O | PENDING | 31,32 | M3-V2 |
| 34 | Live extent enumeration | S | PENDING | 22,25,29 | M3-V4 |
| 35 | Base compaction 与 journal rotation | O | PENDING | 09,23,29,32,34 | M3-V4 |
| 36 | Immutable file retention/cleanup | O | PENDING | 12,27,35 | M3-V4 |
| 37 | Inspect/verify/report schema | C | PENDING | 27,33,36 | M3-V2/V4 |
| 38 | M3 CLI 与 local fake | C | PENDING | 27,29,30,35,37 | M3-V2/V4 |
| 39 | M1/M2/M3 provider 集成 | O | PENDING | 28,32,38 | M3-V6 prerequisite |
| 40 | 真实主机 preflight/E2E harness | C | PENDING | 24,39 | M3-V6 preflight |
| 41 | AIM+NTFS+lazy+VHDX 真机验收 | O | PENDING | 24,32,35,40 | M3-V6 |
| 42 | 模拟/进程 crash matrix | O | PENDING | 33,40,41 | M3-V2/V6/V7 |
| 43 | Soak、资源、性能、最终 M3 verdict | O | PENDING | 36,41,42 | M3-V7 |
| 44 | 文档、许可、最终偏航审计 | C/O | PENDING | 43 | 全门禁 |

---

## E. 逐任务执行卡

## Task 00 — 冻结 M2 GO 并建立 M3 构建边界

**类别：C。允许文件：** `src/CMakeLists.txt`、`tests/CMakeLists.txt`、M3 空 target 入口、`docs/validation/m3-persistence-acceptance.md`。

- [x] 证明当前历史包含 `a37ce86` 和 M2 GO 证据；记录工具链、架构、工作树和 57-test 基线预期。
- [x] 运行 configure、Release build、完整 CTest，红灯只来自尚未存在的 M3 target/label，不得改 M2 使基线通过。
- [x] 添加独立 `myrd_persistence` library、M3 test/e2e target 和 `m3-v0..v5`/`m2.1-v0..v5` CTest 标签骨架；真实 M3-V6/V7 后续由 verification wrapper 驱动，不伪造为本地标签。
- [x] 用 link/header 检查证明 `myrd_persistence` 不依赖 driver/proxy，`myrd_storage` 不包含 persistence/VHDX 类型。
- [x] 复跑完整 Release CTest；预期原有 57 项全部通过，新空壳测试不伪造功能成功。
- [x] 回读 PLAN Task 00 和边界；建议提交 `build(m3): establish persistence targets and baseline`。

## Task 01 — 冻结 M2.1 公开合同

**类别：C。允许文件：** `src/storage/consistent_snapshot.h`、M2 options/error/stats/report 文件、类型测试和 CMake。

- [x] 红灯覆盖 token/status enum、五个稳定错误、两个 option 的全部边界、stats JSON fail-closed 和 enum stable strings。
- [x] 实现 PLAN 1.1 的精确字段/签名；复用现有 `dirty_epoch`，禁止建立第二套同义 epoch。
- [x] `snapshot_cow_max_bytes` 必须容纳一个由预算 allocator 计费的完整 DATA record + 64 KiB payload；不发明块对齐约束。ZERO/DISCARD metadata、map/set node 和 payload 都计入同一硬上限；stall limit 为 `(0,5s]`。
- [x] 运行 `m2.1-v0` 和全部 M2 类型/JSON 回归。
- [x] 偏航审计公开 ABI、错误字符串、M4 边界；建议提交 `feat(storage): define consistent snapshot contracts`。

## Task 02 — 建立 mutation admission 与线性化切点

**类别：O。允许文件：** `dynamic_block_store.h/.cpp`、新的 mutation admission helper、并发测试；不改 persistence。

- [x] 独立红灯证明 write/zero/unmap 的整个多块操作持有 mutation token，reads 不持有。
- [x] 红灯覆盖 begin 与已进入/排队 mutation 的交错、取消、deadline、close 和两个并发 begin。
- [x] 逻辑负责人实现 admission close → drain active mutations → capture generation/epoch → activate → reopen 的唯一切点。
- [x] 用 lock-order instrumentation 证明 drain 阶段 lifecycle/state/全部 stripes 均未被切点持有；Task 02 尚无 coordinator，coordinator → stripe 反向获取门禁随 Task 03 coordinator 一并执行。
- [x] 跑 TSAN 不可用时的确定性 barrier tests、`m2.1-v1` 和全部 M2 并发回归。
- [x] 已提交 `ca1b744 feat(storage): establish consistent mutation cut`。

## Task 03 — 有界 snapshot coordinator 与元数据 COW

**类别：S。允许文件：** snapshot coordinator、store 私有成员/接线、fake clock/barrier、聚焦测试；Claude 写红灯与 class shell，逻辑负责人写/审查 transition。

- [x] 红灯覆盖 active token 唯一性、cutoff membership、每块一次 pre-image、单调 `next_scan_block` frontier、预算计数和 metadata rollback；禁止动态 consumed-block set。
- [x] 先只实现 ZERO/DISCARD 元数据 pre-image 容器与 token 生命周期，不实现 DATA payload。
- [x] 预算保留必须在 mutation 发布前成功；失败/取消不留下幽灵 record 或泄露 byte/accounting。
- [x] 审查所有锁路径符合 PLAN 1.2，运行 `m2.1-v1`、`m2.1-v3`。
- [x] 已提交 `cf3671f` 与安全加固 `7858f2e`；验收证据见 `docs/validation/m3-persistence-acceptance.md`。

## Task 04 — 保存 DATA pre-image

**类别：O。允许文件：** write/read-load mutation 路径、snapshot coordinator、DATA COW tests。

- [x] 红灯覆盖 resident DATA、backing load 后 mutate、跨块 write、同块重复改写、snapshot reader/mutator 交错。
- [x] 在第一次 post-cut mutation 发布前复制恰好 64 KiB；记录原 version/kind/base_generation/dirty_epoch。
- [x] 已 consumed 或已保存 pre-image 的块不重复分配；分配/I/O 失败时前台 mutation 与 snapshot outcome 遵守 frozen policy。
- [x] 用 payload hash/reference model 验证 pre-cut 内容，不接受“最终内容也一致”的弱断言。
- [x] 跑 `m2.1-v2` 和 M2 write/load 回归；实现、加固与测试清理提交为 `3e8b858`、`0ba4be5`、`e24cf89`，证据见验收文档。

## Task 05 — 覆盖 ZERO/DISCARD 状态转换

**类别：O。允许文件：** zero/unmap/write 状态转换、snapshot tests。

- [x] 建立 DATA↔ZERO、DATA↔DISCARD、ZERO↔DISCARD、backing-backed/implicit-zero 的完整真值表红灯。
- [x] ZERO/DISCARD pre-image 不分配 payload，但保留旧逻辑 kind 和精确 ack identity。
- [x] 证明 post-cut 新 dirty block 不加入 token，pre-cut 已 dirty block只出现一次。
- [x] 跑 `m2.1-v2`、M2 zero/unmap/dirty ack 回归；建议提交 `feat(storage): snapshot zero and discard transitions`。

执行记录：

```text
Task / status: 05 / DONE
Executor / timestamp: Codex logic owner / 2026-08-04 20:05:53–22:00 +08:00
Start commit / branch / accepted commits: 4829c743403cb08ba2e17101d7f78c53c39315cc / feature/m3-task05 / 14daea4, 6440409, 170cd51
Allowed files / actual files: zero/unmap/write 状态转换、snapshot coordinator、snapshot/range tests、Task05 控制与验收文档 / dynamic_block_store.h、dynamic_block_store_write.cpp、dynamic_block_store_ranges.cpp、dynamic_block_store_read.cpp（仅扩展 TestAccess identity seam）、consistent snapshot tests、本文件、验收文档
Dependencies and evidence: Tasks 03/04 DONE；docs/validation/m3-persistence-acceptance.md 已记录双独立 GO
Red test command / observed failure / exit code: Release focused 首次在 transition matrix 的 snapshot_preimage 断言失败，证明 DATA→ZERO/DISCARD 覆盖旧值却未 COW，exit 1；后续 partial ZeroDirty invalid-identity no-op 红灯在 !rejected.ok() 失败，exit 1；旧 M2 write/ranges 回归暴露“无活动 token 也验证 test-seeded epoch 0”的过宽修复，2/4 fail，收窄到 active-token COW 路径后恢复
Implementation summary: 统一 DATA/metadata pre-image reservation+publication；接入 ResidentDirty/ZeroDirty 的 write/full ZERO/UNMAP；partial ranges 继续归一化为 DATA；active token 下 kind/epoch/generation 全路径 fail-closed；metadata 计 256 bytes、零 payload；budget/cancel/backing failure 回滚；跨 range 保留 M2 已完成前缀语义
Focused verification / exit code: Debug/Release focused 各 55/55；Debug repeat 20/20；Release repeat 100/100；m2.1-v2 1/1，均 exit 0
Cumulative verification / exit code: unit.m2 ranges/snapshot/backing_generation/write 4/4；m2-v3 6/6；full Debug 59/59；full Release 60/60，均 exit 0
Evidence paths and hashes: tests/unit/dynamic_block_store_consistent_snapshot_tests.cpp；Debug/Release focused binary SHA-256 C9E4A68F855E0B36B1C97715E896BBF3E189E17AD3785DB3AE0B15555C33D059 / C4BB14591DB001AF06EEAEBBA5BD9FB06952B6697734FD4DA370728625E05298；docs/validation/m3-persistence-acceptance.md
SPEC/PLAN/TASKLIST drift audit: 未改 M1/AIM/IMDPROXY；未改 M2 外部 block、非原子跨块前缀、install-before-ack 或 generation 语义；未实现 Task06 batch/finish/abort、durability、文件格式、删除权限或 M4 调度
Independent review: 规格复审经历两轮 NO-GO 修复后 GO；独立代码质量复审 GO；P0/P1/P2 均为 0。Debug map-container 基础设施计费、真实 COW no-resurrection 顺序、final short block 与确定性 reader/mutator 交错均已修复并覆盖。
Open risks / follow-up explicitly excluded: Task06 必须用公共 consistent batch 消费本任务 pre-image 后再验证 exact ack；当前仅用最窄 M2 successor/install/exact-ack/ZeroClean no-fallback 路径锁定 no-resurrection 等价语义。Task06–09 与 M2.1/M3 最终验收仍未完成
Suggested commit: feat(storage): snapshot zero and discard transitions
```

## Task 06 — Token-scoped batch、finish 与 abort

**类别：O。允许文件：** consistent snapshot API 实现、cursor/batch tests。

- [ ] 红灯覆盖 deterministic ascending scan、cursor 必须等于 token frontier、batch byte/record limit、partial final block、原 entry 与 pre-image 二选一、无重复/遗漏。
- [ ] batch 先有界 staging 且不删除 COW；返回对象拥有 payload 后才原子删除覆盖的 COW、推进 caller cursor + coordinator frontier、唤醒 writer。首条前取消零进度，已有安全进度则返回 partial batch 并在下次观察取消。
- [ ] 1 TiB geometry 仅一个 dirty block 的复杂度红灯：通过 sparse directory 跳过 absent leaves/clean ranges，不逐 logical block 扫描、不创建 leaf；访问量与 populated leaves + records 相关。
- [ ] `finish` 仅允许 FullyScanned；提前 finish 返回 `SnapshotNotFullyScanned`，不破坏 active token。
- [ ] token 未 FullyScanned 时 `install_backing_view()`/`mark_persisted()` 返回 `SnapshotBusy`，避免 cut member 被移除；FullyScanned 后按原有 generation/exact ack 规则允许，供 install-before-ack 流程使用。
- [ ] abort 幂等策略、错误 token、旧 token、close token 和 cursor 复用必须有稳定结果。
- [ ] `mark_persisted` 仍按 exact version/kind/generation/dirty_epoch 条件确认；不得因 token 存在而早确认。
- [ ] 跑 `m2.1-v2` 和全部 snapshot/ack 回归；建议提交 `feat(storage): stream consistent snapshot batches`。

## Task 07 — 压力、stall、close 和资源清理

**类别：O。允许文件：** snapshot pressure/lifecycle 实现、fake clock、资源测试。

- [ ] 红灯填满 COW budget，验证 writer 最多等待配置上限后 snapshot 原子 abort，writer 继续。
- [ ] writer 不得持有 block stripe 等待 COW capacity：满额时 release stripe → wait → reacquire → revalidate；成功 reservation 先发布 Preparing marker。加入“cursor 正在 blocked block、budget 被另一 block 占满”的死锁回归。
- [ ] 覆盖释放 consumed pre-image 唤醒 writer、多个 blocked writers、公平性、deadline/cancel 与 spurious wakeup。
- [ ] close 阻止新 begin/mutation，唤醒等待者，drain active mutation/read，释放 token/COW/handles。
- [ ] stats current/peak/abort/stall/blocked 必须与 invariant 对齐，失败路径归零 current accounting。
- [ ] 跑 `m2.1-v3`、`m2.1-v5`、完整 M2 CTest；建议提交 `fix(storage): bound snapshot pressure and shutdown`。

## Task 08 — 独立整盘快照参考模型

**类别：S；oracle 由不同执行者冻结。允许文件：** `tests/model/`、`tests/support/`、测试注册；不改产品实现。

- [ ] 模型只使用简单 block map、cut event 和操作日志，不 include/call production coordinator/codec。
- [ ] 生成跨块 read/write/zero/unmap/evict/load/begin/copy/finish/abort 随机 schedule，至少 100,000 操作。
- [ ] 每个完成 token 的 block set/kind/payload 必须等于同一线性化切点；保存 seed 和最小失败 trace。
- [ ] 加定向 schedule 覆盖所有此前竞态，不允许 production author 修改 expected outcome。
- [ ] 跑 `m2.1-v4` 和 M2 全套；建议提交 `test(storage): model whole disk snapshot cuts`。

## Task 09 — 签发 M2.1 前置 verdict

**类别：O。允许文件：** `docs/validation/m2.1-consistent-snapshot-acceptance.md` 与机器生成 evidence；只修复证据揭示的 M2.1 缺陷。

- [ ] 从 clean Release build 跑 `m2.1-v0..v5`、聚合后的官方 `m3-v3` 和完整 CTest，记录提交、机器、工具链、命令和退出码。
- [ ] 审计 COW 峰值、writer stall、自动 abort、线程/handle/VM/accounting 和 deterministic seeds。
- [ ] 明确 verdict 为 `GO`、`NO-GO` 或 `BLOCKED`；任一必需 gate skip/fail 不能写 GO。
- [ ] 只有 GO 后才允许 Task 28/30 使用 M2.1；基础 codec 任务可按依赖并行准备。
- [ ] 建议提交 `docs(m2.1): issue consistent snapshot prerequisite verdict`。

## Task 10 — CRC32C、streaming SHA-256 与 checked byte codec

**类别：C。允许文件：** `src/common/crc32c.*`、扩展 `sha256.*`、`src/persistence/byte_codec.*`、对应 unit tests/CMake。

- [x] 用公开标准向量和独立 golden bytes 写红灯；覆盖空输入、分段一致性、unaligned buffer 和大长度。
- [x] CRC32C 必须是 Castagnoli 多项式；streaming SHA 结果与现有 one-shot BCrypt 完全一致。
- [x] codec 只做 fixed-width little-endian、GUID、checked offset/length/alignment；无 reinterpret-cast struct serialization。
- [x] truncate/overflow 测试在分配前失败；reserved bytes 的 zero-check 有测试。
- [x] 跑 `m3-v1` 中当前可用项目和全部 common crypto 回归；验收证据见 `docs/validation/m3-persistence-acceptance.md`。

## Task 11 — Exact Windows file I/O 与 sparse fault fake

**类别：C。允许文件：** `src/persistence/file_io.*`、`tests/support/fault_file.*`、I/O tests。

- [x] 红灯覆盖 exact read/write、EOF、short I/O、offset overflow、flush、size、sparse range、cancel/deadline 和 stable Win32 mapping。
- [x] 生产 wrapper 循环处理 partial I/O，但 zero progress 立即失败；每次计算都 checked。
- [x] fake 能按 phase/call/byte 注入 short write、disk full、torn write、flush error、readback corruption 与 close error。
- [x] multi-terabyte sparse fake 的 open/one-range read 保持 bounded calls/memory，产品不 whole-file map；fault injection 只可由显式 test build/object 启用，production 默认 no-op。
- [x] 证明 persistence library 不依赖 driver/proxy，fake 与生产接口行为对齐。
- [x] 跑 `m3-v1` 中当前可用项目；建议提交 `feat(persistence): add checked file io`。

## Task 12 — 目录约束、文件身份与单 writer lock

**类别：C。允许文件：** `src/persistence/image_paths.*`、`file_identity.*`、`writer_lock.*`、tests。

**执行记录（READY_FOR_REVIEW）：**

- Executor / timestamp: Codex / 2026-08-04T20:36:56+08:00
- Start commit / branch: `a280de5e31f485b31cef75db1d3aa76be1840a9b` / `feature/m3-task12`
- Allowed files: `src/persistence/image_paths.*`、`file_identity.*`、`writer_lock.*`、对应 tests/CMake、本任务状态与记录。
- Dependencies: Task 11 为 `DONE`；基线提交包含 `49f9731`、`dad0c6d`、`42b71ff`、`68a4190` 与验收提交 `a280de5`。
- Baseline: `cmake --preset msvc`、Release build、`ctest --preset release -L m3-v1 --output-on-failure`（2/2）、完整 Release CTest（61/61）均退出 0。
- Assumptions: 文件名仍只允许一个 UTF-8 相对 component；Task 12 仅定义最小稳定错误类型并标注 Task 13 迁移点；不实现 manifest codec 或 Task 13 outcome。
- Actual files: `src/persistence/file_identity.*`、`image_paths.*`、`writer_lock.*`、`src/CMakeLists.txt`、`tests/unit/m3_persistence_path_tests.cpp`、`tests/helpers/m3_writer_lock_helper.cpp`、`tests/CMakeLists.txt`、本记录。
- Red tests: Debug target 首次因缺少 `file_identity.h`、第二次因缺少 `writer_lock.h` 编译失败（C1083，退出 1）；新增 hardlink-after-validation 期望 `HardLink`、case-sensitive sibling、按值 manifest identity static assert、intermediate junction swap 均先稳定失败（退出 1），再逐项实现转绿。
- Implementation: 严格 UTF-8 单 component/DOS device 校验；handle-opened normalized final path；root/target/target-parent volume serial + 128-bit file ID 与 size/attrs/reparse/link-count；reparse/hardlink fail-closed；handle-anchored inspection/mutation capability；cleanup 前重新验证；manifest identity 绑定的进程内竞态防护 + 跨进程 `LockFileEx`；正常/异常退出 RAII 释放。
- Verification: Debug focused 1/1；Release focused 1/1；Release high-risk repeat 20/20；当前 `m3-v1` 3/3；完整 Release 62/62；`git diff --check` 退出 0。
- Self-review: 逐文件审查发现并修复 case-sensitive `Bundle`/`bundle` 父目录误判、中间 junction 换向后从原始路径 reopen、`manifest_identity()` 悬空引用，以及 cleanup 后增 hardlink 的错误分类；所有缺陷均有回归测试。
- Evidence: `tests/unit/m3_persistence_path_tests.cpp` SHA-256 `6F546F01BEC4C316EEC030495B1D71126EFDDCF223EF2C76EA957EC4134DEF05`；Release `myrd_m3_persistence_path_tests.exe` SHA-256 `5E0AFF0B6C4BA54E49D5D6F33E2E9B4DA1F4C790EA756A6F1647BC8F861ABFC3`。
- Drift audit: 未修改 M1 protocol、AIM contract、M2 block 语义、checkpoint durability 顺序或 M4 边界；仅新增 Task 12 最小稳定错误码并明确 Task 13 迁移；cleanup capability 只授予路径/identity/writer 权限，Task 36 仍必须另外证明 manifest/BackingView/in-flight/reconciliation retention 条件。
- Open risks / excluded follow-up: Task 13 合并完整 persistence error context；后续 codec 必须消费/适配已验证 handle，禁止按 `final_path()` 重新打开；Task 36 实现引用保留策略。独立审查委派因 Claude API `ConnectionRefused` 未产出结论，因此本任务按用户要求停在 `READY_FOR_REVIEW`，不置 `DONE`。
- Suggested commit: `feat(persistence): secure image paths and identities`。

- [x] 红灯覆盖相对/绝对逃逸、`..`、symlink/junction/reparse、hard link、case/Unicode、替换文件和 concurrent writer。
- [x] 路径先 canonicalize 再验证在 bundle root；打开后以 volume serial/file ID/size 等身份复核，禁止只信文件名。
- [x] writer lock 绑定 manifest identity；读者可并发，第二 writer 返回稳定 Busy，不偷锁。
- [x] cleanup API 只接收已验证 bundle-owned identity。
- [x] 跑 `m3-v1` 中当前可用项目；建议提交 `feat(persistence): secure image paths and identities`。

## Task 13 — Persistence 稳定类型、错误与 outcome

**类别：C。允许文件：** `src/persistence/persistence_types.*`、`persistence_error.*`、类型/JSON tests。

- [ ] 红灯覆盖 geometry、identity、manifest/journal/root/commit descriptor、phase/session enum 和 stable strings。
- [ ] 定义 caller-supplied `PersistenceLimits`：virtual/journal records、index/BAT cache bytes+entries、I/O scratch、verify batch；zero/overflow/单条记录放不下/隐藏 unlimited 全部拒绝。
- [ ] 冻结 `NotCommitted`、`CommitIndeterminate`、`Committed`、`CommittedNeedsReconciliation`，不得折叠为 bool 或普通 `Result` error。
- [ ] 冻结 `VolumeBarrierKind`：真实 filesystem 只有 exact volume flush 成功后可标 `WindowsVolumeFlushed`；raw/local test 必须显式标 `RawBlockTestNoFilesystemCache` 并禁止产生 filesystem durability claim。
- [ ] error 保存 phase、stable code、原生 code 与可选安全 context；未知 enum/字段 fail-closed。
- [ ] 每个 public error/outcome context 都有 operation、phase、Win32 code（适用时）、image UUID、operation id、generation 和 commit disposition；默认值与脱敏有红灯。
- [ ] 序列化不依赖 enum ordinal/ABI；checked generation increment 拒绝回绕。
- [ ] 跑 `m3-v1` 中当前可用项目；建议提交 `feat(persistence): define stable persistence contracts`。

## Task 14 — 编解码 4096-byte manifest slot

**类别：C。允许文件：** `manifest_format.*`、manifest unit tests、`tests/fixtures/m3/manifest-v1.hex`。

- [ ] 独立 hand-built fixture 锁定每个字段的 exact bytes、CRC 位置和 reserved zero。
- [ ] 每个字段、长度、UTF-8、geometry、identity、generation/root/commit 关系、unknown mandatory flag 都有 corruption 红灯。
- [ ] encode 先清零固定 4096-byte buffer；CRC 计算时 CRC 字段视为零；decode 禁止容忍 reserved semantic data。
- [ ] 对有效值随机 round-trip，并在每个 byte boundary truncate。
- [ ] 跑 `m3-v1`；建议提交 `feat(persistence): add manifest slot codec`。

## Task 15 — 选择最高“完全有效”的双槽 candidate

**类别：O。允许文件：** `manifest_selector.*`、独立 reference model/fixtures/tests；不改 wire format。

- [ ] 独立执行者冻结 A/B valid/invalid、equal generation conflict、higher incomplete、fallback、path escape fixtures。
- [ ] selector 先按 generation 倒序，但只有 manifest、VHDX identity/layout、journal commit/digest/root/length 全部有效才可选择。
- [ ] equal-generation durable fields 不完全相同时返回 split-brain corruption，不凭 slot index 猜。
- [ ] 记录每个 rejected slot 的第一稳定原因和 fallback；corruption 不能当作不存在。
- [ ] 跑 oracle/corruption tests 与 `m3-v2` 中当前可用项目；建议提交 `feat(persistence): select complete manifest generations`。

## Task 16 — Journal header、records、commit 与 chain digest

**类别：C（digest 由逻辑负责人审查）。允许文件：** `journal_format.*`、golden fixtures/tests。

- [ ] exact-byte 红灯覆盖 header、DATA/ZERO/DISCARD/INDEX_NODE/CHECKPOINT_COMMIT、4096 alignment、padding、final block。
- [ ] 独立 SHA fixture 锁定 canonical record order、previous digest、epoch/generation/root/payload；任何 byte 改变都改变结果或 CRC 失败。
- [ ] decode 在 allocation 前检查 type/length/alignment/overflow/UUID/generation；unknown mandatory type fail-closed。
- [ ] genesis 与跨 epoch predecessor digest 按 SPEC 实现。
- [ ] 跑 truncate/fuzz 和 `m3-v1`；建议提交 `feat(persistence): add journal wire format`。

## Task 17 — Immutable B+tree reader

**类别：S。允许文件：** `journal_index.*`、reader tests；Claude 可写 parser/cache，逻辑负责人审查 corruption。

- [ ] 红灯覆盖 empty/leaf/multilevel/boundary/absent、unsorted/duplicate/cycle、bad CRC/child/range、depth 9 和 cache eviction。
- [ ] iterative lookup 最大深度 8，visited-offset 防环；所有 read 在 manifest committed length 内。
- [ ] cache key 包含完整 file identity/epoch/root generation/node offset，不能跨 view 混用。
- [ ] 一百万随机 lookup 与 `std::map` 对比；固定 seed。
- [ ] 跑 `m3-v1`；建议提交 `feat(persistence): read immutable journal index`。

## Task 18 — COW index builder

**类别：S。允许文件：** 扩展 `journal_index.*`、builder tests；Claude 写 tests/pure shell，逻辑负责人审查 path-copy。

- [ ] 红灯覆盖 insert/replace、DATA↔ZERO、root/leaf/internal split、unchanged subtree reuse 和输入顺序无关的 deterministic bytes。
- [ ] 同一 checkpoint 重复 block update 必须拒绝，不能 last-write-wins 掩盖 snapshot bug。
- [ ] 先生成纯内存 node plan，再 checked assign append offsets，最后 encode；写失败不影响旧 root。
- [ ] 至少 100,000 updates 后每个 new root 与 map oracle 一致。
- [ ] 跑 `m3-v1`；建议提交 `feat(persistence): build copy-on-write journal indexes`。

## Task 19 — Append、flush 并 readback 完整 checkpoint

**类别：S。允许文件：** `journal_writer.*`、writer fault tests；durability 顺序由逻辑负责人签字。

- [ ] 为 data/node/commit/flush/readback 每一 phase 写 fault 红灯，含 short write、disk full、cancel、deadline。
- [ ] 只从 manifest committed length 追加，不覆盖已提交 byte；padding 清零并 streaming 计算 canonical digest。
- [ ] 完整 commit 后 flush 一次，再重读所有新 byte 独立复算 CRC/digest/end。
- [ ] 返回 PreparedCheckpoint，不发布 manifest、不 install、不 ack。
- [ ] 证明任一 pre-manifest failure 只留可忽略 tail；跑 `m3-v1`/fault tests。
- [ ] 建议提交 `feat(persistence): prepare durable journal checkpoints`。

## Task 20 — JournalReader 解析 committed blocks

**类别：S。允许文件：** `journal_reader.*`、reader tests；corruption policy 由逻辑负责人审查。

- [ ] 红灯覆盖 DATA/ZERO/DISCARD/Absent 和 descriptor-valid/payload-corrupt/type/index/generation mismatch。
- [ ] DATA exact read 后同时验证 record、descriptor、payload CRC 与 block identity。
- [ ] 任何损坏返回 `BackingDataCorrupt`，禁止 fallback 为 Absent/older layer。
- [ ] read 不越 manifest committed length，支持 cancel/deadline 和 immutable concurrent lifetime。
- [ ] 跑 journal/index/format tests 与 `m3-v1`；建议提交 `feat(persistence): read committed journal blocks`。

## Task 21 — Strict VHDX structural parser

**类别：C。允许文件：** `vhdx_format.*`、minimal fixtures/tests。

- [ ] golden bytes 覆盖 identifier、双 header、双 region table、metadata entries、geometry 与 BAT。
- [ ] 红灯覆盖坏 signature/checksum/GUID、conflicting sequence、missing/duplicate/overlap、overflow/truncate 和 unsupported flags。
- [ ] 只接受 SPEC 冻结的 dynamic non-differencing profile；双 header/region 副本分别选最高有效项。
- [ ] 输出 immutable `VhdxLayout`，禁止返回 raw writable structure 或接受未知 mandatory feature。
- [ ] 以 Microsoft MS-VHDX 为规范逐字段审查，跑 `m3-v1`；建议提交 `feat(persistence): parse strict vhdx profile`。

## Task 22 — Lazy VHDX BAT reader

**类别：S。允许文件：** `vhdx_reader.*`、reader tests；BAT state 语义由逻辑负责人审查。

- [ ] 红灯覆盖 fully-present/not-present/undefined/zero/unmapped、final partial、512/4096 sectors 和 2 MiB→64 KiB 映射。
- [ ] partially-present、invalid bits、payload overlap/out-of-file/before-data、overflow/truncate 都 fail-closed。
- [ ] BAT page cache 有界且按 immutable base identity 分区；读取一个 block 不加载整 VHDX。
- [ ] allocated extent enumeration 只读 metadata/BAT，返回 sorted unique checked ranges。
- [ ] 随机 sparse map oracle 与 `m3-v1` 通过；建议提交 `feat(persistence): read sparse vhdx blocks lazily`。

## Task 23 — Sparse dynamic VHDX writer

**类别：S。允许文件：** `vhdx_writer.*`、writer tests；publish/flush/sparse 由逻辑负责人审查。

- [ ] 红灯覆盖 empty/sparse/large/final partial/all-zero/non-contiguous payload 与全部 I/O fault phase。
- [ ] mutation 前先完成 checked deterministic layout；metadata/BAT 先规划，payload 对齐，reserved 清零，zero block 不分配。
- [ ] 写 same-directory unique temp，flush/close/reopen product verify 后 atomic rename；不覆盖任何 referenced base。
- [ ] 每个 failure point 不返回 partial final name，且保留可诊断 cleanup outcome。
- [ ] 跑 `m3-v1`；建议提交 `feat(persistence): write sparse dynamic vhdx bases`。

## Task 24 — Windows native VHDX compatibility gate

**类别：C。允许文件：** `tests/integration/m3_windows_vhdx_compat_tests.cpp`、`tests/e2e/test-m3-vhdx-compat.ps1`、测试注册。

- [ ] capability probe 区分 non-admin/API unavailable；机器可读 SKIP reason，不允许静默 pass。
- [ ] product 生成 empty/sparse/boundary/512/4096 fixture，通过 `OpenVirtualDisk`、read-only attach、geometry/data sample、detach。
- [ ] product reader 打开一份 Windows native-created compatible VHDX 并比较 geometry/data。
- [ ] finally cleanup；禁止 VHDX 与同一 AIM RAM disk 在线双挂载。
- [ ] 跑 `m3-v5` native gate；建议提交 `test(m3): add native vhdx compatibility gate`。

## Task 25 — Journal-first composite backing

**类别：O。允许文件：** `composite_backing_store.*`、truth-table/instrumentation tests。

- [ ] 独立冻结 Journal DATA/ZERO/DISCARD/Absent × VHDX data/zero/absent 真值表。
- [ ] DATA 胜出；ZERO/DISCARD 遮蔽旧 base；只有 Absent 才 fallback；corruption 永不 fallback。
- [ ] immutable view/cache identity 隔离红灯：旧 reader 与新 generation 并发时各自看到固定映射。
- [ ] 计数证明 open 只做有界 metadata/index read，first touch 只读相关 BAT/index/payload。
- [ ] 跑 `m3-v2` 中 recovery/layering 项；建议提交 `feat(persistence): compose immutable backing layers`。

## Task 26 — Recovery engine 与 immutable BackingView

**类别：O；oracle 由不同执行者冻结。允许文件：** `recovery_engine.*`、recovery tests/fixtures。

- [ ] fixture 覆盖 A/B valid、newer torn fallback、both invalid、missing/replaced/truncated、ignorable tail、split-brain、unsupported。
- [ ] 每个 slot 依次做完整 bounded validation；只有 manifest/base/journal/commit/digest/root/length 全有效才候选。
- [ ] selected candidate 构造 immutable BackingView 和持有的 handles；recovery 只读，不 truncate/repair/ack/delete。
- [ ] diagnostics 记录 rejected reason/fallback，默认不泄漏 absolute path。
- [ ] 与独立 oracle 比较并跑 `m3-v2`；建议提交 `feat(persistence): recover complete image generations`。

## Task 27 — Generation-zero image create/reopen

**类别：S。允许文件：** `image_creator.*`、create integration tests；首次 publish 由逻辑负责人审查。

- [ ] 红灯覆盖 geometry、existing/path escape/low-space/all I/O faults/concurrent creators。
- [ ] 顺序固定：unique empty base → journal genesis → flush/readback → exact 8192-byte manifest（inactive slot zero/invalid）→ generation-zero slot → flush/full validation。
- [ ] 成功必须通过 Task 26 production recovery path reopen，边界 blocks 为 zero；禁止 create shortcut 自证成功。
- [ ] retry 不覆盖 published image，不引用 incomplete artifact。
- [ ] 跑 `m3-v2` 中 create/recovery 项；建议提交 `feat(persistence): create generation zero images`。

## Task 28 — 将 recovered backing 接入 M2 first-touch load

**类别：O。允许文件：** storage backing adapter/install/lifecycle、`m3_lazy_backing_tests.cpp`；不改 M1 wire。

- [ ] 依赖 Task 09 必须为 GO；否则本任务 BLOCKED。
- [ ] 红灯证明 open 不 eager payload、同 block single-flight、writer-during-load ordering、ZERO/DISCARD/Absent/corruption/cancel/close。
- [ ] 只通过现有 `install_backing_view()` 发布完整 immutable view；旧 view 活到最后 in-flight read 释放。
- [ ] eviction/reload 必须从相同 selected generation 获取；dirty/snapshot-protected block 不错误淘汰。
- [ ] 跑 `m3-v3`、focused lazy-I/O metrics、M2 lazy/eviction/concurrency 和完整 CTest；这只是 M3-V7 前置证据。建议提交 `feat(m3): load recovered blocks on first access`。

## Task 29 — Image session state 与 structured outcome

**类别：O。允许文件：** `image_session.*`、state tests。

- [ ] table-driven 红灯穷举 Opening/Ready/Checkpointing/Compacting/ResolvingCommit/Reconciling/Closing/Closed/PersistenceFaulted 转换。
- [ ] serialized operation gate 阻止第二个 publish-affecting operation，但 immutable reads 可继续。
- [ ] first failure 与 CommitDisposition 贯穿 API；Indeterminate 进入 ResolvingCommit 并阻止 mutation/checkpoint。
- [ ] close 幂等并按规定 drain，不能用内存 state 伪装 durable success。
- [ ] 跑 `m3-v2` 中 session/outcome 项；建议提交 `feat(persistence): enforce image session state`。

## Task 30 — Checkpoint 的 pre-manifest durability pipeline

**类别：O。允许文件：** `checkpoint_coordinator.*`、checkpoint integration tests；不实现 manifest publish。

- [ ] 依赖 Task 09 GO；missing/unknown volume barrier 在 begin 前失败；红灯锁定 begin snapshot → stream token once → append records/index/commit → flush → readback → Prepared。
- [ ] DATA/ZERO/DISCARD、overwrite-after-cut、clean image、partial final、cancel/deadline、每个 fault phase 全覆盖。
- [ ] batch bounded streaming，不建立 disk-sized snapshot；持久 append 后立即释放临时 payload。
- [ ] 任一 pre-manifest-attempt failure abort token、保留旧 view/manifest、返回 `NotCommitted`，tail 可忽略。
- [ ] 跑 `m3-v2` 中当前 checkpoint 项和 M2.1 snapshot gates；建议提交 `feat(persistence): prepare checkpoint durability`。

## Task 31 — Manifest publish 与 CommitIndeterminate resolver

**类别：O；crash oracle 独立冻结。允许文件：** 扩展 coordinator、`commit_resolver.*`、publish tests。

- [ ] alternate-slot exact 4096-byte write 的 before/during/after、flush、readback 全 phase fault 红灯。
- [ ] 顺序固定：inactive slot encode → exact write → flush manifest → reread → full candidate validation。
- [ ] slot-write attempt 前失败为 `NotCommitted`；attempt 后 slot write/manifest flush 报错为 `CommitIndeterminate`；manifest flush 成功即建立 `Committed`。其后 readback/validation 失败只能是 `CommittedNeedsReconciliation`。
- [ ] resolver 关闭/重开 handles、走 normal recovery，并在新 candidate 完整时重做 manifest flush；新代完整且 flush 成功则 reconcile，attempted slot 被证明无效且旧代唯一完整才 abort 为 `NotCommitted`，其余 PersistenceFaulted。
- [ ] 禁止依据 cache、write 返回值或 FlushFileBuffers 返回值猜测 durable generation。
- [ ] 跑 `m3-v2`；建议提交 `feat(persistence): publish and resolve checkpoint commits`。

## Task 32 — Install backing、conditional ack 与 reconciliation

**类别：O。允许文件：** coordinator/session/storage integration、reconcile tests。

- [ ] 红灯锁定 durable proof → recover/build new view → install → exact conditional ack → finish token → success。
- [ ] after-cut mutation：旧 pre-image 已提交，但更新后的 dirty epoch 仍 Dirty。
- [ ] durable commit 后 install/ack/finish failure 返回 `CommittedNeedsReconciliation`；reopen 必须选新 generation，可重试。仅全部必要 reconciliation 完成后返回 `Committed`。
- [ ] COW pre-images 只在每个 token member 已 conditional ack 或已确认 newer dirty 后释放。
- [ ] 跑 `m3-v2`、`m3-v3` 中 ack concurrency 项、M2 ack/eviction 与完整 CTest；建议提交 `feat(m3): reconcile durable checkpoints with live memory`。

## Task 33 — 独立 checkpoint crash model

**类别：O；expected outcomes 由非生产实现作者维护。允许文件：** `tests/model/m3_checkpoint_crash_model_tests.cpp`、crash fixture、test registration；不改 production。

- [ ] 枚举 record/node/commit/journal flush/readback/manifest write/flush/readback/install/ack 每个 durability boundary 与 torn/short write。
- [ ] 每个点丢弃全部进程内存，从 cloned bytes 新进程式恢复；结果只能旧完整代或新完整代。
- [ ] 独立 map oracle 不调用 production codec/selector；全盘 block compare，禁止只比 generation number。
- [ ] CI 至少 10,000 deterministic schedules，失败保存 seed/phase/bytes hash。
- [ ] 跑 `m3-v2`；建议提交 `test(m3): prove checkpoint crash atomicity`。

## Task 34 — Enumerate live extents for compaction

**类别：S。允许文件：** `live_extent_enumerator.*`、extent tests；snapshot identity 由逻辑负责人审查。

- [ ] 红灯覆盖 journal DATA/ZERO/DISCARD 遮蔽 VHDX、final partial、contiguous merge 与 sparse holes。
- [ ] 捕获一个 immutable BackingView 后按 block ascending bounded stream；禁止读取 live mutable RAM 作为 compaction source。
- [ ] zero ranges 不写 payload，每个 DATA 在交给 writer 前完成 source integrity validation。
- [ ] randomized full-map oracle 比较 descriptors 和 payload hashes。
- [ ] 跑 `m3-v4`；建议提交 `feat(persistence): enumerate live image extents`。

## Task 35 — Compact to a new base and journal epoch

**类别：O。允许文件：** `compaction_coordinator.*`、compaction tests。

- [ ] 整个 compaction 持有 session 的唯一 modifying-operation gate：capture durable view G → write/flush/readback/verify unique VHDX(G) → write-through rename → create/flush journal epoch E+1。
- [ ] 构建 VHDX 期间普通 live mutation 可继续，但 checkpoint/第二个 compaction 返回 busy；新 base/journal 就绪后验证本 operation 的显式 volume barrier，再建立 M2.1 一致切点。
- [ ] 把相对 G 的 cut-time dirty DATA/ZERO/DISCARD 流入新 journal，构建 index/commit G+1（即使 dirty set 为空也写 commit），再走正常 manifest publish/indeterminate resolution/install/conditional ack。
- [ ] pre-commit failure 保留旧 bundle/runtime authoritative，新文件仅为 unreferenced artifacts；post-commit recover/install new view，旧 files 继续服务旧 slot/reader/reconciliation。
- [ ] 用 journal physical bytes、journal/base ratio、checkpoint count、index depth 红灯并实现 pure `should_compact()`；只返回建议，不自行调度。
- [ ] 用独立模型比较切点时全盘 logical hash/metadata descriptor，reopen 后完全相等；跑 `m3-v4`。
- [ ] 建议提交 `feat(persistence): compact committed image generations`。

## Task 36 — Safe retention and cleanup of immutable files

**类别：O。允许文件：** `retention_manager.*`、retention/race tests。

- [ ] reference set 同时包含两个 fully valid manifest slots 与全部 live BackingView identities。
- [ ] unknown/replaced/hard-linked/path-escaping file 永不自动删除；identity 必须归属当前 bundle。
- [ ] 仅当无 valid slot、无 live view 引用时可删；sharing violation/crash 只留下 harmless artifact 和 diagnostics。
- [ ] concurrent read/close/recovery/compaction races 证明最后 handle/reference 释放前不开始 deletion。
- [ ] 跑 `m3-v4`；建议提交 `feat(persistence): retain immutable generation files safely`。

## Task 37 — Inspect、verify 与 machine-readable report

**类别：C。允许文件：** `image_inspector.*`、`m3_report.*`、`m3_evidence_publisher.*`、report tests。

- [ ] deterministic JSON 红灯覆盖 geometry、A/B slots、selected/fallback、identities、root/commit、corruption、cleanup candidates 和 no selection。
- [ ] inspect 只读且 bounded，不 repair/truncate/publish/ack/delete。
- [ ] `verify --full` streaming 校验所有 logically allocated blocks；默认 verify 只做 structural bounded validation。
- [ ] absolute paths 与用户 block contents 默认不输出；schema/version、operation id、source commit、binary/artifact hashes、enum strings fail-closed。
- [ ] evidence publisher 使用 unique same-directory temp → exact write → flush → write-through rename，拒绝覆盖，故障保留诊断；不得依赖 proxy session-id helper。
- [ ] 跑 `m3-v2`、`m3-v4` 中当前可用项与 JSON golden tests；建议提交 `feat(m3): inspect and verify persistent images`。

## Task 38 — M3 CLI with local fake storage

**类别：C（workflow 由逻辑负责人审查）。允许文件：** `src/m3/m3_cli.*`、`src/m3/m3_lifecycle_runner.*`、thin `src/m3/main.cpp`、CLI tests、CMake。

- [ ] 红灯覆盖 SPEC 的 `create/inspect/verify/checkpoint/compact/recover-test/fault-matrix`、invalid args、exit codes、JSON/stdout/stderr。
- [ ] geometry/path/timeout 全显式；CLI 不识别、初始化或格式化 Windows volume。
- [ ] 命令必须走 production session/recovery path，不写 command-specific shortcut。
- [ ] JSON 显示 CommitDisposition/stable error，默认不泄漏完整路径。
- [ ] 跑 local fake lifecycle、`m3-v2` 和 `m3-v4` 中当前可用项；建议提交 `feat(m3): add persistence lifecycle cli`。

## Task 39 — Integrate M3 with M1/M2 provider

**类别：O。允许文件：** `src/m3/m3_provider.*`、`src/m3/m3_windows_acceptance.*`、M3 lifecycle composition root、provider integration tests、CMake；禁止改 M1/M2 provider 行为和 M1 wire fixture。

- [ ] 红灯覆盖 recovered startup、lazy reads、writes、TRIM/zero、checkpoint、provider reconnect、process reopen。
- [ ] 在 composition root 注入 ImageSession/DynamicBlockStore；persistence 不依赖 AIM types，transport 不知道 file formats。
- [ ] stop 顺序：停止外部 mutation → 上层按编排 flush → optional checkpoint → resolve indeterminate → drain/close。
- [ ] 全部 M1 wire golden tests 原样通过；任何 protocol/driver delta 立即 BLOCKED。
- [ ] 跑 M0/M1/M2 完整回归和 provider 本地 integration subset；这不是官方 M3-V6。建议提交 `feat(m3): integrate persistence with provider lifecycle`。

## Task 40 — Safe real-host preflight and E2E harness

**类别：C。允许文件：** `tests/e2e/M3Harness.Common.ps1`、`tests/e2e/test-m3-harness-preflight.ps1`、`tests/e2e/run-m3-persistence.ps1`、`tools/Invoke-M3Verification.ps1`、`tools/Verify-M3Json.ps1`、`docs/testing/m3-e2e.md`。

- [ ] probe admin、AIM interface、artifacts、filesystem、space、power policy、Virtual Disk API，输出 JSON+summary。
- [ ] 必须显式 scratch root 和 test disk identity；拒绝 system volume、workspace root、用户数据盘、ambiguous path。
- [ ] finally 清理 provider/proxy/device/drive letter/VHDX attach/temp；残留给出人工恢复命令。
- [ ] verification wrapper 暴露 Local/Preflight/RealFunctional/Crash/Soak；尚未实现的 tier 必须 BLOCKED/nonzero。JSON verifier 拒绝 schema/source commit/binary hash 不匹配和 required SKIP。
- [ ] 非管理员 dry-run 测所有拒绝规则；destructive step 前再次核对 exact disk identity。
- [ ] 跑 script safety/preflight tests；preflight 通过不是官方 M3-V6 功能通过。建议提交 `test(m3): add safe real-host e2e harness`。

## Task 41 — Real AIM、NTFS、lazy load and VHDX acceptance

**类别：O。允许文件：** `tests/e2e/Invoke-M3NtfsWorkload.ps1`、frozen harness scenarios、机器生成 evidence；不得顺手改 production 绕过失败。

- [ ] create generation zero，real AIM export 后由 harness 明确识别 scratch disk，再初始化/格式化 NTFS、mount。
- [ ] 写 deterministic tree、overwrites、sparse、rename/delete；调用文件系统/volume flush 后 checkpoint。
- [ ] clean stop/reopen，禁止重新分区/格式化，运行声明的 filesystem check 并校验 hashes/metadata；cold memory 小样本读取证明 startup/first-touch bounded，再全量校验。
- [ ] 再次 flush exact volume 后 compact durable NTFS generation，证明新 base_generation hash 对应该完整 durable view；RAM disk offline 后用 Windows native API read-only open/attach，验证 partition/NTFS，再 detach。禁止拿旧 base 对比新 journal 内容或双写挂载。
- [ ] 记录版本、命令、seed、I/O/memory counters、hash、cleanup。格式化仅是 E2E setup，不是 M3 API。
- [ ] `m3-v6` 所有 required scenario 通过；不满足环境写 BLOCKED，不写 GO。

## Task 42 — Simulated and real-process crash matrix

**类别：O；oracle 独立。允许文件：** `tests/e2e/Invoke-M3CrashMatrix.ps1`、`tests/e2e/run-m3-persistence.ps1`、scenario/evidence schema。

- [ ] 在 named durability phases 执行 deterministic file-clone simulated crashes，每次用 fresh process recovery。
- [ ] 执行 real process termination：journal flush 前后、manifest publish 周围、publish proof 后、reconcile 前后。
- [ ] 非真实断电不得声称 power-loss test；报告明确 simulated/process-kill/controlled-power-cut 类型。
- [ ] 每次先 read-only recover，验证 expected old/new generation 与 full corpus hash，再 inspect slot/fallback。
- [ ] mixed generation、silent corruption fallback、unresolved indeterminate、resource leak 或 non-reproducible 全部失败。
- [ ] 跑 `m3-v2` 的 crash/recovery gate、`m3-v6` 的 real kill scenarios 和 `m3-v7` 的重复 soak profile；建议提交 `test(m3): add crash recovery matrix`。

## Task 43 — Soak、resource、performance and final verdict

**类别：O。允许文件：** `tests/e2e/Invoke-M3Soak.ps1`、扩展 `tools/Invoke-M3Verification.ps1`/`tools/Verify-M3Json.ps1`、`docs/evidence/m3/M3_VERIFICATION_REPORT.md`、机器生成报告；缺陷修复回到对应任务。

- [ ] clean Release configure/build/full CTest，保存 exact commit/commands/output。
- [ ] deterministic mixed read/write/TRIM/checkpoint/reopen/compaction soak，含 first-touch concurrency、low-memory、I/O fault、重复 close/reopen。
- [ ] 检查 bounded memory/lazy I/O、COW/stall、journal growth/compaction、handle/thread/device leak、data hashes 与 SPEC thresholds。
- [ ] soak 后复跑 native VHDX、real AIM/NTFS 和 crash required scenarios。
- [ ] 分别列出内部 M3-V0 基线与官方 M3-V1..V7 的 PASS/FAIL/SKIP、artifact hash 和 defect；required SKIP 不能 GO。
- [ ] verdict 只能 `GO`、`NO-GO`、`BLOCKED`；建议提交 `docs(m3): issue persistence milestone verdict`。

## Task 44 — Documentation、license and final drift closure

**类别：C/O。允许文件：** README/status/license notices、M3 文档；若发现代码缺陷回到原任务，不在本任务改算法。

- [ ] 文档说明 supported/unsupported VHDX、bundle 文件、backup/restore、checkpoint、compaction、shutdown、CommitIndeterminate 恢复和限制。
- [ ] inventory 每个 third-party dependency/fixture/tool 的许可与 redistribution；不无意复制 AIM Toolkit user-mode code。
- [ ] 逐行对照 SPEC/PLAN/TASKLIST，列出每个 frozen contract 的实现与 test/evidence 路径。
- [ ] 从 clean tree 重跑 docs commands、Debug build/full CTest、Release build/full CTest 和最终 required gates。
- [ ] 只有 Task 43 GO 且所有漂移关闭才将本任务和 M3 标为 DONE。
- [ ] 建议提交 `docs(m3): close persistence milestone`。

---

## F. 最终验收核对表

- [ ] 00–44 全部 `DONE`，没有超过一个 `DOING`，没有未解释的 `BLOCKED`。
- [ ] 每个任务都有红灯、聚焦测试、累计门禁、偏航检查和单独提交记录。
- [ ] M2.1 在并发下给出一个全盘一致、内存有界、stall 有界的切点。
- [ ] 创建、增量 checkpoint、重开、fallback、lazy load、compaction、retention 走 production path。
- [ ] 任一 crash point 只恢复旧完整代或新完整代，永不混代。
- [ ] dirty ack 只在 exact successor view install 后按 exact epoch/version/kind 条件发生。
- [ ] product VHDX 通过 Windows native API；real AIM+NTFS 数据往返通过。
- [ ] M0/M1/M2 回归、内部 M3-V0 基线和官方 M3-V1..V7 必需门禁在最终提交上新鲜通过。
- [ ] 报告没有把 SKIP、模拟断电或旧 evidence 包装为 GO。

最终执行记录：

```text
Final commit / branch:
Toolchain / OS / AIM driver:
Clean-tree status:
M2.1-V0..V5 results:
Internal M3-V0 / official M3-V1..V7 results:
Native VHDX result:
Real AIM+NTFS result:
Crash matrix result:
Soak/resource result:
Evidence paths and hashes:
Known limitations:
Final verdict and signer:
```

---

## G. 推荐执行顺序

1. 先让 Claude Code 按 Task 00、01、10–14、16、21 的依赖顺序完成 C 类基础层，每次只发一个任务并由我们 review。
2. 我们主导 Task 02–09，先把 M2.1 一致切点做成 GO；这是后续 checkpoint 正确性的地基。
3. Claude Code 为 S/O 任务先写红灯、fixture、fake 和 instrumentation；我们实现/审查 Task 15、17–20、22–33 的核心算法。
4. 再完成 compaction、retention、CLI/provider wiring；最后在明确 scratch disk 的真实环境执行 Task 40–44。

这种分工允许 Claude Code 承担大部分可机械验证的基础工作，同时把最容易“测试看似通过但真实会丢数据”的决策留给受控评审。
