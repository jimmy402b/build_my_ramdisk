# M3 Remaining Work Microtask List

本文件把 `M3_TASKLIST.md` 中尚未完成的 Task 07–44 拆成可由执行型 Agent
逐张完成的微任务卡。它不重新定义产品；权威顺序仍是：

1. `docs/superpowers/specs/2026-08-04-m3-vhdx-journal-checkpoint-design.md`
2. `docs/superpowers/plans/2026-08-04-m3-persistence-engine.md`
3. `M3_TASKLIST.md`
4. 本文件

任一上级文档与本文件冲突时，立即停止，不得自行选择方便的解释。

## 1. 给非 SOTA Agent 的硬规则

每张卡有推理等级：

- `M`：机械实现。Medium 模型可独立完成。
- `H`：高风险语义。Medium 模型只能准备测试、fixture、接口壳和诊断；生产
  逻辑必须等待 High 模型复核后继续。
- `X`：线性化、durability、recovery、删除或最终 verdict。非 High/XHigh
  模型执行到该卡时必须写出 `PAUSED_FOR_HIGH_REASONING: <card-id>` 并停止。

禁止模型谎称自己提升了推理等级。禁止把 `H/X` 卡改成 `M`。禁止在一张卡内
开始下一张卡。每完成一张卡，必须重新阅读 PLAN 同编号任务和 SPEC 对应章节，
逐项回答：

```text
Card / result:
Start commit / worktree:
Allowed files / actual files:
RED command / expected failure / observed failure / exit code:
GREEN command / test count / exit code:
PLAN re-read:
Drift check: M1 protocol / AIM contract / M2 semantics / durability order /
             error class / deletion authority / M4 boundary
Next card or PAUSED_FOR_HIGH_REASONING:
```

任一项没有证据时，该卡不是完成。RED 因编译器、环境或无关旧测试失败，不算
目标红灯。不得扩大 timeout、删除断言、重试到绿、改 oracle 迎合实现，或在
未证明 durable commit 时清理旧文件/ack dirty state。

## 2. 每张卡的统一执行事务

1. 运行 `git status --short`；工作树非预期不干净就停止。
2. 记录 HEAD，确认 `M3_TASKLIST.md` 的依赖任务为 `DONE`。
3. 只打开卡片列出的文件和 PLAN/SPEC 对应段落；不得“顺手重构”。
4. 先写最小 RED，并确认失败点正是卡片要求。
5. 只实现使该 RED 转绿的最小改动。
6. 运行卡片 focused gate、受影响旧测试和 `git diff --check`。
7. 回读 PLAN，写上面的执行记录；审查通过前不进入下一卡。
8. 每张卡通过本卡 review 后生成一个可审查提交；独立 oracle/fixture 必须先单独
   提交并记录 SHA，后续 production 卡把这些路径列为只读。父 Task 是一组按序
   micro-commits；所有卡和独立复审通过后才更新父 Task 状态。

缺 header/symbol 导致的 compile/link failure 只算 wiring bootstrap，不算语义 RED。
registration 卡必须先补最小可编译 declaration/stub，再让目标 test case 在冻结断言
处稳定失败，之后才能实现 GREEN。

运行任何 CTest gate 前，必须先执行同一过滤条件的 discovery，例如：

```powershell
ctest --test-dir out/build/msvc -C Release -N -R '<exact-regex>'
ctest --test-dir out/build/msvc -C Release -R '<exact-regex>' --output-on-failure
```

注册卡必须冻结 exact test name、label 和 discovery count。后续卡实际匹配数量必须
等于冻结值或由本卡明确增加后的新值；`0 tests` 即失败，不能用退出码 0 冒充通过。
记录 PASS/FAIL/SKIP 数；required gate 的 SKIP 不是 PASS。

环境型任务同时记录两个状态：

```text
implementation_status: PENDING / DONE
gate_result: NOT_RUN / PASS / FAIL / BLOCKED
```

实现可在无真机环境时完成，但 Task24/41/42/43 的 required gate 未 `PASS` 时，任何
里程碑 verdict 仍被阻止。

通用禁止文件：当前卡未列出的 M1 protocol/golden fixture、AIM/driver contract、
已冻结 M2 public semantics、其他父 Task 的生产文件、用户真实磁盘与 bundle。

### 2.1 冻结测试名与精确文件表

第一张 registration 卡可创建表中“测试文件”；之后 test name/label/count 进入执行
记录，未经单独审查不得改名或移除。`*.h/*.cpp` 在这里表示明确列出的同 basename
两文件，不表示目录通配授权。

| Task | 允许生产文件 | 允许测试/证据文件 | frozen test / gate |
|---:|---|---|---|
| 07 | `dynamic_block_store.h`, `dynamic_block_store_internal.h`, `dynamic_block_store_core.cpp`, `dynamic_block_store_write.cpp`, `dynamic_block_store_read.cpp`, `dynamic_block_store_ranges.cpp`, `dynamic_block_store_snapshot.cpp`, `dynamic_block_types.h`, `m2_report.h/.cpp` | `dynamic_block_store_consistent_snapshot_tests.cpp`, exact test CMake | `unit.m3.m2_consistent_snapshot`; `m2.1-v3,v5` |
| 08 | none | `tests/support/consistent_snapshot_model.h/.cpp`, `tests/unit/dynamic_block_store_consistent_snapshot_model_tests.cpp`, exact test CMake | `unit.m3.m2_consistent_snapshot_model`; `m2.1-v4` |
| 09 | none | `docs/validation/m3-persistence-acceptance.md`, `M3_TASKLIST.md` | aggregate M2/M2.1 gates |
| 13 | `src/persistence/persistence_types.h/.cpp`, optional `persistence_error.h/.cpp` only if approved | `tests/unit/m3_persistence_types_tests.cpp`, `tests/fixtures/m3/fault-phase-registry.json`, exact CMake | `unit.m3.persistence_types`; `m3-v1` |
| 14 | `src/persistence/manifest_format.h/.cpp` | `tests/unit/m3_manifest_format_tests.cpp`, `tests/fixtures/m3/manifest/*`, exact CMake | `unit.m3.manifest_format`; `m3-v1` |
| 15 | `src/persistence/manifest_selector.h/.cpp` | `tests/unit/m3_manifest_selector_tests.cpp`, `tests/fixtures/m3/recovery/*`, exact CMake | `unit.m3.manifest_selector`; `m3-v2` |
| 16 | `src/persistence/journal_format.h/.cpp` | `tests/unit/m3_journal_format_tests.cpp`, `tests/fixtures/m3/journal/*`, exact CMake | `unit.m3.journal_format`; `m3-v1` |
| 17 | `src/persistence/journal_index.h/.cpp` | `tests/unit/m3_journal_index_tests.cpp`, exact CMake | `unit.m3.journal_index`; `m3-v1` |
| 18 | `src/persistence/journal_index.h/.cpp` | `tests/unit/m3_journal_index_builder_tests.cpp`, exact CMake | `unit.m3.journal_index_builder`; `m3-v1` |
| 19 | `src/persistence/journal_writer.h/.cpp` | `tests/unit/m3_journal_writer_tests.cpp`, existing fault-file support only, exact CMake | `unit.m3.journal_writer`; `m3-v1,v2` |
| 20 | `src/persistence/journal_reader.h/.cpp` | `tests/unit/m3_journal_reader_tests.cpp`, exact CMake | `unit.m3.journal_reader`; `m3-v1` |
| 21 | `src/persistence/vhdx_format.h/.cpp` | `tests/unit/m3_vhdx_format_tests.cpp`, `tests/fixtures/m3/vhdx/*`, exact CMake | `unit.m3.vhdx_format`; `m3-v1` |
| 22 | `src/persistence/vhdx_reader.h/.cpp` | `tests/unit/m3_vhdx_reader_tests.cpp`, exact CMake | `unit.m3.vhdx_reader`; `m3-v1` |
| 23 | `src/persistence/vhdx_writer.h/.cpp` | `tests/unit/m3_vhdx_writer_tests.cpp`, exact CMake | `unit.m3.vhdx_writer`; `m3-v1` |
| 24 | none | `tests/integration/m3_windows_vhdx_compat_tests.cpp`, `tests/e2e/test-m3-vhdx-compat.ps1`, exact CMake | `integration.m3.windows_vhdx`; `m3-v5` |
| 25 | `src/persistence/composite_backing_store.h/.cpp` | `tests/unit/m3_composite_backing_store_tests.cpp`, exact CMake | `unit.m3.composite_backing`; `m3-v2` |
| 26 | `src/persistence/recovery_engine.h/.cpp` | `tests/integration/m3_recovery_tests.cpp`, frozen Task15 fixtures, exact CMake | `integration.m3.recovery`; `m3-v2` |
| 27 | `src/persistence/image_creator.h/.cpp` | `tests/integration/m3_image_create_tests.cpp`, exact CMake | `integration.m3.image_create`; `m3-v2` |
| 28 | `src/storage/backing_store.h`, exact existing `dynamic_block_store*.cpp` named by card, approved persistence adapter h/cpp | `tests/integration/m3_lazy_backing_tests.cpp`, exact CMake | `integration.m3.lazy_backing`; `m3-v3` |
| 29 | `src/persistence/image_session.h/.cpp` | `tests/unit/m3_image_session_tests.cpp`, exact CMake | `unit.m3.image_session`; `m3-v2` |
| 30 | `src/persistence/checkpoint_coordinator.h/.cpp` | `tests/integration/m3_checkpoint_tests.cpp`, exact CMake | `integration.m3.checkpoint_prepare`; `m3-v2` |
| 31 | `checkpoint_coordinator.h/.cpp`, `commit_resolver.h/.cpp` | `tests/integration/m3_commit_publish_tests.cpp`, frozen phase registry, exact CMake | `integration.m3.commit_publish`; `m3-v2` |
| 32 | `checkpoint_coordinator.h/.cpp`, `image_session.h/.cpp`, exact approved storage adapter | `tests/integration/m3_checkpoint_reconcile_tests.cpp`, exact CMake | `integration.m3.checkpoint_reconcile`; `m3-v2,v3` |
| 33 | none | `tests/model/m3_checkpoint_crash_model_tests.cpp`, `tests/fixtures/m3/crash-matrix.json`, exact CMake | `model.m3.checkpoint_crash`; `m3-v2` |
| 34 | `src/persistence/live_extent_enumerator.h/.cpp` | `tests/unit/m3_live_extent_tests.cpp`, exact CMake | `unit.m3.live_extents`; `m3-v4` |
| 35 | `src/persistence/compaction_coordinator.h/.cpp` | `tests/integration/m3_compaction_tests.cpp`, frozen phase/oracle fixtures, exact CMake | `integration.m3.compaction`; `m3-v4` |
| 36 | `src/persistence/retention_manager.h/.cpp` | `tests/integration/m3_retention_tests.cpp`, exact CMake | `integration.m3.retention`; `m3-v4` |
| 37 | `image_inspector.h/.cpp`, `src/m3/m3_report.h/.cpp`, `src/m3/m3_evidence_publisher.h/.cpp` | `tests/unit/m3_report_tests.cpp`, JSON fixtures, exact CMake | `unit.m3.report`; `m3-v2,v4` |
| 38 | `src/m3/m3_cli.h/.cpp`, `m3_lifecycle_runner.h/.cpp`, `main.cpp` | `tests/integration/m3_cli_tests.cpp`, exact CMake | `integration.m3.cli`; `m3-v2,v4` |
| 39 | `src/m3/m3_provider.h/.cpp`, `m3_windows_acceptance.h/.cpp`, composition-root/CMake lines explicitly listed by card | `tests/integration/m3_provider_integration_tests.cpp` | `integration.m3.provider` |
| 40 | none | `M3Harness.Common.ps1`, `test-m3-harness-preflight.ps1`, `run-m3-persistence.ps1`, `Invoke-M3Verification.ps1`, `Verify-M3Json.ps1`, `docs/testing/m3-e2e.md` | script preflight/JSON gates |
| 41 | none | `Invoke-M3NtfsWorkload.ps1`, frozen run harness/scenarios, generated evidence | `RealFunctional`; `m3-v5,v6` |
| 42 | none | `Invoke-M3CrashMatrix.ps1`, frozen scenarios/evidence schema | `Crash`; `m3-v2,v6,v7` |
| 43 | none | `Invoke-M3Soak.ps1`, verification/verifier scripts, `docs/evidence/m3/*` | `Soak`; M3-V1..V7 aggregate |
| 44 | none | README/status/license notices、M3 docs/tasklists only | full clean gates |

### 2.2 Oracle、fault phase 与随机测试协议

- Oracle/fixture 卡先单独提交，记录 file SHA-256 和 commit SHA；后续 production
  卡禁止修改这些路径。只有独立 oracle reviewer 可以开新的 oracle repair 卡。
- Task13 必须先冻结 `tests/fixtures/m3/fault-phase-registry.json`：每个 phase 有稳定
  ID、发生点、允许 short/torn/kill 与预期 disposition。Task19/31/33/35/42 不得
  使用 registry 外的私有 phase 名，也不得在后续任务静默删除 required phase。
- 随机测试 schema 固定为 `m3-seed-v1`。Quick 固定 seeds：`0`, `1`,
  `0x00C0FFEE`, `0xDEADBEEF`；批量 seeds 由 `SHA256(source-commit | card-id | i)`
  的低 64 位生成。失败保存 seed、profile、operation count、最小 trace；shrinker
  只能删操作/缩短 range，不得改 expected outcome。
- CI/quick 与 manual/soak profile 分开记录；扩大时间/操作数不能替代确定性 barrier
  与 mutation proof。

## 3. 阶段一：完成 M2.1 前置（Task 07–09）

### Task 07 — 压力、stall、close 与资源清理

前置：Task 06 `DONE`。允许范围仅限 PLAN Task 07 列出的 storage coordinator、
core/write/read/ranges/snapshot、对应 TestAccess、stats/report 和本任务测试。

- [x] `T07.1 [M]` 建立 focused registration，先用 `ctest -N` 冻结 exact name/
  count；加入 one-record COW budget fixture、fake clock 和 barrier，不改 production。
- [x] `T07.2 [H]` 冻结 ownership/lock-order 表：stripe、coordinator mutex、admission、
  reservation、Preparing marker、payload、waiter 各由谁持有/释放；未签署前不写逻辑。
- [x] `T07.3 [H]` RED：第一 pre-image 填满 budget，第二 writer 到达“容量不足”
  barrier 后保持 blocked；用事件证明它尚未 publish mutation，不得用 sleep。
- [x] `T07.4 [H]` RED：第二 writer 等 capacity 时，另一个线程必须能取得该 block
  stripe；故意持 stripe wait 的 mutation 必须稳定卡在测试 watchdog 断言。
- [x] `T07.5 [X]` 只实现 try-reserve→capacity unavailable→release stripe→登记
  waiter；此卡不实现 wake 后 retry。验证 T07.3/4 的 blocked state 与 counters。
- [x] `T07.6 [X]` 只实现成功 reservation 的 per-block `Preparing` publication 与
  rollback；reader 可等 marker condition，但禁止通过获取 stripe 等 marker。
- [x] `T07.7 [X]` 只实现 wake 后 reacquire stripe + 重验 token/frontier/pre-image/
  entry identity；任一变化就 rollback/retry/按 frozen error 返回，禁止用旧指针复制。
- [x] `T07.8 [H]` RED+GREEN：batch consume 一个 pre-image 释放额度并精准唤醒；
  writer 完成后 mutation 与 snapshot record 均 exact，一次 notify 不丢失。
- [x] `T07.9 [H]` 多 writer/fairness/spurious/cancel/deadline matrix；每个 interleaving
  用 barrier 固定，current blocked-writer gauge 和 reservation bytes 始终一致。
- [x] `T07.10 [X]` 实现 no-progress stall deadline：一次 CAS 转 Aborted，保存
  `SnapshotResourceExhausted`，notify 全 reader/writer，foreground mutation 继续；
  lifetime abort/stall counters 各恰好加一。
- [x] `T07.11 [H]` 冻结并 RED close matrix：admission、capacity、transient batch、
  finish、abort 五个等待点；每格写具体 waiter/event/资源 before/after。
- [x] `T07.12 [X]` 实现 close：先阻止新 admission，再 signal 全 wait domain，drain
  active mutation/read，最后释放 token/COW/pin/handle；不得在 active count 前清 state。
- [x] `T07.13 [M]` resource invariant tests：所有 current gauges 归零，peak/lifetime
  只按 frozen 事件变化；失败/取消/close/abort 每条路径都检查。
- [x] `T07.14 [M]` Debug/Release focused repeat、`unit.m2.concurrency`、
  `unit.m2.stress`、`m2-v1/v3`、完整 Release；记录 discovery count、seed、峰值、hash。
- [x] `T07.15 [X]` 独立审查 lock order、每个 predicate/notify、Preparing rollback、
  close/accounting；做“持 stripe wait”和“删 notify”mutation proof，P0/P1=0 后 DONE。

### Task 08 — 独立整盘快照参考模型

允许文件只有 `tests/support/consistent_snapshot_model.*`、
`tests/unit/dynamic_block_store_consistent_snapshot_model_tests.cpp`、test CMake。
生产 snapshot/storage 文件全部禁止修改。

- [x] `T08.1 [M]` 写纯 map oracle 类型：block→kind/value/version，另存 cut
  snapshot；不得 include/call coordinator、batch helper 或生产 codec。
- [x] `T08.2 [M]` 写确定性 schedule generator：2–64 blocks，write/zero/discard/
  begin/copy/finish/abort；输入只有 seed 和 operation count，失败打印完整 seed。
- [x] `T08.3 [H]` 冻结比较规则：返回 records 的 union 必须与一个线性化 cut
  完全相等；post-cut clean→dirty 不得出现；DATA/ZERO/DISCARD、final short block、
  abort partial progress 全覆盖。独立 reviewer 审查 oracle，生产作者不得改期望。
- [x] `T08.4 [M]` 加最小 trace reducer，把失败 schedule 缩到仍失败的前缀；
  reducer 只删操作，不重写 expected outcome。
- [x] `T08.5 [H]` 做 mutation proof：在隔离补丁中禁用一次 pre-image preservation，
  证明模型稳定失败并记录断言；随后恢复补丁，工作树不得残留 mutation。
- [x] `T08.6 [M]` Debug/Release 固定种子各一次、100 个生成 seed、完整 M2 回归；
  保存最慢 seed、总操作数和 hash。
- [x] `T08.7 [H]` 独立 reviewer 确认 oracle 与生产实现无共享算法/codec，GO 后
  Task 08 才可 `DONE`。

### Task 09 — M2.1 prerequisite verdict

只允许修改 `docs/validation/m3-persistence-acceptance.md`、`M3_TASKLIST.md`；
发现代码缺陷时另开修复卡，不得在 verdict 卡夹带产品改动。

- [ ] `T09.1 [M]` 从 clean worktree 完整 configure/build Debug/Release，运行完整
  CTest；记录测试数量、exit code、commit、binary hashes，不引用旧日志。
- [ ] `T09.2 [M]` 运行 `m3-v3` 两次、`m2-v1..v5`、Task08 100 个新 seed 和
  bounded concurrency soak；保存原始结果。
- [ ] `T09.3 [H]` 逐文件生成 M2.1 diff inventory，确认 `src/storage` 无 persistence
  format/Win32 file/AIM/proxy 依赖；列出 public API/错误/stats 变化。
- [ ] `T09.4 [X]` 按 SPEC §7、PLAN §1.1–1.3 逐条签署线性化、COW budget、
  no-resurrection、install-before-ack、close/resource 结论。任一项无证据即 NO-GO。
- [ ] `T09.5 [M]` 把结论和证据写入验收文档；只能写 `M2.1 PASS/NO-GO/
  BLOCKED`，不得写 M3 GO。
- [ ] `T09.6 [X]` 独立验收人复核原始日志与 diff；GO 后 Task09 `DONE`，否则
  回到具体 defect 卡。

## 4. 阶段二：冻结格式与稳定类型（Task 13–16）

Task 10–12 必须先 `DONE`。所有格式字段、offset、reserved bytes、CRC/digest input
以 SPEC 为唯一来源；禁止从同一 production encoder 生成 golden expected bytes。

### Task 13 — Persistence 稳定类型、错误与 outcome

允许：`src/persistence/persistence_types.*`、必要的 `persistence_error.*`、
`tests/unit/m3_persistence_types_tests.cpp`、test CMake。

- [ ] `T13.1 [M]` 新建 focused target；RED 覆盖全部 enum 数值/stable string、
  geometry constants、phase/session names。先补最小 compile stub，目标 RED 必须是
  断言失败，缺 API 只记录为 bootstrap。
- [ ] `T13.2 [M]` RED 覆盖 geometry 与 checked increment：512/4096 sectors、
  64 KiB/2 MiB divisibility、zero、overflow、generation/epoch wrap。
- [ ] `T13.3 [M]` RED 覆盖 caller-supplied `PersistenceLimits`：每个 zero、单条
  record/node/block 放不下、bytes/entries 不一致、算术 overflow、隐藏 unlimited。
- [ ] `T13.4 [H]` 冻结 `NotCommitted`、`CommitIndeterminate`、`Committed`、
  `CommittedNeedsReconciliation` 数据结构和 exhaustive transition table；只写
  types/tests，不写 checkpoint 流程。
- [ ] `T13.5 [H]` 实现/测试 `VolumeBarrierKind` 和 request validation；unknown/
  missing barrier 必须在 snapshot begin 前失败，raw-test 明确不声称 filesystem
  durability。
- [ ] `T13.6 [M]` 实现 error context value types：operation、phase、native code、
  image UUID、operation id、generation、disposition；默认值 fail-closed，日志字符串
  不参与程序分支，路径/块内容默认 redact。
- [ ] `T13.7 [H]` exhaustive switch/serialization/ABI review；运行 Debug/Release
  focused、`m3-v1`、完整 Release。未覆盖 enum 必须编译或测试失败。
- [ ] `T13.8 [H]` 独立冻结 `fault-phase-registry.json`：stable ID、exact boundary、
  short/torn/kill capability、required gate 与 expected disposition；单独提交/hash，
  Task19/31/33/35/42 只读复用。

### Task 14 — 4096-byte manifest slot codec

允许：`manifest_format.*`、独立 fixtures、manifest codec tests。

- [ ] `T14.1 [M]` 手工写一份 4096-byte golden fixture 和字段 offset 表；fixture
  不得调用 production encoder。先加可编译 stub，测试必须在 golden bytes/value
  断言 RED；缺 decoder/encoder 只算 bootstrap。
- [ ] `T14.2 [M]` 实现 decode 的固定长度/magic/version/reserved-zero/name UTF-8/
  geometry/overflow 边界；任一 short/extra/unknown 必须返回具体格式错误。
- [ ] `T14.3 [M]` 实现 CRC：CRC 字段视为零，覆盖冻结范围；single-bit flip、CRC
  field flip、reserved byte flip 都必须被拒绝。
- [ ] `T14.4 [M]` 实现 deterministic encode：先清零整 slot，再按 offset 写字段，
  最后 CRC；同一输入 byte-for-byte 相同。
- [ ] `T14.5 [M]` 加 boundary matrix：max name、empty/invalid name、generation
  0/max、slot A/B、unknown flags、trailing garbage；encode→decode 不能替代 golden。
- [ ] `T14.6 [H]` 独立 offset/coverage review 和 mutation proof（改一个 offset/CRC
  range 必须失败）；Debug/Release focused + `m3-v1`。

### Task 15 — 最高完全有效的双槽 candidate

允许：`manifest_selector.*`、selector/recovery fixture tests；不得修改 journal/VHDX
codec 来迎合 selector。

- [ ] `T15.1 [H]` 冻结 candidate validation 输入：slot 自身、referenced file identity、
  journal header/commit/root、VHDX identity/geometry 都是同一个完整 verdict。
- [ ] `T15.2 [M]` 建立 A/B matrix fixture：valid/invalid/missing/corrupt/unsupported/
  identity mismatch/generation ties；只写 fake validator 与期望。
- [ ] `T15.3 [X]` 实现“先完整验证每个 candidate，再从完整有效集合选最高 generation”；
  禁止先按 generation 选 slot 后才验证，禁止把 corrupt 当 missing。
- [ ] `T15.4 [X]` 实现 fallback diagnostics：高代无效、低代完整时选择低代并保留
  高代失败 provenance；同代内容冲突/混代引用必须 fail closed。
- [ ] `T15.5 [H]` 加 mutation tests：跳过任一 journal/root/base identity 检查时必须
  失败；selector 不得读取 manifest committed length 之外。
- [ ] `T15.6 [M]` Debug/Release focused + `m3-v2` 当前可用项，保存每格 verdict。
- [ ] `T15.7 [X]` 独立 recovery reviewer GO 后才可 `DONE`。

### Task 16 — Journal header、records、commit 与 chain digest

允许：`journal_format.*`、journal codec tests/fixtures。

- [ ] `T16.1 [M]` 手工冻结 header/DATA/ZERO/DISCARD/index/commit golden bytes、
  declared lengths、alignment 和 reserved ranges；不调用 production encoder。
- [ ] `T16.2 [M]` 实现 header encode/decode：magic/version/UUID/geometry/base
  generation/epoch/CRC/reserved bytes 全验证。
- [ ] `T16.3 [M]` 实现 DATA/ZERO/DISCARD codec；payload 长度、final partial block、
  offset+length overflow、unknown type/flags、padding 全 fail closed。
- [ ] `T16.4 [M]` 实现 index-node/commit descriptor codec，但不实现 tree 算法；
  child ranges、root offset、committed length、generation/epoch 做 checked validation。
- [ ] `T16.5 [H]` 冻结 streaming chain-digest input 顺序与 commit exclusion/inclusion；
  chunked SHA 必须与独立 golden digest 一致，不能 buffer 整 journal。
- [ ] `T16.6 [M]` 加 corruption matrix/fuzz：每个字段、长度、CRC、digest、truncation、
  duplicate/unknown record；reader 永不越过 caller committed length。
- [ ] `T16.7 [H]` offset/length/digest independent review 与 mutation proof；运行
  Debug/Release focused、fuzz、`m3-v1`。

## 5. 阶段三：Journal 与 VHDX 读写层（Task 17–24）

### Task 17 — Immutable B+tree reader

允许：`journal_index.*`、`m3_journal_index_tests.cpp`、test CMake。

- [ ] `T17.1 [M]` 建 focused target 和手工 node fixtures：empty/leaf/multi-level、
  boundary/absent、DATA/ZERO/DISCARD。先加可编译 lookup stub，RED 必须在 expected
  descriptor/Absent 断言失败，缺 API 只算 bootstrap。
- [ ] `T17.2 [H]` 冻结 corruption matrix：CRC、unsorted/duplicate key、cycle、
  child 越 committed length、depth 9、offset/alignment overflow；corrupt 绝不能
  变成 Absent。
- [ ] `T17.3 [H]` 实现 iterative lookup：max depth 8、visited-offset set、每次 exact
  bounded read、key range 单调收窄；任何 allocation 前先验证 count/length limit。
- [ ] `T17.4 [M]` 实现 bounded cache 外壳和 eviction tests；cache key 必须包含
  file identity + epoch + root generation + node offset，禁止跨 immutable view 复用。
- [ ] `T17.5 [M]` 一百万 random lookup 对 `std::map`，记录 seed/peak cache bytes/
  read calls；Debug/Release focused + format tests。
- [ ] `T17.6 [H]` 独立 malformed-input/cache-identity review，mutation 删除 depth/
  committed-bound check 时测试必须失败。

### Task 18 — COW index builder

允许：`journal_index.*`、`m3_journal_index_builder_tests.cpp`。

- [ ] `T18.1 [M]` RED fixtures：insert/replace kind、leaf/internal/root split、unchanged
  subtree reuse、乱序输入得到相同 bytes；同 checkpoint duplicate block 必须拒绝。
- [ ] `T18.2 [H]` 冻结 pure planning result：新 immutable node images、logical child
  references、old-root identity；此卡不写文件、不分配 physical offsets。
- [ ] `T18.3 [H]` 实现只复制受影响 root paths 的 planner；失败不得改变 old root，
  node fanout/count/size 全受 `PersistenceLimits` 约束。
- [ ] `T18.4 [M]` 单独实现 checked append-offset assignment + encoding；输入 planner
  不变，同输入 bytes deterministic；overflow/align/limit 先失败后零写入。
- [ ] `T18.5 [M]` 100,000 updates 对 `std::map` 比较每个新 root lookup，并验证
  unchanged subtree physical identity 复用。
- [ ] `T18.6 [H]` path-copy/split independent review + Debug/Release focused。

### Task 19 — Append、flush、readback 完整 checkpoint

允许：`journal_writer.*`、writer tests/fakes；不得写 manifest。

- [ ] `T19.1 [M]` 建 phase fake 和 RED matrix：每个 record/node/commit 的 before/
  short/disk-full/cancel/deadline、flush、readback；每格记录 calls 与 surviving bytes。
- [ ] `T19.2 [H]` 冻结 append contract：起点只能是 manifest committed length，
  禁止覆盖 committed bytes；padding 全零；canonical digest streaming。
- [ ] `T19.3 [H]` 实现 record append 与 COW nodes/commit append；每个 offset/length
  checked，任何失败只留下未引用 tail，old root 不变。
- [ ] `T19.4 [X]` 实现唯一 durability 顺序：完整 commit bytes→一次 journal flush→
  re-read 所有新 bytes→独立重算 record/node CRC、digest、committed end。禁止在
  readback 前返回 Prepared。
- [ ] `T19.5 [M]` 构造 `PreparedCheckpoint` value：generation、root/commit、length、
  digest、counts、bounded ack-stream descriptor；类型不得暗示已 publish。
- [ ] `T19.6 [H]` 证明每个 pre-manifest failure 都只产生 ignorable tail；mutation
  调换 commit/flush/readback 顺序必须红。Debug/Release focused + `m3-v1`。

### Task 20 — JournalReader block resolution

允许：`journal_reader.*`、reader tests。

- [ ] `T20.1 [M]` RED truth table：DATA/ZERO/DISCARD/Absent、boundary/final block；
  DATA 必须重验 descriptor/header/type/index/generation/payload CRC。
- [ ] `T20.2 [H]` RED corruption matrix：valid index 指向 corrupt/wrong type/wrong
  block/wrong generation/truncated payload；统一 `BackingDataCorrupt`，不得 fallback。
- [ ] `T20.3 [H]` 实现 bounded exact reads 和 `BlockLoadKind` 映射；任何 read 不得
  越 manifest committed length，物理 tail 不可见。
- [ ] `T20.4 [M]` 加 cancel/deadline、short read、immutable concurrent readers、
  handle lifetime tests。
- [ ] `T20.5 [H]` mutation 去掉任一 descriptor/payload check 必须红；跑 journal/
  index/format Debug/Release focused。

### Task 21 — Strict VHDX structural parser

允许：`vhdx_format.*`、独立 golden fixtures、format tests。

- [ ] `T21.1 [M]` 用独立脚本/手工 bytes 冻结 file identifier、双 headers、双
  region tables、metadata、parameters、disk size、sector sizes、BAT geometry。
- [ ] `T21.2 [M]` RED 每个 structural boundary truncation 和 bad signature/checksum/
  GUID；不能用 production writer 生成唯一 expected fixture。
- [ ] `T21.3 [H]` RED malformed matrix：header sequence 冲突、duplicate/missing
  regions/metadata、overlap/alignment/overflow、unsupported differencing/compression、
  invalid sector/block geometry。
- [ ] `T21.4 [H]` 实现 frozen dynamic non-differencing subset parser；双 header 与双
  region table 各自从有效集合选最高，不能把一份 copy 的 validity 传给另一份。
- [ ] `T21.5 [M]` 构造 immutable `VhdxLayout` value，仅暴露 checked ranges/
  geometry/identity；无 writable pointer，无 silent feature downgrade。
- [ ] `T21.6 [M]` boundary fuzz + independent fixture builder cross-check；Debug/
  Release focused。
- [ ] `T21.7 [H]` malformed-input reviewer 和 mutation proof GO 后完成。

### Task 22 — Lazy VHDX BAT reader

允许：`vhdx_reader.*`、reader tests。

- [ ] `T22.1 [M]` RED states：fully present/not present/undefined/zero/unmapped、
  boundary/final、512/4096 sector、2 MiB payload→64 KiB block slicing。
- [ ] `T22.2 [H]` RED rejected states：partially present/invalid bits/physical overlap/
  payload outside file or before data region/overflow/post-open truncation。
- [ ] `T22.3 [H]` 实现 on-demand BAT page lookup；cache 有 caller budget，key 含
  immutable base identity，Absent/Zero 不读 payload、不分配 image-sized buffer。
- [ ] `T22.4 [M]` 实现 exact payload slice read 与 read counters；读一个 uncached
  64 KiB block只能触及对应 BAT page/payload range。
- [ ] `T22.5 [M]` 实现 allocated extent descriptor enumeration，不读 payload；输出
  sorted/unique/bounds-checked ranges。
- [ ] `T22.6 [H]` random sparse map oracle、cache identity mutation、Debug/Release
  focused。

### Task 23 — Sparse dynamic VHDX writer

允许：`vhdx_writer.*`、writer tests；最终文件名必须是 unique，禁止 overwrite。

- [ ] `T23.1 [M]` RED fixtures：empty/minimum/large sparse/final partial/all-zero/non-
  contiguous payload，expected layout 来自 Task21 独立 parser/fixture。
- [ ] `T23.2 [H]` 实现 pure deterministic layout plan：metadata/BAT first、payload
  alignment、checked arithmetic、all-zero skip、reserved zero；此卡零文件写入。
- [ ] `T23.3 [M]` 实现 same-directory unique temp file、sparse mark、extend、按 plan
  exact writes；每个 phase 可注入失败。
- [ ] `T23.4 [X]` 实现 publish 顺序：flush→close→Task21/22 reopen verify checksums+
  sampled payload→write-through rename 到 unique final；任何失败不得返回 final。
- [ ] `T23.5 [M]` failure matrix：create/sparse/extend/metadata/BAT/payload/flush/close/
  reopen/rename；断言 manifest-referenced base 从未覆盖。
- [ ] `T23.6 [H]` random sparse logical hash 与 reader 比较、mutation 调换 publish
  顺序必须红；Debug/Release focused。

### Task 24 — Windows native VHDX compatibility gate

允许：compat integration test、PowerShell harness、test registration；不得在 gate
中修改 VHDX writer 来“跳过”失败。

- [ ] `T24.1 [M]` 实现 capability probe：admin、`OpenVirtualDisk`、
  `AttachVirtualDisk`、`GetVirtualDiskPhysicalPath`；不可用返回 machine-readable
  `BLOCKED/SKIP reason`，不算 PASS。
- [ ] `T24.2 [M]` 生成 empty/sparse/boundary/512/4096 fixtures；read-only/no-drive-
  letter open、query、attach、sample、finally detach，记录 Win32 codes。
- [ ] `T24.3 [H]` 加 safety gate：AIM-exported disk online 时禁止 attach 同一数据；
  cleanup failure 必须使 gate 非零并给恢复命令。
- [ ] `T24.4 [M]` 用 Windows 独立创建一份兼容 VHDX，Task21/22 读取并比较 geometry/
  data；若 fixture 不入库，保存可重复 generator + hash。
- [ ] `T24.5 [H]` 管理员真机复核所有场景并签署 M3-V5；环境 SKIP 不能 `DONE`。

## 6. 阶段四：Recovery、create 与 lazy session（Task 25–29）

### Task 25 — Journal-first composite backing

允许：`composite_backing_store.*`、对应 tests。

- [ ] `T25.1 [H]` 独立作者冻结 4×3 truth table：journal DATA/ZERO/DISCARD/
  Absent × VHDX data/zero/absent；只有 journal Absent fallback，corrupt 永不 fallback。
- [ ] `T25.2 [M]` 建 fake readers/counters 和 RED tests，覆盖 cancel/deadline、稳定
  corruption provenance、one-block output。
- [ ] `T25.3 [X]` 实现 resolution precedence 与错误传播；禁止把 ZERO/DISCARD 当
  Absent，禁止读旧 layer 掩盖新 layer corruption。
- [ ] `T25.4 [H]` 实现 immutable view/cache identity binding；publish 新 generation
  后旧 reader 仍只见旧 base/journal/root。
- [ ] `T25.5 [M]` lazy-I/O gate：open bounded metadata reads；一个 uncached block 只
  读相关 index/BAT/payload；记录 calls/bytes/peak cache。
- [ ] `T25.6 [X]` truth-table 与 lifetime independent review 后完成。

### Task 26 — Recovery engine 与 immutable BackingView

允许：`recovery_engine.*`、recovery tests/fixtures；recovery 绝不写盘。

- [ ] `T26.1 [M]` 独立 fixtures：A only/B only/both/newer torn+valid predecessor/
  both invalid/missing/replaced/truncated committed/valid tail/split-brain/unsupported。
- [ ] `T26.2 [H]` 冻结每个 slot 的 first stable rejection reason 与 selected/fallback
  期望；fixture oracle 不 include selector/production codec。
- [ ] `T26.3 [X]` 实现 bounded candidate validation：base identity/layout、journal
  identity/header、commit/digest/root/committed length 全通过才进入 valid set。
- [ ] `T26.4 [X]` 只从 selector 结果构造一个 immutable `BackingView` + owned handles；
  recovery 禁止 truncate/repair/publish/ack/delete。
- [ ] `T26.5 [M]` 实现 redacted structured diagnostics，逐 slot 保存 provenance，
  默认无 absolute path/block content。
- [ ] `T26.6 [H]` independent oracle 比 selection 与所有 block reads；mutation 跳过
  任一 identity/root/digest check 必须红。
- [ ] `T26.7 [X]` recovery reviewer GO + Debug/Release `m3-v2` 后完成。

### Task 27 — Generation-zero create/reopen

允许：`image_creator.*`、create tests；只能通过 Task26 recovery 判定成功。

- [ ] `T27.1 [M]` RED matrix：geometry/existing/path escape/low disk/every I/O phase/
  concurrent creators；记录 artifacts 与 writer ownership。
- [ ] `T27.2 [M]` 建 unique-name artifact planner：base、journal、8192-byte manifest；
  inactive slot 保持 zero/invalid，不覆盖已发布路径。
- [ ] `T27.3 [X]` 实现 generation-zero durability：base flush+readback→journal genesis
  flush+readback→manifest exact create→一个 slot publish+flush+full-slot readback。
- [ ] `T27.4 [X]` 关闭 creation handles 后只调用 Task26 reopen；仅当 recovery 选择
  generation 0 且 boundary blocks 为 zero 才返回成功。
- [ ] `T27.5 [H]` retry/concurrent/failure artifact tests：已发布 image 永不覆盖；
  unreferenced artifacts 可诊断，但此任务不自动删除。
- [ ] `T27.6 [X]` publish-order mutation proof + independent review；Debug/Release
  `m3-v2`。

### Task 28 — Recovered backing 接入 M2 first-touch load

允许：storage backing adapter、必要的 `dynamic_block_store*`、lazy integration tests；
禁止改变冻结 M2 public semantics。

- [ ] `T28.1 [M]` 只写 instrumentation/RED：open 零 payload eager reads，一个 block
  只有一个 concurrent load，多个 reader 共享，writer-during-load 时序确定。
- [ ] `T28.2 [H]` RED ZERO/DISCARD/Absent/corrupt/cancel/deadline/retry/close-in-flight
  matrix；expected 由 Task25 contract 得出。
- [ ] `T28.3 [X]` 通过现有 `install_backing_view()` 安装完整 immutable view；旧 view
  由每个 in-flight load 强引用，最后 load 释放前不得销毁 handles/cache。
- [ ] `T28.4 [X]` 实现 first-touch arbitration 与 mutation ordering；corrupt 不变 zero，
  cancel/timeout 不发布半初始化 block。
- [ ] `T28.5 [H]` eviction/reload：clean 从 exact selected generation 重载；dirty/
  pinned/snapshot-protected 不错误 evict。
- [ ] `T28.6 [M]` 跑全部 M2 labels、新 lazy tests、read counters 和 concurrency
  repeat；保存 fixed seed。
- [ ] `T28.7 [X]` storage/persistence 双 reviewer GO 后完成。

### Task 29 — Image session state 与 structured outcome

允许：`image_session.*`、state tests；不得实现 checkpoint/compaction 算法。

- [ ] `T29.1 [M]` 手工 transition table 覆盖 Opening/Ready/Checkpointing/
  Compacting/ResolvingCommit/Reconciling/Closing/Closed/PersistenceFaulted 的每对边。
- [ ] `T29.2 [H]` RED concurrent operation gate：immutable reads 可并发；checkpoint/
  compaction/close 的允许、busy、closing 顺序全部确定。
- [ ] `T29.3 [X]` 实现 serialized publish-affecting gate 和 state transition invariant；
  禁止普通 bool 代替状态/operation ownership。
- [ ] `T29.4 [X]` 保存 first failure + `CommitDisposition`；Indeterminate 必须进入
  ResolvingCommit 并阻止新 mutation/checkpoint，不能被后续 cleanup error 覆盖。
- [ ] `T29.5 [H]` close idempotence/drain tests：结果不能仅因内存 state 更新宣称
  durable success。
- [ ] `T29.6 [X]` exhaustive transition/lock-order/outcome independent review +
  Debug/Release focused。

## 7. 阶段五：Checkpoint、commit 与 reconciliation（Task 30–33）

### Task 30 — Checkpoint pre-manifest durability pipeline

允许：`checkpoint_coordinator.*`、checkpoint tests/fakes；禁止写 manifest slot、安装
new backing 或 ack dirty state。

- [ ] `T30.1 [H]` 冻结 phase oracle：validate barrier→begin snapshot→copy each token
  member once→append records/index/commit→journal flush→full readback→Prepared；此时
  manifest write/install/ack calls 必须为零。
- [ ] `T30.2 [M]` RED matrix：DATA/ZERO/DISCARD、overwritten-after-cut、clean image、
  final partial、cancel/deadline、每个 pre-manifest injected failure。
- [ ] `T30.3 [H]` RED unknown/missing `VolumeBarrierKind`：必须在 snapshot begin 前
  返回，raw-test 标记传播到 outcome/report 且不声称 filesystem durability。
- [ ] `T30.4 [X]` 实现 bounded streaming batch→JournalWriter；不 materialize whole
  disk，不在 durable append 后保留 payload；cursor/frontier/partial batch 只按 Task06。
- [ ] `T30.5 [X]` 所有 pre-manifest failure abort token、保留 old manifest/view、返回
  `NotCommitted`；journal 新 bytes 只是 ignorable tail，禁止 ack/cleanup。
- [ ] `T30.6 [H]` call-order mutation proof、peak memory/read-write counters、Debug/
  Release `m3-v2`；独立 durability reviewer GO。

### Task 31 — Manifest publish 与 CommitIndeterminate resolver

允许：`checkpoint_coordinator.*` 的 publish 段、`commit_resolver.*`、publish tests。

- [ ] `T31.1 [M]` 从 frozen fault registry 生成 phase tests：before slot write、short/
  torn write、after write、flush fail/success、readback fail/corrupt；固定 inactive slot，
  `ctest -N` count 与 registry required IDs 必须一一相等。
- [ ] `T31.2 [X]` 冻结 classification table：write attempt 前=`NotCommitted`；attempt
  开始后的 write/flush error=`CommitIndeterminate`；manifest flush success=
  `Committed`；之后 readback/validation fail=`CommittedNeedsReconciliation`。
- [ ] `T31.3 [H]` 只实现 inactive-slot encode、active/inactive identity 校验和 exact
  4096-byte prepared buffer；此卡不写文件。
- [ ] `T31.4 [X]` 只实现 slot-write attempt 线性化点 + one exact write；进入 attempt
  后直到 disposition 确定不接受普通 cancellation，不得 overwrite active slot。
- [ ] `T31.5 [X]` 只实现 manifest flush 与唯一 commit point；flush success 立即把
  disposition 置 `Committed`，后续错误不能倒退。
- [ ] `T31.6 [H]` 只实现 post-flush re-read + Task15 full candidate validation；失败
  只升级 `CommittedNeedsReconciliation`。
- [ ] `T31.7 [X]` resolver 入口只做：冻结 first failure/disposition、关闭旧 handles、
  重开并调用 normal recovery；禁止读 cached slot bytes。
- [ ] `T31.8 [X]` resolver 分支 A：N+1 complete→再次 manifest flush；成功后进入
  Reconciling，失败保持 indeterminate/fault context，不宣称 plain Committed。
- [ ] `T31.9 [X]` resolver 分支 B：attempted slot 可证明 invalid 且 N 是唯一 complete
  candidate→abort token 并返回 `NotCommitted`；“missing evidence”不等于 invalid。
- [ ] `T31.10 [X]` resolver 分支 C：无法证明 A/B→PersistenceFaulted，禁止猜测、
  fallback mutation 或清理 artifacts。
- [ ] `T31.11 [H]` resolver retry/idempotence/close tests，first failure 和 disposition
  不被 readback/cleanup 的后续 error 覆盖。
- [ ] `T31.12 [H]` crash clone/mutation 调换 write/flush/readback 或四类 disposition
  必须红；Debug/Release `m3-v2`。
- [ ] `T31.13 [X]` 独立 commit-point reviewer 对 phase×state×ownership 表签署后完成。

### Task 32 — Install backing、conditional ack 与 reconciliation

允许：checkpoint coordinator/session/storage adapter/reconcile tests；不得删除旧文件。

- [ ] `T32.1 [H]` 冻结 success call order：durable proof→recover/construct new immutable
  view→install backing→按 exact dirty identity conditional ack→finish token→report。
- [ ] `T32.2 [M]` RED overwritten-after-cut：old pre-image 已 checkpoint，但 newer
  dirty_epoch 仍 dirty；为每种 DATA/ZERO/DISCARD 建表。
- [ ] `T32.3 [X]` 实现 install + bounded ack-stream replay；ack 只匹配 version/kind/
  generation/dirty_epoch，禁止 broad “generation persisted” 清脏。
- [ ] `T32.4 [X]` install/ack/finish 任一 post-commit failure 返回
  `CommittedNeedsReconciliation`；reopen 选新 generation，retry reconcile 幂等；仅
  runtime reconciliation 完整后可普通 `Committed`。
- [ ] `T32.5 [X]` COW pre-image 仅在对应 member 已 conditional ack 或已证明更新
  dirty 仍保留后释放；close/cancel 不得提前释放。
- [ ] `T32.6 [H]` no-resurrection/lock-order/failure matrix mutation proof；运行完整
  M2 + `m3-v2/v3`。
- [ ] `T32.7 [X]` persistence/storage 双独立 reviewer GO。

### Task 33 — Independent checkpoint crash model

允许：`tests/model/m3_checkpoint_crash_model_tests.cpp`、独立 fixtures/JSON、test
registration；production 代码禁止修改，production author 禁止改 expected outcome。

- [ ] `T33.1 [M]` 从 SPEC 列出每个 durable primitive boundary：records/nodes/commit/
  journal flush+readback/manifest write+flush+readback/install/ack，给稳定 scenario ID。
- [ ] `T33.2 [H]` 独立 oracle 只接受 cloned bytes + transaction rules；每个 crash 后
  丢弃 process memory，期望只能 old complete generation 或 new complete generation。
- [ ] `T33.3 [M]` 加 short writes、sector-aligned torn slots、tail garbage、reordered
  simulated durability；输出 seed/phase/byte hashes。
- [ ] `T33.4 [H]` 每个 recovered block 与 simple map 比较；mixed mapping、silent
  corrupt fallback、unresolved indeterminate 一律失败。
- [ ] `T33.5 [M]` CI profile 跑 10,000 deterministic schedules，保存最小 failure
  trace；manual profile 更大但不替代 CI gate。
- [ ] `T33.6 [H]` mutation 破坏 commit classification/flush order/ack order 必须使
  oracle 红；独立 reviewer 确认无 production codec/selector 复用。

## 8. 阶段六：Compaction 与 retention（Task 34–36）

### Task 34 — Live extent enumeration

允许：`live_extent_enumerator.*`、extent tests。

- [ ] `T34.1 [M]` RED merged extents：journal DATA/ZERO/DISCARD shadow VHDX、base
  payload shadowing、holes、adjacent ranges、final partial。
- [ ] `T34.2 [H]` 冻结 capture contract：只枚举 compaction start 的一个 immutable
  `BackingView`，禁止读取 live mutable RAM 或跟随 later generation。
- [ ] `T34.3 [H]` 实现 ascending bounded descriptor stream；logical zero skip，DATA
  source 交 writer 前复核 identity/CRC，内存与 extent count 无关。
- [ ] `T34.4 [M]` random sparse full-map oracle，比较 descriptors、payload hashes、
  sorted/nonoverlap；记录 calls/peak bytes。
- [ ] `T34.5 [H]` view lifetime/cancel/deadline/corruption tests + Debug/Release
  `m3-v4` current gates。

### Task 35 — Compact to new base and journal epoch

允许：`compaction_coordinator.*`、compaction tests；复用 Task23/30–32，不复制其
publish/resolver 算法。

- [ ] `T35.1 [H]` 冻结 full phase log：single modifying-operation gate→capture durable
  view G→write/flush/readback/verify unique VHDX→write-through publish→create+flush
  journal E+1。
- [ ] `T35.2 [M]` RED ordinary live mutations 在 VHDX build 期间继续；checkpoint/
  second compaction 必须 Busy；记录 barrier/snapshot begin 前后 calls。
- [ ] `T35.3 [X]` 实现 pre-cut build against immutable G；禁止将 live RAM 混入 base，
  禁止在 new base/journal fully verified 前建立 cut。
- [ ] `T35.4 [X]` new files ready 后验证本 operation explicit volume barrier，建立
  Task06 cut，把相对 G 的 cut-time DATA/ZERO/DISCARD 写入 new journal。
- [ ] `T35.5 [X]` dirty set 即使为空也 append COW index+commit G+1，再严格调用
  Task31 publish/resolver 和 Task32 install/conditional ack；post-cut writes 保持 dirty。
- [ ] `T35.6 [H]` pre/post-commit failure matrix：pre 保持 old authoritative、新文件
  unreferenced；post recover/install new，旧文件为 old slots/readers/reconcile 保留。
- [ ] `T35.7 [M]` pure `should_compact()` tests：physical bytes、ratio、checkpoint
  count、index depth、overflow/boundary；只 recommendation，绝不自动 schedule。
- [ ] `T35.8 [H]` independent logical hash 比 cut-time model，reopen 后 full disk 等价；
  Debug/Release `m3-v4`。
- [ ] `T35.9 [X]` compaction/commit reviewer GO 后完成。

### Task 36 — Safe retention and cleanup

允许：`retention_manager.*`、retention/race tests；删除仅限测试 scratch bundle。

- [ ] `T36.1 [H]` 独立 reference oracle：两个 fully-valid slots + 全部 live
  `BackingView` handle identities + reconciliation/in-flight ownership。
- [ ] `T36.2 [M]` RED matrix：referenced/unreferenced/unknown/replaced/hard-linked/
  reparse/path escape/sharing violation；后五类永不自动删除。
- [ ] `T36.3 [X]` 实现 eligibility pure decision：无 valid slot、无 live view/inflight/
  reconcile reference，且 exact identity 属于当前 bundle/manifest，才返回 eligible。
- [ ] `T36.4 [X]` 实现 handle-anchored best-effort delete；重验 writer/path/identity，
  sharing/crash 只留 harmless artifact+diagnostic，不能伤当前 generation。
- [ ] `T36.5 [H]` deterministic races：read/close/recovery/compaction publication；最后
  handle/reference 释放前 delete backend call 必须为零。
- [ ] `T36.6 [M]` cleanup dry-run report 列候选与拒绝原因；默认不含 absolute path，
  不允许 broad glob/recursive delete。
- [ ] `T36.7 [X]` destructive target、TOCTOU、reference completeness 独立审查；
  Debug/Release `m3-v4`，P0/P1=0 后完成。

## 9. 阶段七：产品面、真机与最终验收（Task 37–44）

### Task 37 — Inspect、verify 与 machine-readable report

允许：`image_inspector.*`、`m3_report.*`、`m3_evidence_publisher.*`、report tests。

- [ ] `T37.1 [M]` 冻结 deterministic JSON fixture：geometry、A/B slots、selected/
  fallback、identities、root/commit、corruption provenance、cleanup candidates、no
  selection；字段顺序/enum string/schema version 固定。
- [ ] `T37.2 [H]` 实现 read-only bounded inspect；用 mutating fake 断言 truncate/
  repair/publish/ack/delete calls 全为零。
- [ ] `T37.3 [H]` 实现 default structural verify 与 `--full` streaming verify；full
  只遍历 logically allocated blocks，内存有预算，corruption 不 fallback。
- [ ] `T37.4 [M]` 实现 redaction：absolute paths/block contents 默认不输出，显式
  diagnostics opt-in；operation id/source commit/binary/artifact hashes 必填。
- [ ] `T37.5 [H]` evidence publish RED：same-directory unique temp→exact write→flush→
  write-through no-overwrite rename；每个 fault 留可诊断 temp，不发布半 JSON。
- [ ] `T37.6 [M]` `Verify-M3Json` fixture tests：unknown/missing enum、schema/hash/
  source mismatch、required SKIP 均 fail closed。
- [ ] `T37.7 [H]` schema/security/publish-order independent review；Debug/Release
  `m3-v2/v4`。

### Task 38 — M3 CLI with local fake storage

允许：`m3_cli.*`、`m3_lifecycle_runner.*`、thin `main.cpp`、CLI tests/CMake。

- [ ] `T38.1 [M]` RED parse matrix：create/inspect/verify/checkpoint/compact/recover-test/
  fault-matrix、help/invalid args、exit code、stdout JSON vs stderr diagnostics。
- [ ] `T38.2 [M]` 实现纯 argument/value validation：geometry/path/timeout/barrier；
  默认 non-destructive，无命令推断/format Windows volume。
- [ ] `T38.3 [H]` route commands 只调用 production session/recovery/coordinator，
  禁止 command-specific codec/publish shortcut。
- [ ] `T38.4 [M]` JSON 必含 stable error + `CommitDisposition`，normal output 无 secrets/
  full path/block bytes；fake phase injection 可由显式 test 参数使用。
- [ ] `T38.5 [H]` lifecycle runner cleanup/close/result precedence review；local fake
  integration 中每个 command 重开验证。
- [ ] `T38.6 [M]` Debug/Release CLI + `m3-v2/v4` current gates，binary/help hashes。

### Task 39 — Integrate M3 with M1/M2 provider

允许：M3 composition root/provider/windows adapter、integration tests/CMake；M1/M2
provider 和 wire golden fixtures 行为只读。

- [ ] `T39.1 [M]` RED lifecycle scenario：recovered startup→lazy read→write/TRIM/zero→
  checkpoint→reconnect→reopen；保存全部 M1 wire golden hash 作为不变基线。
- [ ] `T39.2 [H]` 建 composition interfaces，把 `ImageSession`、`DynamicBlockStore`、
  accepted M1 provider 注入；persistence target 禁止 include AIM/proxy types。
- [ ] `T39.3 [X]` 实现 stop order：stop new external mutations→按 orchestrator 要求
  flush/checkpoint→resolve indeterminate→drain provider/store/session→close handles。
- [ ] `T39.4 [X]` 并发 close/checkpoint/driver disconnect/error precedence tests；不得
  因 transport close 覆盖 durable disposition。
- [ ] `T39.5 [M]` 跑全部 M0/M1/M2 + M3 provider tests；任何 protocol fixture/driver
  contract delta 立即 `BLOCKED`，不得顺手改旧合同。
- [ ] `T39.6 [X]` lifecycle/contract independent review 后完成。

### Task 40 — Safe real-host preflight and E2E harness

允许：PLAN Task40 列出的 PowerShell、JSON verifier、testing docs；默认 dry-run，未经
用户明确 scratch identity 不执行 destructive action。

- [ ] `T40.1 [M]` 实现只读 probe：admin、AIM driver/interface、artifacts、filesystem
  capability/free space/power policy/native VHDX APIs；输出 JSON+human summary。
- [ ] `T40.2 [H]` 冻结 target rejection tests：system volume/workspace root/ambiguous
  device/mounted user data/outside scratch/mismatched disk identity 全拒绝。
- [ ] `T40.3 [M]` 实现 explicit scratch directory + disk identity parsing；不接受
  wildcard、环境变量推断、默认磁盘号或当前目录。
- [ ] `T40.4 [H]` 实现 finally cleanup ledger：provider/proxy/device/drive letter/VHDX/
  temp files；每一步 idempotent，残留输出 exact manual recovery commands。
- [ ] `T40.5 [M]` 实现 verification routing Local/Preflight/RealFunctional/Crash/Soak；
  未实现 tier 返回 `BLOCKED` nonzero，required SKIP 不可 PASS。
- [ ] `T40.6 [M]` `Verify-M3Json` fail-closed schema/source-commit/binary-hash tests。
- [ ] `T40.7 [M]` 无 admin/device 的 dry-run/Pester tests，断言零 format/mount/delete
  调用。
- [ ] `T40.8 [X]` destructive safety reviewer 逐 target/path/cleanup 审查并人工批准
  harness 后完成。

### Task 41 — Real AIM、NTFS、lazy load 与 native VHDX acceptance

必须在人工明确批准的 scratch disk 上执行。允许：frozen harness、NTFS workload、
scenario manifests、生成 evidence；产品代码缺陷另开卡。

- [ ] `T41.1 [M]` 运行 preflight 并记录 admin/driver/version/disk identity/free space/
  source commit/binary hashes；任一 prerequisite 不满足标 `BLOCKED`。
- [ ] `T41.2 [X]` generation 0 经 real AIM export 后，仅对确认 scratch disk 初始化/
  format NTFS；运行 deterministic tree/overwrite/sparse/rename/delete/flush workload。
- [ ] `T41.3 [X]` filesystem/volume flush 后 checkpoint，记录 RAM/backing counters；
  clean stop/reopen，验证 file hashes/metadata，禁止隐藏 format。
- [ ] `T41.4 [H]` cold memory restart 不 repartition/reformat；bounded startup + small
  sample I/O 证明 lazy，再 full corpus exact verify。
- [ ] `T41.5 [X]` 再 flush volume 后 compact exact durable view；offline RAM disk，
  native API read-only attach new base、验证 partition/NTFS、finally detach；禁止双写 attach。
- [ ] `T41.6 [M]` evidence 保存命令/version/seed/counters/hashes/cleanup；machine-
  specific mutable artifacts 不入库，除非 policy 明确。
- [ ] `T41.7 [X]` 人工+独立 reviewer 签署 M3-V5/V6，required SKIP 不能完成。

### Task 42 — Simulated and real-process crash matrix

允许：crash harness/scenarios/evidence schema；power-loss 只有真实受控断电才可这样标注。

- [ ] `T42.1 [M]` 从 Task33 phase inventory 生成 stable scenario manifests；每场景
  固定 expected old/new generation、disposition、slot diagnostics。
- [ ] `T42.2 [H]` simulated crash：clone bytes、named phase kill、fresh recovery process；
  丢弃所有内存/cache，不重用 handles。
- [ ] `T42.3 [X]` real process termination 覆盖 journal flush 前/后、manifest attempt/
  proof 后、reconcile 前/后；未受控断电只能标 `simulated-process-kill`。
- [ ] `T42.4 [X]` 每场景先 read-only recover/verify expected generation+full corpus，
  再检查 fallback provenance；mixed/silent fallback/unresolved indeterminate 全失败。
- [ ] `T42.5 [H]` cleanup gate：device/letter/process/handle 残留或不可复现 outcome
  使 scenario 非零；保存 recovery commands。
- [ ] `T42.6 [M]` 输出 stable IDs/environment/hashes/raw logs，rerun fixed seeds。
- [ ] `T42.7 [X]` independent crash oracle reviewer GO 后完成 M3-V2/V6/V7 对应项。

### Task 43 — Soak、resource、performance 与 final M3 verdict

只允许 soak harness/verifier/evidence/report；发现代码缺陷回到所属 Task，不在 verdict
卡修产品。

- [ ] `T43.1 [M]` clean Release build + complete CTest，归档命令、counts、commit、
  binary/artifact hashes；旧日志无效。
- [ ] `T43.2 [H]` 冻结 soak seeds/workload/thresholds：mixed read/write/TRIM/
  checkpoint/reopen/compaction、concurrent first-touch、low-memory、I/O faults、close。
- [ ] `T43.3 [M]` 执行 soak 并收集 memory/cache/COW stall/journal ratio/index depth/
  handles/threads/devices/I/O bytes/data hashes；阈值是 gate，不是 observation。
- [ ] `T43.4 [X]` 评估 bounded-memory/lazy-I/O/no-leak/no-mixed-generation；任何
  threshold 超界或 flaky seed 为 NO-GO，不扩大阈值。
- [ ] `T43.5 [M]` soak 后重跑 native VHDX、real AIM/NTFS、crash scenarios；required
  prerequisite SKIP 单列，不能变 GO。
- [ ] `T43.6 [X]` 按内部 M3-V0 和官方 M3-V1..V7 逐项签 `GO/NO-GO/BLOCKED`，列
  artifacts/unresolved defects/limitations；不得用总 bool 掩盖 gate。
- [ ] `T43.7 [X]` 独立最终验收人核对 raw evidence 和 source/binary hashes。

### Task 44 — Documentation、license 与 final drift closure

允许：README/status/license notices、M3 SPEC/PLAN/TASKLIST、operator docs；产品代码
变更必须退回所属 Task。

- [ ] `T44.1 [M]` 文档 inventory：supported/unsupported VHDX profile、bundle files、
  backup/restore、failure outcomes、compaction、shutdown、CommitIndeterminate recovery。
- [ ] `T44.2 [M]` third-party inventory：每个 header/library/tool 的来源/version/
  license/redistribution；AIM user-mode code 未经单独审查不得复制。
- [ ] `T44.3 [H]` 逐条对照 implementation evidence 修正 docs；不得修改 SPEC 来
  追认无证据行为，设计变更需单独 decision/review。
- [ ] `T44.4 [X]` line-by-line drift audit：M1 protocol、AIM、M2 semantics、durability
  order、error/disposition、cleanup authority、M4 handoff 全列结论。
- [ ] `T44.5 [M]` fresh clean-tree Debug/Release build/tests 和 docs commands；记录
  当前 commit/hashes/counts，任何 required failure 回所属 Task。
- [ ] `T44.6 [X]` 只有 Task43 GO、所有 required evidence 可复现、license 清楚、
  drift P0/P1=0 时才发布 M3 final GO。

## 10. 任何 Agent 都不得越过的暂停点

遇到以下任一情况，写执行记录并停止：

- 当前卡是 `H/X` 且没有对应推理等级/reviewer；
- 需要改变 SPEC 冻结格式、commit point、install-before-ack 或 recovery selector；
- 首次 RED 不是目标失败，或测试只能靠 sleep/retry 才失败；
- corruption 被降级为 Absent/Zero，dirty state 在 durable proof 前被 ack；
- manifest slot-write attempt 后仍试图返回普通 `NotCommitted`；
- 任何 cleanup 目标仅由字符串/path/glob 推导，没有 validated identity/reference proof；
- required real-host gate 被 SKIP，但有人要求写 GO；
- 需要修改当前卡禁止文件、用户真实数据或 M0/M1/M2 frozen contracts。

此时唯一合法输出是：

```text
PAUSED_FOR_HIGH_REASONING: <card-id>
Blocking evidence: <command/file/test>
Decision required: <one precise question>
No out-of-scope files changed: yes/no
```
