# M2 Dynamic Block Store TASKLIST

这是一份给执行型 Agent 使用的细颗粒度清单。它不能替代设计和实现计划；执行者必须同时服从：

1. `docs/superpowers/specs/2026-08-01-m1-m2-proxy-dynamic-block-store-design.md`
2. `docs/superpowers/plans/2026-08-03-m2-dynamic-block-store.md`
3. 本文件

本清单以 `master` 的 M2 规划基线 `2adf84a` 为起点；M1 验收文档记录的功能验收提交为 `3eac4f7`。Task 00 仍必须重新验证当前事实，没有新鲜的 `M1 GO` 回归证据不得开始 M2 产品代码。

---

## A. Agent 执行纪律

### A1. 每次开始工作前

- [ ] 运行 `git status --short`，记录已有修改、暂存和未跟踪文件；默认它们属于用户，不清理、不覆盖。
- [ ] 阅读本文件的“总状态表”、当前任务全部内容和下一任务标题。
- [ ] 阅读 Plan 中同编号任务，并回看 Plan 第 0、1、3、7、8 节。
- [ ] 阅读 SPEC 中与当前任务对应的接口、状态、门禁和排除项。
- [ ] 确认当前任务所有前置任务为 `DONE`，对应证据路径可访问。
- [ ] 将当前任务状态从 `TODO` 改为 `DOING`；同一时间只能有一个 `DOING`。
- [ ] 在任务记录中写下开始 commit、分支、时间、假设和本次允许修改的文件。
- [ ] 先写能够失败的测试或验证，再改产品代码。若测试无法先写，记录可审计原因。

### A2. 每次结束一个任务前

- [ ] 运行该任务的聚焦测试，并保存命令、退出码和摘要。
- [ ] 运行该任务要求的累计验证层；不能只跑新测试。
- [ ] 运行 `git diff --check`。
- [ ] 检查 `git status --short`，确认没有碰用户无关文件。
- [ ] 逐项回读 Plan 同编号任务，写一段“偏航检查”：目标、边界、接口、测试、证据是否一致。
- [ ] 检查是否意外引入 M3/M4 内容、代理对存储内部的依赖、驱动对存储的依赖或静默降级。
- [ ] 若当前任务涉及 backing、Dirty、Clean 或 eviction，逐条核对：不可变 view、generation 单调递增、install-before-ack、ack 后可安全 reload。
- [ ] 在本文件任务记录中填写文件、测试、结果、证据、未解决风险和 commit。
- [ ] 只有全部验收条件满足后，才能将任务标为 `DONE`；否则保持 `DOING` 或标为 `BLOCKED`。
- [ ] 若提交代码，只提交当前任务相关文件。不得用破坏性 Git 命令处理用户修改。

### A3. 发现缺陷时

- [ ] 立即保留失败命令、种子、日志、JSON、环境和退出码。
- [ ] 将当前任务标为 `BLOCKED`，不要用重试掩盖非确定性失败。
- [ ] 在最小层级增加确定性红灯测试。
- [ ] 修复后依次跑：聚焦测试 → 当前层 → 受影响的所有已完成层。
- [ ] 产生新的证据目录；禁止编辑失败 JSON 使其变成成功。
- [ ] 数据损坏、脏数据丢失、backing resurrection、错误磁盘、清理失败均为立即停止条件。

### A4. 状态值

- `TODO`：未开始。
- `DOING`：正在执行；全表最多一个。
- `BLOCKED`：已有具体阻塞和失败证据。
- `DONE`：实现、累计验证、证据和偏航检查全部完成。

---

## B. 总状态表

执行者必须在每次任务结束时更新此表，不得只在对话中报告。

| ID | 任务 | 状态 | 依赖 | 主要门禁 |
|---|---|---|---|---|
| 00 | M1 基线与 M2 构建壳 | DONE | 无 | M2-V0 |
| 01 | 几何、选项、状态与统计类型 | DONE | 00 | M2-V1 |
| 02 | 不可变 Backing 接口、Zero 与 Fake | DONE | 01 | M2-V1 |
| 03 | Windows 虚拟内存 Arena | DONE | 01 | M2-V1 |
| 04 | 稀疏目录与固定锁条带 | DONE | 01 | M2-V1 |
| 05 | Store 工厂、生命周期、默认状态 | DONE | 02,03,04 | M2-V1 |
| 06 | Zero/Resident READ | DONE | 05 | M2-V1 |
| 07 | Backing 懒加载与 Read-through | DONE | 06 | M2-V1 |
| 08 | WRITE 与内容版本 | DONE | 07 | M2-V1 |
| 09 | UNMAP/ZERO 与零覆盖 | DONE | 08 | M2-V1 |
| 10 | Backing generation、脏快照与 persisted ack | TODO | 09 | M2-V1 |
| 11 | Clean CLOCK eviction | TODO | 10 | M2-V1 |
| 12 | Hard limit、预算和失败回滚 | TODO | 11 | M2-V1 |
| 13 | 100,000 次参考模型 | TODO | 12 | M2-V2 / M2-A |
| 14 | 单飞加载与同块并发 | TODO | 12 | M2-V3 |
| 15 | 快照/变更/淘汰/关闭竞态 | TODO | 14 | M2-V3 |
| 16 | 长压测、不变量、资源核算 | TODO | 13,15 | M2-V3 / M2-B |
| 17 | 注入 M1 Proxy 与本地 SHM | TODO | 16 | M2-V4 |
| 18 | 64 GiB/1 TiB 真实 VM 集成 | TODO | 16 | M2-V2 |
| 19 | CLI、生命周期、报告、性能探针 | TODO | 17,18 | 本地假验收 |
| 20 | 安全 NTFS 脚本与总编排 | TODO | 19 | Preflight |
| 21 | 执行 M2-A/B 与 M2-C 功能门 | TODO | 20 | M2-V1..V5 |
| 22 | 性能、故障、20 轮与最终判定 | TODO | 21 | M2-V6..V7 |

---

## C. 通用构建与证据约定

必须使用仓库已提交的 preset 名，不允许每个 Agent 自己猜或发明别名：

```powershell
cmake --preset msvc
cmake --build --preset release
ctest --preset release --output-on-failure
```

所有 generation 相关实现和测试必须服从唯一顺序：

```text
copy_dirty_batch
  -> 构造完整且不可变的 successor backing（M2 使用 Fake）
  -> install_backing_view(new_generation)
  -> mark_persisted(exact version, exact kind, same generation)
  -> Clean 才可被 eviction
  -> reload 必须来自已安装 successor view
```

ack 发生在 install 之前、generation 不晚于 dirty base、generation 尚未安装、version/kind 已变化时都必须保留 Dirty。

每个任务在本节下追加一条记录或链接到 `docs/validation/m2-dynamic-block-store-acceptance.md`：

```text
Task:
Status:
Start commit / end commit:
Files changed:
Red test and observed failure:
Focused verification command / exit code:
Cumulative verification command / exit code:
Evidence paths:
Plan drift audit:
Open risks:
Executor / timestamp:
```

---

## Task 00 — 冻结 M1 基线并建立 M2 构建壳

**目标：** 把“假设 M1 完成”转化为可审计事实，并建立不改变 M1 行为的 M2 target。

**允许文件：** `src/CMakeLists.txt`、`tests/CMakeLists.txt`、`docs/validation/m2-dynamic-block-store-acceptance.md`、仅为可编译 `myrd-m2-e2e` target 所需的空入口文件。不得修改 M1 行为代码。

### 前置与事实检查

- [x] M0 acceptance 证据明确写出最终 `M0 GO`，namespace/driver-only contract 已关闭。
- [x] `docs/validation/m1-proxy-acceptance.md` 存在且明确写出 `M1 GO`，不是草稿或条件通过。
- [x] 记录 M1 功能验收提交 `3eac4f7` 和 merged planning baseline `2adf84a`；确认当前历史包含二者或明确当前 worktree 与它们的关系。
- [x] 记录当前 `git status --short`，特别保护用户已有修改和未跟踪诊断文件。
- [x] 记录 `cl`/MSVC、Windows SDK、CMake、CTest、PowerShell、架构均为预期版本。
- [x] 确认进程为 64 位构建目标。
- [x] 阅读 M1 `IBlockDevice`、`OperationContext`、结果类型、provider 生命周期、报告和 cleanup API。
- [x] 记录这些真实路径和 target 名；若与 Plan 示例不同，写出唯一映射。
- [x] 执行 M1 Plan 规定的 fresh local gate 并保留输出。
- [x] 若任一项失败：标记 `BLOCKED`，停止；不得进入 Task 01。

### 测试与实现

- [x] 先添加一个配置/链接测试，证明现有 `myrd_storage` 可继续独立于 driver/proxy 实现 target 构建；不得创建第二个 storage library。
- [x] 添加 `myrd-m2-e2e` executable target，但不伪造存储行为。
- [x] 添加/预留 `m2-v1`、`m2-v2`、`m2-v3`、`m2-v4` CTest labels。
- [x] 沿用 M1 的 warnings、runtime library、Unicode、异常/RTTI策略；记录任何差异。
- [x] 新建 acceptance 文档，写入机器、工具链、基线 commit、工作树、M1 证据、命令和结果。
- [x] Release configure/build 成功。
- [x] 再跑 M1 local gate，证明 CMake 改动未回归。

### 任务验收

- [x] M2-V0 所有事实可通过文件和命令重现。
- [x] `myrd_storage` 不依赖 driver/proxy 实现；`myrd-m2-e2e` 可以依赖已验收的 M1/driver targets。
- [x] M1 源码行为未变化。
- [x] 回读 Plan Task 0 与第 0 节，完成偏航检查。
- [x] 建议提交：`build(m2): establish dynamic store targets and baseline`。

---

## Task 01 — 定义 checked geometry、options、状态和 stats

**状态：DONE**

**目标：** 先固定所有数字、边界和公开类型，避免后续模块各自解释 64 KiB、limit 或版本。

**文件：** `src/storage/block_constants.h`、`block_math.h`、`dynamic_block_types.h`、`tests/unit/block_math_tests.cpp`、CMake。

### 红灯测试

- [x] 64 KiB 常量、1,024 blocks/leaf、64 MiB/leaf。
- [x] checked add 正常、`UINT64_MAX` 溢出。
- [x] 向 64 KiB 上取整正常、已对齐、零和溢出。
- [x] request 切分：块内、跨 2/3 块、末尾 partial logical block。
- [x] offset+length 溢出、超 logical size、非 sector 对齐、buffer 长度错误。
- [x] 64 GiB 和 1 TiB 的 block/leaf/reservation 数值。
- [x] option：`0 < low <= soft < hard` 每个反例。
- [x] limits 必须块对齐。
- [x] snapshot bytes 至少 64 KiB且块对齐，records > 0。
- [x] stripes 为 2 的幂且 >=64。
- [x] `BackingGeneration` 使用 `uint64_t`；generation 回绕被识别为 fatal invariant。
- [x] `DirtyBlockSnapshot` 精确包含 `base_generation`；stats 包含 current generation、view installs、generation conflicts、stale-load retries。
- [x] `BackingUnresident` 缺 backing 失败；`CleanZero` null backing 的规范化行为。
- [x] version increment 溢出被识别为 fatal invariant。

### 实现

- [x] 只使用固定宽度无符号整数和 checked helpers，不允许隐式 narrowing。
- [x] 定义 `kBlockSizeBytes`、`kBlocksPerLeaf`。
- [x] 定义 `InitialBlockState`、稳定/瞬态 state、options、create args。
- [x] 定义 `BackingGeneration`、`DirtyRecordKind`、带 `base_generation` 的 `DirtyBlockSnapshot`、`DirtyScanCursor` 和 `BackingGenerationConflict`。
- [x] 定义 SPEC 的完整 stats 字段：logical/reserved/committed，四种 resident/zero block counts、loading、evicted、backing reads/bytes、uncached reads、unmap/zero bytes、hard rejects、commit/decommit failures、snapshot records/conflicts、current generation、view installs/generation conflicts、stale-load retries。
- [x] `CleanZero` 的 implicit zero blocks 通过总块数减去偏离默认状态的块计算，统计不得创建 leaves。
- [x] library 不提供隐藏 product defaults；create caller 必须显式传全部 options。
- [x] 为所有公开 enum 提供稳定的报告字符串，不依赖 ordinal。
- [x] 编译失败检查：production header 不包含 AIM/driver header。

### 验证与验收

- [x] `unit.m2.block_math` 通过。
- [x] M2-V1 当前可用测试通过。
- [x] `git diff --check` 通过。
- [x] 回读 Plan Task 1、1.1、1.2、1.6；确认没有发明新 block size 或 limit。
- [x] 建议提交：`feat(storage): define dynamic block geometry and options`。

---

## Task 02 — 建立不可变 IBackingStore、ZeroBackingStore 与 FakeBackingStore

**目标：** 用最小、线程安全且发布后不可变的接口表示“Data 或 AllZero”，为懒加载、generation 切换和并发测试提供可控后端。

**文件：** `src/storage/backing_store.*`、`zero_backing_store.*`、`tests/support/fake_backing_store.h`、`tests/unit/backing_store_tests.cpp`、CMake。

### 红灯测试

- [x] destination 不是恰好 64 KiB 时失败。
- [x] ZeroBackingStore 返回 `AllZero`，不依赖 destination 初始内容。
- [x] Fake 根据 `(seed, block_index)` 产生稳定 Data。
- [x] Fake 可把指定 block 标为 `AllZero`。
- [x] 多线程并发加载不同块正确。
- [x] per-block/total load count 精确。
- [x] 可用 barrier/gate 暂停和释放加载，不使用 sleep 猜时序。
- [x] 可注入指定 block/第 N 次失败。
- [x] cancellation/deadline 在接口结果中保留。
- [x] `load_block` 可通过 `shared_ptr<const IBackingStore>` 调用。
- [x] Fake 的块内容发布后不可修改；下一 generation 必须构造新 Fake 对象。
- [x] 旧 Fake 在 in-flight load 持有引用时不会析构，调用完成后可释放。

### 实现

- [x] `IBackingStore::load_block` 是 `const`，签名与 SPEC 完全一致。
- [x] 接口析构为 virtual；没有文件/VHD/driver 概念。
- [x] Zero 实现无状态、线程安全，不将 64 KiB 清零工作错误放入 backing。
- [x] Fake 只链接测试 target，不进入 production library。
- [x] Fake 的 gate、mutable 计数和故障注入自身线程安全；测试 builder 能由旧完整内容加 patch 产生新的完整不可变 Fake。

### 验证与验收

- [x] `unit.m2.backing_store` 通过。
- [x] TSAN 不可用时至少用并发测试和锁审查证明 fake 自身无竞态。
- [x] 回读 Plan Task 2 与 1.3；确认未提前实现 M3 file backing。
- [x] 建议提交：`feat(storage): add immutable block backing contract`。

---

## Task 03 — 实现 Windows VirtualMemoryArena

**目标：** 只 reserve 大地址空间，按 64 KiB commit/decommit，并让每个 Win32 错误可测试。

**文件：** `src/storage/virtual_memory_arena.*`、`tests/support/fake_virtual_memory_api.h`、`tests/unit/virtual_memory_arena_tests.cpp`、CMake。

### 红灯测试

- [x] reserve 调用参数为 `MEM_RESERVE | PAGE_NOACCESS`。
- [x] 64 GiB 和 1 TiB reservation 的 checked 大小。
- [x] block index 到稳定地址的计算，含首块、末块和越界。
- [x] commit 仅用 `MEM_COMMIT | PAGE_READWRITE` 且恰好 64 KiB。
- [x] decommit 仅用 `MEM_DECOMMIT` 且恰好 64 KiB。
- [x] 整体销毁只对 base 使用一次 `MEM_RELEASE`，size 参数符合 Win32 要求。
- [x] reserve/commit/decommit/release/VirtualQuery 分别可注入失败且错误码可见。
- [x] double commit/decommit 或越界 transition 被拒绝/捕获。
- [x] partial construction、move/lifetime 不 double release。
- [x] committed accounting 仅在真正成功时变化。
- [x] VirtualQuery 可交叉验证 committed/decommitted page state。

### 实现

- [x] 建立窄 VM seam；不得把 fake 条件编译散落到产品逻辑。
- [x] factory reserve 成功后立刻由 RAII 对象拥有。
- [x] 地址运算全部 checked，返回 pointer 前验证 block index。
- [x] block release 使用 decommit，不得 `MEM_RELEASE` 子区间。
- [x] decommit 失败时向上返回，不把 metadata 假装成 unresident。
- [x] 析构不抛异常；release 错误进入可观测 fatal/cleanup 路径。

### 验证与验收

- [x] `unit.m2.virtual_memory` 通过。
- [x] Application Verifier/guard page 如已有配置则通过。
- [x] 回读 Plan Task 3 与 1.1，核对每个 flag/size。
- [x] 建议提交：`feat(storage): add reserved virtual memory arena`。

---

## Task 04 — 实现稀疏 BlockDirectory 和 lock stripes

**目标：** 1 TiB 逻辑盘不能预分配每块 metadata；并发状态通过固定锁条带协调。

**文件：** `src/storage/block_directory.*`、`dynamic_block_store_internal.h`、`tests/unit/block_directory_tests.cpp`、CMake。

### 红灯测试

- [x] leaf index / entry index 的首尾边界。
- [x] 每 leaf 恰好 1,024 entries，覆盖 64 MiB。
- [x] 空目录只分配 top-level pointers，不分配 leaves。
- [x] 第一次访问按需创建一个 leaf。
- [x] 同 leaf 并发创建最终只发布一个 leaf。
- [x] 不同 leaf 可并发创建。
- [x] 已发布 entry 地址直到 store 析构保持稳定。
- [x] implicit initial state 不需要 entry。
- [x] stripe 映射为 `block_index & (count - 1)`，same/different stripe 用例明确。
- [x] 64 GiB/1 TiB construction metadata 不与 block 数线性增长为完整 entries。

### 实现

- [x] `BlockEntry` 包含 state、prior state、owner/epoch、version、pin、reference bit、transition/load epoch、zero dirty kind、`dirty_base_generation`、`clean_generation`。
- [x] 每块不放 mutex/CV。
- [x] top-level 使用安全 publication 方案；leaf publication lock 持有时间极短。
- [x] published leaf 不回收，防止悬空 entry 指针。
- [x] 固定 stripe 数在 factory 时一次创建，每条含 mutex/CV。
- [x] backing I/O、VM commit/decommit、wait 均不得持 leaf publication lock。
- [x] 增加 debug/test invariant checker。
- [x] invariant checker 明确拒绝 generation 回绕，以及 `ResidentClean.clean_generation > current backing generation`。

### 验证与验收

- [x] `unit.m2.block_directory` 通过并含并发 publication。
- [x] 记录 64 GiB/1 TiB 初始 metadata 估算/实测。
- [x] 回读 Plan Task 4 与 1.4；确认没有 per-block lock 和 eager leaves。
- [x] 建议提交：`feat(storage): add sparse block metadata directory`。

验收记录（2026-08-03）：顶层目录实测为 64 GiB = 8,192 bytes、1 TiB = 131,072 bytes，初始 leaf 数均为 0；并发 publication 重复 100 次通过，规格审查 COMPLIANT，质量审查 APPROVED。

---

## Task 05 — Store factory、lifecycle、默认 zero 与 stats snapshot

**目标：** 产生只能“完整有效或明确失败”的 `DynamicBlockStore`，直接实现 M1 `IBlockDevice`。

**文件：** `src/storage/dynamic_block_store.h`、`dynamic_block_store_core.cpp`、`tests/unit/dynamic_block_store_core_tests.cpp`、CMake。

### 红灯测试

- [x] 每条 create args validation 的成功和失败。
- [x] public logical size 保持原值，reservation size 向 64 KiB 对齐。
- [x] CleanZero implicit block 无 leaf/commit/backing load。
- [x] BackingUnresident implicit block 需要 backing。
- [x] CleanZero null backing 被统一 normalize 为 ZeroBackingStore。
- [x] 新空盘显式 initial generation `0`；恢复场景允许显式非零 generation，stats/API 精确返回。
- [x] implicit CleanZero 逻辑使用 initial generation，但不得仅为记录该值创建 leaf。
- [x] initial pointer/generation 作为一个 coherent pair 发布；null pointer 或 generation 回绕原子失败。
- [x] reserve/stripe/directory/worker 各阶段失败都无泄漏。
- [x] 初始 stats、geometry、options snapshot 精确。
- [x] close 后新请求被拒绝。
- [x] 析构顺序可通过 fake 验证：拒绝新请求→唤醒→停 worker→quiesce→释放 arena。
- [x] 通过 M1 interface compile test，无 proxy downcast。

### 实现

- [x] 使用 fallible `create` factory；构造函数不发布半成品。
- [x] `DynamicBlockStore` 直接实现真实 M1 `IBlockDevice`。
- [x] 若 ABI 强制 adapter，记录原因、路径和依赖方向；adapter 不能泄露 internals。
- [x] `shared_ptr<const IBackingStore>` 生命周期覆盖所有 load；factory 建立唯一初始 `BackingView`。
- [x] factory 严格按 Plan 顺序拥有资源。
- [x] closing flag 与 worker/waiter 通知由单一 lifecycle owner 管理。
- [x] stats snapshot race-free，但 stats 不参与正确性判断。
- [x] 公开 `backing_generation`、`install_backing_view`、snapshot/ack/invariant test hook 的最小接口；mutable internals 私有。

### 验证与验收

- [x] `unit.m2.store_core` 通过。
- [x] M1 interface/protocol 编译测试通过。
- [x] 构造失败矩阵无 reservation/thread/handle 泄漏。
- [x] 回读 Plan Task 5、1.2、1.4；确认 store 不知道 driver/SCSI。
- [x] 建议提交：`feat(storage): create dynamic block store lifecycle`。

验收记录（2026-08-03）：`unit.m2.store_core` 15/15 通过并连续重复 100 次；M2-V1 9/9、Release 全量 32/32 通过。构造失败覆盖 reserve、stripe、directory、worker 启动前和启动后回滚；真实 waiter 关闭唤醒、observer 重入、coherent backing view 与锁外 backing 析构均有回归测试。规格审查 COMPLIANT，质量审查 APPROVED。

---

## Task 06 — 实现 Zero/Resident READ

**目标：** 在不做 backing I/O 的状态上完成完全正确的 read path，并固定 pin/copy 规则。

**文件：** `src/storage/dynamic_block_store_read.cpp`、`tests/unit/dynamic_block_store_read_tests.cpp`、CMake。

### 红灯测试

- [x] implicit CleanZero 全块、部分、跨块读取全零且不创建 leaf。
- [x] explicit `ZeroClean`/`ZeroDirty` 返回零且不 commit/load。
- [x] `ResidentClean`/`ResidentDirty` 全块和部分读取字节精确。
- [x] 跨 2/3 block 读取正确拼接。
- [x] 最后 partial logical block 边界正确。
- [x] 非对齐、overflow、越界、buffer mismatch 在任何 copy 前失败。
- [x] cancellation/deadline 在开始和块间被观察。
- [x] pin 阻止测试中的 decommit/transition；copy 后 pin 归零。
- [x] reference bit 在成功 resident read 后设置。

### 实现

- [x] 共用 Task 01 checked range splitter。
- [x] zero read 只填请求 destination span。
- [x] resident path：锁内确认并 pin → 解锁 copy → 加锁 unpin/reference/wake。
- [x] 不在 metadata lock 下复制大 buffer。
- [x] 不读取未 commit arena 地址。
- [x] `Result<void>` 不报告 transferred-byte success；后续块取消/失败时返回错误，已经复制的 destination prefix 可以保留。

### 验证与验收

- [x] `unit.m2.read` 通过。
- [x] zero reads 的 leaf/commit/load counters 保持零。
- [x] 回读 Plan Task 6 与 1.5；核对 range 和 cancellation。
- [x] 建议提交：`feat(storage): implement zero and resident reads`。

验收记录（2026-08-03）：`unit.m2.read` 11/11 通过并连续重复 100 次；覆盖 implicit/explicit zero、ResidentClean/Dirty、2/3-block 拼接、最后 partial block、全量预校验、取消/deadline 前缀语义、pin/reference、未 commit Resident 防护与 observer 重入关闭。Resident copy 使用 RAII pin，测试 decommit 使用 `ResidentClean -> Evicting -> Unresident` 且在 stripe 锁外释放内存。规格复验 COMPLIANT，质量复验 APPROVED。

---

## Task 07 — 单飞 lazy backing load 与 hard-pressure read-through

**状态：DONE**

**目标：** 第一次访问才载入 64 KiB；同块并发只调用 backing 一次；无法缓存时 read 仍可有界完成。

**文件：** `dynamic_block_store_read.cpp`、internal header、read/limit tests。

### 红灯测试

- [x] `Unresident + Data -> ResidentClean`，内容和 load count 正确。
- [x] `Unresident + AllZero -> ZeroClean`，无 commit。
- [x] 2/8/32 个同块 readers 只产生一次 backing load。
- [x] 不同块可同时 load，不被 global lock 串行。
- [x] waiter cancellation/deadline 独立返回，不取消 owner。
- [x] owner cancellation/failure 恢复 prior state 并唤醒全部 waiter。
- [x] commit failure 恢复 stable state、释放 budget、无不可访问地址。
- [x] close during load 可合作取消并 join。
- [x] hard limit 无 victim 时，Data 由 private buffer read-through，状态回 `Unresident`。
- [x] hard limit 下 8 个同时 miss 的 readers 共享同一 load 和 epoch-scoped private buffer。
- [x] read-through counter 精确按成功 reader 计数，commit bytes 不变；下一次读可再次 load。
- [x] gate 阻塞 generation G 的 load，安装内容不同的 G+1，再释放旧 load：旧结果不发布，自动从 G+1 重试。
- [x] 上述竞态的 `stale_backing_load_retries` 精确 +1；最终 Clean 记录 G+1；旧 backing 在调用返回前保持存活，之后可释放。

### 实现

- [x] 锁内用 `Unresident -> Loading` 和 owner epoch 选主。
- [x] waiter 使用 predicate loop，处理 spurious wake、cancel、deadline、close。
- [x] 原子捕获 `{generation, shared_ptr<const IBackingStore>}`，backing I/O 写入私有 64 KiB 对齐 buffer，期间不持 metadata/leaf lock。
- [x] AllZero publish 前由 store 负责在锁外将 read 目标清零，并用短暂 publication gate 阻止 generation 穿越。
- [x] publish 前复核 owner/epoch 和 captured generation；generation 已变化则丢弃旧结果并有界重试。
- [x] generation 未变化时，Data 先获得 budget，再 commit/copy，最后以 captured generation publish `ResidentClean`；AllZero 同样记录 `ZeroClean.clean_generation`。
- [x] 每条失败路径检查 owner epoch 后恢复 prior state并 notify_all。
- [x] hard limit 缓存失败只对 read 允许 read-through；write 不允许。
- [x] 不把一个 waiter 的取消传播给仍有价值的 owner load。
- [x] waiter 计数由异常安全 RAII 注销；read-through 裸指针发布后由 fail-fast guard 保证不会悬空返回。

### 验证与验收

- [x] `unit.m2.read`、`unit.m2.limits` 通过。
- [x] 有测试证明 backing call 期间其他 stripe 能前进。
- [x] 回读 Plan Task 7 与 1.3/1.4；核对 single-flight、immutable view lifetime、generation retry 和 lock boundary。
- [x] 两轮独立复审最终均为 APPROVED，无 Critical、Important 或 Minor。
- [x] 建议提交：`feat(storage): add lazy block loading and read through`。

---

## Task 08 — WRITE、copy-on-load 与内容版本

**状态：DONE**

**目标：** 写入发布完整块镜像和新版本；任何 reader/snapshot 都看不到 torn content。

**文件：** `dynamic_block_store_write.cpp`、internal header、`dynamic_block_store_write_tests.cpp`、CMake。

### 红灯测试

- [x] full-block write 覆盖 implicit zero、ZeroClean、ZeroDirty、Unresident、ResidentClean、ResidentDirty。
- [x] full-block write 对 Unresident 不调用 backing。
- [x] partial write 对 Unresident 恰好 load 一次并保留未覆盖旧 bytes。
- [x] partial write 对 AllZero/zero states 从完整零块开始。
- [x] partial resident write 只改变目标 bytes。
- [x] 跨块 write 与末块边界。
- [x] invalid range 在任何 state/version/commit 改变前失败。
- [x] hard limit 无空间且需要新 commit 时 write 返回 `MemoryHardLimit`，旧内容不变。
- [x] 已 Resident block 的 overwrite 不需要新 commit，即使 committed bytes 已到 hard limit也可继续。
- [x] backing/commit/cancel/deadline failure 均保留当前块旧内容；跨块请求已完成前缀可保留但整体 `Result<void>` 返回错误。
- [x] 每个成功变更 block 的 version 只增加一次；溢出 fatal。
- [x] 每个成功新 version 同时捕获当前 installed generation 为 `dirty_base_generation`；失败/no-op 不改变该值。
- [x] reader 并发时只观察旧块或新块，不能观察半块。

### 实现

- [x] full write 直接建立 64 KiB image，不 load backing。
- [x] partial Unresident 用 Task 07 的 single-flight/private buffer 逻辑，不复制出第二套状态机。
- [x] zero partial image 先完整清零再 patch。
- [x] resident mutation 使用明确排他 transition/pin 协议。
- [x] 先取得 commit budget，成功 commit/copy 后才 publish `ResidentDirty`。
- [x] copy-then-publish，避免失败后半写状态。
- [x] version、bytes、Dirty state 和 `dirty_base_generation` 在同一 publish 线性化点生效。
- [x] multi-block partial-completion 与 M1 byte count/error 语义一致。
- [x] stats 只在对应 block 成功 publish 后更新。

### 验证与验收

- [x] `unit.m2.write` 通过。
- [x] read tests 和 limit tests 仍通过。
- [x] 回读 Plan Task 8 与 state table；确认 full write 不 load backing。
- [x] 建议提交：`feat(storage): implement versioned dynamic writes`。

---

## Task 09 — UNMAP/ZERO 与 backing resurrection 防护

**状态：DONE**

**目标：** 释放可释放内存，同时明确记住“这里必须为零”，使旧 backing 永远不会复活。

**文件：** `dynamic_block_store_ranges.cpp`、internal header、`dynamic_block_store_range_tests.cpp`、CMake。

### 红灯矩阵

- [x] 为 5 个 stable states × full ZERO 写测试。
- [x] 为 5 个 stable states × full UNMAP 写测试。
- [x] 为 5 个 stable states × partial ZERO 写测试。
- [x] 为 5 个 stable states × partial UNMAP 写测试。
- [x] 每组分别使用 ZeroBacking 与 non-zero FakeBacking。
- [x] full operation 不 load old backing。
- [x] full ZERO 在 non-zero/unknown backing 上得到 `ZeroDirty(Zero)`。
- [x] full UNMAP 在 non-zero/unknown backing 上得到 `ZeroDirty(Discard)`。
- [x] partial operation 对 Unresident/Resident 生成完整 `ResidentDirty Data` image。
- [x] partial operation 对 ZeroClean/ZeroDirty 是 no-op，不 commit、不增 version。
- [x] resident full operation 安全 decommit，commit accounting 下降。
- [x] decommit failure 保持原 resident 内容/状态。
- [x] repeated ZERO/UNMAP 按“最后一次操作 kind + 新 version”规则。
- [x] 每次成功产生新 version 时，`dirty_base_generation` 等于该线性化点的 installed generation。
- [x] 随后 read 永远为零，且 `ZeroDirty` 不触发 backing load。
- [x] cancellation/invalid range/close race 无半个 range 的虚假 full success。

### 实现

- [x] range validation 在 mutation 前完成。
- [x] full operation 以 block transition 排他，等 pins 清零。
- [x] 先保证 zero override metadata 可发布，再进行安全 decommit/state publish。
- [x] 所有成功 full ZERO/UNMAP（包括 CleanZero）都发布 `ZeroDirty`，分别记录 Zero/Discard，并增加 version。
- [x] partial operation 对 zero state 直接 no-op；其他 state 复用 load/read-modify-write helper。
- [x] version 和 DirtyRecordKind 与最新成功请求一致。
- [x] version、DirtyRecordKind 和 `dirty_base_generation` 在同一 publish 中一致。
- [x] Dirty zero state 不占 committed data memory但计入 dirty metadata/stat。

### 验证与验收

- [x] `unit.m2.ranges` 通过完整矩阵。
- [x] read/write/limits regressions 通过。
- [x] 回读 Plan Task 9 和 state table，明确回答为何 backing 不会复活。
- [x] 建议提交：`feat(storage): implement unmap and zero ranges`。

验收记录（2026-08-03）：`unit.m2.ranges` Debug 连续重复 100 次通过；Debug/Release focused read/write/limits/ranges 4/4，通过全量回归各 36/36，M2-V1 13/13。状态矩阵覆盖 5 个 stable states、ZERO/UNMAP、full/partial、ZeroBacking/non-zero backing；跨块用例证明仅 partial 首尾加载旧 backing，full 段不加载。resident decommit 失败与后续块失败分别证明精确回滚和已完成前缀语义，close 等待 pin 时恢复原状态。`ZeroDirty` 是显式 dirty override，read 直接返回零且不接触 backing，因此旧 backing 无法复活；Plan Task 9、SPEC 状态表与第 14 节已回读核对。

---

## Task 10 — Backing generation、dirty snapshot 与 exact persisted ack

**目标：** 为 M3 提供不会在 eviction/reload 后复活旧数据的批处理协议；本阶段只用完整不可变 Fake successor 验证协议，不写文件、不做 durable 承诺。

**文件：** `dynamic_block_store_backing.cpp`、`dynamic_block_store_snapshot.cpp`、public/internal headers、`dynamic_block_store_backing_generation_tests.cpp`、`dynamic_block_store_snapshot_tests.cpp`、CMake。

### 红灯测试：snapshot

- [ ] 空 store scan 返回空，cursor 到 total blocks。
- [ ] cursor 从 0 递增扫描，完成后再次调用仍空；显式 reset 到 0 可重扫。
- [ ] deterministic ascending block order。
- [ ] Data record 含 block index/version/`base_generation`/恰好 64 KiB payload。
- [ ] Zero/Discard record 不含 payload，但 kind 与 `base_generation` 精确保留。
- [ ] max records 与 max bytes 分别/共同限制。
- [ ] zero/discard records 消耗 record quota，不消耗 payload-byte quota。
- [ ] 合法 cap 下始终进步，不无限返回空。
- [ ] snapshot vector 与后续写入隔离。
- [ ] partial logical last block 的越界 bytes 确定为零。
- [ ] cancellation/分配失败不清 Dirty。

### 红灯测试：view installation

- [ ] 初始 `{generation, pointer}` 与 create args 一致；`backing_generation()` race-free。
- [ ] 非空完整 Fake G+1 安装成功；pointer 与 generation 对所有 reader 始终成对可见。
- [ ] null store、相同 generation、较小 generation、`UINT64_MAX` 回绕、closed Store 安装均原子失败并保留旧 pair。
- [ ] 成功安装增加 `backing_view_installs_total`；generation 顺序错误增加 `backing_generation_conflicts`。
- [ ] 安装 G+1 后，仍持有 G `shared_ptr` 的 in-flight 调用可安全结束；最后引用释放后 G 才析构。

### 红灯测试：ack 与 resurrection

- [ ] ack-before-install 返回 `BackingGenerationConflict`，Dirty/state/version/kind/bytes 不变。
- [ ] `persisted_generation <= dirty_base_generation` 返回 `BackingGenerationConflict`。
- [ ] `persisted_generation > backing_generation()` 返回 `BackingGenerationConflict`。
- [ ] install successor 后 exact version + exact kind + exact generation：ResidentDirty→ResidentClean，ZeroDirty→ZeroClean，`clean_generation` 精确等于 persisted generation。
- [ ] stale version 或 kind 返回 `VersionChanged`，state/version 不变，`snapshot_version_conflicts` 增加。
- [ ] ack clean/nonexistent block 的定义有测试并符合 SPEC 结果类型。
- [ ] canonical Data 流程：snapshot G0 → 构造包含记录的完整 Fake G1 → install G1 → exact ack → force eviction → reload；读取必须等于 snapshot，不得等于 G0。
- [ ] canonical Zero/Discard 流程执行相同 install/ack/evict/reload 证明，读取始终为零且不触发旧 view resurrection。
- [ ] snapshot 后并发新写入：即使旧 snapshot 已进入 installed G1，旧 ack 也只能 `VersionChanged`；新 Dirty 覆盖 G1 并等待下一 generation。

### 实现

- [ ] scan sparse leaves，不 materialize implicit leaves，不长持 leaf publication/global lock。
- [ ] ResidentDirty copy 时锁定 mutation/pin，一致捕获 version、bytes 与 `dirty_base_generation`。
- [ ] cursor、configured cap、per-call cap 按 Plan 1.6 精确实现。
- [ ] `install_backing_view` 用一个同步边界 coherent publish pointer/generation；不执行外部 I/O、不获取 block locks。
- [ ] exact ack 在同一 block protocol 内比较 dirty state、version、kind、dirty base、persisted generation 和 current installed generation。
- [ ] 成功 ack 同时发布 Clean state 与 `clean_generation`；任何失败绝不改变 Dirty 或淘汰资格。
- [ ] `VersionChanged` 与 `BackingGenerationConflict` 使用不同稳定错误与 counter，禁止混淆或静默成功。
- [ ] 任何 snapshot/install/ack failure 保持旧 view 或 Dirty state，并明确不声称 durability。

### 验证与验收

- [ ] `unit.m2.backing_generation` 与 `unit.m2.snapshot` 通过。
- [ ] resurrection canonical Data/Zero 两条测试在 Release 各连续 100 次通过。
- [ ] model 前置 snapshot/generation cases 通过。
- [ ] 回读 SPEC 第 6.7、11、12、17、29 节和 Plan Task 10/1.3/1.6，逐句核对 install-before-ack。
- [ ] 确认未实现 file write/checkpoint/journal，Fake successor 是完整 view 而非 patch。
- [ ] 建议提交：`feat(storage): add generation-safe dirty handoff`。

---

## Task 11 — Clean-only CLOCK/second-chance eviction

**目标：** soft limit 后后台释放 `ResidentClean`，绝不触碰 dirty/pinned/transient block。

**文件：** `eviction_manager.*`、core/internal、`dynamic_block_store_eviction_tests.cpp`、CMake。

### 红灯测试

- [ ] reference bit 第一次扫描清除、第二次才成为 victim。
- [ ] `ResidentClean` 且 pins=0 才可选。
- [ ] `ResidentClean.clean_generation <= backing_generation()` 才可选；反例 fail closed，不 decommit。
- [ ] ResidentDirty、ZeroClean、ZeroDirty、Loading、Committing、Evicting、pinned 全部 skip。
- [ ] victim transition 与 owner epoch 唯一。
- [ ] 成功 decommit 后 `ResidentClean -> Unresident`，accounting 精确。
- [ ] decommit failure 恢复 ResidentClean 并 notify waiter。
- [ ] cursor 跨 pass wrap，远端 leaf 不饥饿。
- [ ] 低于/equal soft 时不启动无用 eviction。
- [ ] 超 soft 后 coalesced wake，降到 low watermark。
- [ ] 无 victim 时 bounded no-progress 返回，不 busy spin。
- [ ] close 唤醒并 join worker。

### 实现

- [ ] CLOCK 只扫描已有 sparse leaves。
- [ ] cursor 为 manager 持久状态。
- [ ] 选 victim 在 stripe lock 下，decommit 时释放 metadata lock。
- [ ] decommit 后复锁验证 owner/epoch 再 publish。
- [ ] victim 选择和 publish 都保留 clean-generation invariant；不得靠 stats 判断 reload 是否安全。
- [ ] 一个 background worker，仅 factory 完成后启动。
- [ ] CV predicate 包含 pressure/close，不使用 polling sleep。
- [ ] worker 停止先于 arena release。

### 验证与验收

- [ ] `unit.m2.eviction` 通过。
- [ ] dirty victim attempts 为零；skip counters 合理。
- [ ] 回读 Plan Task 11、1.3、1.4、state table；确认 eviction 不做持久化，并能证明每个 victim 可从当前完整 view reload。
- [ ] 建议提交：`feat(storage): add clean block eviction`。

---

## Task 12 — Hard limit、commit budget 与全失败回滚

**目标：** 多线程 commit 也不能突破 hard limit；不能取得空间时 read 可 uncached，mutation 必须安全失败。

**文件：** core/read/write/ranges/eviction、`dynamic_block_store_limit_tests.cpp`。

### 红灯测试

- [ ] tiny limits 下 low/soft/hard exact boundary。
- [ ] 两个/多个并发 committer 的 reserved budget 总和不越 hard。
- [ ] commit 前预算预留，commit 成功后转换 accounting。
- [ ] commit failure 释放 reservation，不增加 committed。
- [ ] soft crossing 唤醒 worker。
- [ ] hard crossing 触发 bounded synchronous eviction。
- [ ] clean victim 足够时 mutation 成功并降压。
- [ ] 无 victim时 Data read-through 成功且不 cache。
- [ ] 无 victim时 write/partial ZERO/partial UNMAP 在修改前 resource-exhausted。
- [ ] 无 victim且 committed 已到 hard limit 时，对现有 Resident block 的 overwrite 因不需新 commit而成功。
- [ ] decommit 成功只减一次；失败不减。
- [ ] background 与 synchronous 同时 eviction 不 double-decommit。
- [ ] debug reconciliation：budget + committed + state 一致。

### 实现

- [ ] 设计独立的 `reserved_commit_bytes` 与 `committed_bytes`。
- [ ] budget CAS/lock 逻辑支持并发且有 overflow check。
- [ ] synchronous pressure pass 有 deadline/cancel/no-progress bound。
- [ ] read-through 使用 private loaded buffer 并返回 `Unresident`。
- [ ] 需要新 commit 的 mutation 不用 read-through 绕过 hard limit；已 Resident 的原位 mutation 不错误申请新 budget。
- [ ] 所有 early return 用 RAII 释放 reservation/pin/transition owner。
- [ ] stats 不用来决定是否能 commit，正确性使用受保护 state。

### 验证与验收

- [ ] `unit.m2.limits|eviction|read|write|ranges` 全通过。
- [ ] 失败注入矩阵后 invariant checker 全通过。
- [ ] 回读 Plan Task 12；逐条审查每个 reserve/commit/decommit return path。
- [ ] 建议提交：`feat(storage): enforce bounded commit pressure`。

---

## Task 13 — 确定性 100,000 操作 reference model

**目标：** 用与产品实现无共享逻辑的模型验证 byte content、dirty meaning 和状态结果。

**文件：** `tests/support/reference_block_model.h`、`dynamic_block_store_model_tests.cpp`、CMake。

### 模型建设

- [ ] 模型使用普通 byte vectors/maps，不 include production block math/state helpers。
- [ ] 模型定义 read/write/ZERO/UNMAP/snapshot/view-install/ack/evict-reload 的独立语义。
- [ ] 模型 logical disk 足够小以便频繁全量比较，但含稀疏远端 block。
- [ ] canonical fixed seed 写死且失败时打印。
- [ ] 支持环境/参数 override seed 仅用于诊断。
- [ ] operation log 含 index、type、range、snapshot version/kind/base generation、installed generation、ack generation。

### 100,000 操作内容

- [ ] aligned reads/writes，含块内、跨块、末 sector。
- [ ] full/partial ZERO。
- [ ] full/partial UNMAP。
- [ ] dirty scans with cursor/batch limits and base generations。
- [ ] successor view install、ack-before-install conflict、exact/stale generation acknowledgements。
- [ ] force pressure/clean eviction/read-through。
- [ ] exact ack 后强制 clean eviction/reload，并将 Store 与模型比较。
- [ ] repeated same block 和 sparse distant blocks。
- [ ] 至少每 N 次做 sampled/full byte comparison。
- [ ] 至少每 N 次做 dirty records semantic comparison。
- [ ] 至少每 N 次做 invariants/accounting check。
- [ ] 最终输出恰好 `100000` completed operations。

### 验证与验收

- [ ] 先故意注入一个测试模型差异，确认 harness 会红，再移除注入。
- [ ] `unit.m2.model` Release 通过。
- [ ] 同 seed 连续两次结果一致。
- [ ] 记录 seed、耗时、操作数和证据。
- [ ] M2-V1 + M2-V2 当前层通过，形成 M2-A 核心证据草案。
- [ ] 回读 Plan Task 13；确认模型没有复制 production bug。
- [ ] 建议提交：`test(storage): add deterministic block store model`。

---

## Task 14 — 单飞加载与同 block 并发证明

**目标：** 用确定性调度测试而非“跑起来没崩”证明 transition/waiter 正确。

**文件：** `dynamic_block_store_concurrency_tests.cpp`，仅按红灯结果修产品文件。

### 确定性并发用例

- [ ] barriers/latches 控制 2 readers 同时进入同块 Loading。
- [ ] 扩展到 8/32 readers，backing count 仍为 1。
- [ ] 所有成功 reader bytes 相同。
- [ ] 一个 waiter cancel，其他 waiter/owner 不受污染。
- [ ] owner fail，所有 waiters 被唤醒且 state 可重试。
- [ ] retry 成功且 epoch 不复用旧结果。
- [ ] generation G load 被 gate 阻塞时安装 G+1；旧结果返回后不得 publish，必须从 G+1 重试。
- [ ] 上述竞态只允许每个 generation 一个 owner load，所有 waiter 最终看到 G+1，旧 backing 生命周期安全。
- [ ] 同块 read vs full write 的结果可序列化。
- [ ] read vs partial write 不 torn。
- [ ] write vs write 最终为一个完整版本。
- [ ] full ZERO/UNMAP vs read 不 resurrection。
- [ ] close vs load 在 deadline 内结束。
- [ ] 同 stripe 不同 blocks 正确；不同 stripes 可并行。

### 验证与验收

- [ ] 不使用任意 `Sleep` 决定竞态顺序。
- [ ] 需要 hook 时保持 private/test-only，并记录为何必要。
- [ ] focused concurrency subset 连续 100 次通过。
- [ ] M2-V1/V2 回归通过。
- [ ] 回读 Plan Task 14、1.3 和 1.4；检查所有 wait 都是 predicate loop + notify，并核对 generation retry 的有界进度。
- [ ] 建议提交：`test(storage): prove dynamic store transition concurrency`。

---

## Task 15 — Snapshot/mutation/eviction/cancel/close 竞态

**目标：** 证明版本、pins、owner epoch 和 shutdown 在最危险交叉点仍正确。

**文件：** concurrency tests；只按失败测试修改产品代码。

### 确定性竞态用例

- [ ] snapshot copy vs write 只能得到 `(old version, old bytes)` 或 `(new,new)`。
- [ ] ack-before-install 返回 `BackingGenerationConflict`，block 保持 Dirty。
- [ ] install old snapshot 的 successor 后，若已发生后续 mutation，old persisted ack 返回 `VersionChanged`，block 保持 Dirty。
- [ ] exact install/ack 与并发新写只允许一个明确线性化顺序；不能把新版本误标 Clean。
- [ ] exact ack 后与 eviction/reload 竞态，任何 reader 都不能看到 predecessor view 的旧 bytes。
- [ ] eviction select vs reader pin，不能在 copy 中 decommit。
- [ ] eviction select vs writer transition，无 double ownership。
- [ ] background vs synchronous eviction，无 double-decommit/accounting underflow。
- [ ] waiter 在 Loading 上 cancel/deadline。
- [ ] waiter 在 mutation/pin 上 cancel/deadline。
- [ ] pressure wait cancel/deadline。
- [ ] shutdown 唤醒所有上述 waiters。
- [ ] blocked backing 观察 cooperative cancel，owner 可退出。
- [ ] worker join 和 arena release 恰好一次。

### 验证与验收

- [ ] focused tests 使用外层 timeout，失败可打印阶段。
- [ ] `unit.m2.concurrency` until-fail 100 次通过。
- [ ] 所有 earlier unit/model tests 通过。
- [ ] 回读 Plan Task 15；审查 view install、generation-bound ack、pin、owner epoch 四类线性化点。
- [ ] 建议提交：`test(storage): cover snapshot eviction and shutdown races`。

---

## Task 16 — 长并发压力、不变量与资源核算

**目标：** 在多角色长期操作中证明无数据漂移、无挂死、无资源累积，形成 M2-B。

**文件：** concurrency/stress tests、CMake，按缺陷最小修改产品文件。

### 压力场景

- [ ] 固定 seed 和固定 operation/time bound。
- [ ] readers、writers、ZERO、UNMAP、snapshot、immutable successor installer、generation-bound ack、pressure 并发角色齐全。
- [ ] shadow model 受独立同步保护，不共享 production state。
- [ ] 周期性 quiescent sampled/full byte check。
- [ ] 周期性 invariant check：state、prior、owner、pins、version、dirty base/clean/current generation。
- [ ] accounting check：reserved、committed、dirty、arena fake/real counters。
- [ ] repeated create/destroy 前后 handle/thread/private bytes/reservation/backing object lifetime baseline。
- [ ] 每个 wait/test 有 timeout；failure 打印 seed/counters/last operations。
- [ ] 无 crash-only pass：必须有 correctness comparison。

### 验证与验收

- [ ] M2-V3 完整跑两次，均一次通过，不能失败后靠重跑选成功。
- [ ] 零 store-owned thread/handle/reservation 泄漏。
- [ ] 无 invariant failure、hang、counter underflow/overflow。
- [ ] M2-V1/V2 累计回归通过。
- [ ] 在 acceptance 文档标记 M2-B 及证据；未齐全不得标记。
- [ ] 回读 Plan Task 16 与 verification tier M2-V3。
- [ ] 建议提交：`test(storage): add concurrent stress and resource audits`。

---

## Task 17 — 将 Store 注入 M1 Proxy 和本地 SHM

**目标：** 证明 M2 是标准 `IBlockDevice`，M1 proxy 不需要理解 VirtualAlloc、cache 或 metadata。

**文件：** `src/m2/m2_provider.*`、`m2_provider_tests.cpp`、`m2_shm_proxy_tests.cpp`、CMake。

### 红灯测试

- [ ] fake store creation fail 时 provider 不启动 transport。
- [ ] transport start fail 时 store/资源正确销毁。
- [ ] Ready 只在 store + proxy transport 都成功后发布。
- [ ] stop 顺序：拒绝新工作 → stop transport/session → quiesce → destroy store。
- [ ] double stop/idempotent cleanup 行为明确。
- [ ] local SHM cross-block read/write byte-correct。
- [ ] local SHM ZERO/UNMAP 返回正确并读回零。
- [ ] cancellation/deadline 穿过 protocol 到 store。
- [ ] malformed request 在 storage mutation 前被 M1 拒绝。
- [ ] disconnect/restart 不泄漏 session/store。

### 实现

- [ ] `M2Provider` 仅组合 store、M1 proxy/session、transport。
- [ ] store 通过 `IBlockDevice` 注入，无 downcast。
- [ ] 一个 owner 管理生命周期，不使用 detached thread/global singleton。
- [ ] `src/proxy` 不 include `src/storage` concrete headers。
- [ ] `src/storage` 不 include driver/AIM headers。
- [ ] local integration stats 只由测试 owner 读取作佐证，不改变 protocol。

### 验证与验收

- [ ] `unit.m2.provider` 与 `integration.m2.shm` 通过。
- [ ] 所有 M1 protocol/transport/local tests 原样通过。
- [ ] M2-V4 通过。
- [ ] 用 include/dependency 检查证明架构方向。
- [ ] 回读 Plan Task 17 和 Architecture；确认无“为了方便”泄漏 internals。
- [ ] 建议提交：`feat(m2): connect dynamic store to proxy provider`。

---

## Task 18 — 真实 VirtualAlloc 的 64 GiB/1 TiB 集成验证

**目标：** 用 OS 指标证明“reserve 大、实际 commit 小、访问后增加、释放后下降”。

**文件：** `tests/integration/m2_virtual_memory_tests.cpp`、CMake。

### 测试准备

- [ ] 确认测试进程为 x64 Release。
- [ ] 选定并记录 OS 指标 API：process commit/private bytes/working set 与 store counters。
- [ ] 记录 allocator noise tolerance，不能把 arena stats 当作唯一 OS 证据。
- [ ] 测量 empty test-process baseline 多次，选择有说明的统计方式。

### 64 GiB 流程

- [ ] construct empty clean-zero 64 GiB store。
- [ ] Ready 时 committed memory `<=128 MiB`，记录固定 metadata delta。
- [ ] sparse zero reads 不增加 data commits。
- [ ] 写入 1 GiB unique blocks，新增 `<=1.10 GiB + fixed metadata`。
- [ ] 校验写入 bytes。
- [ ] 通过 clean eviction/适用的 unmap 释放 1 GiB。
- [ ] 10 秒内回收至少 90%，同时看 store 与 OS samples。
- [ ] destroy 后 reservation/thread/handle 回 baseline tolerance。

### 1 TiB 流程

- [ ] construct empty 1 TiB store 成功且 initial entries/leaves 非线性分配。
- [ ] 访问低、中、高 sparse block，地址和内容正确。
- [ ] 只 commit 被访问/写入 block。
- [ ] destroy 释放整个 reservation。

### 验证与验收

- [ ] `integration.m2.virtual_memory` 通过。
- [ ] M2-V2（含 100,000 model）通过。
- [ ] 证据包含每个采样值、时间点、阈值和 pass/fail。
- [ ] 回读 Plan Task 18 和 performance limits；不能只说“代码用了 MEM_RESERVE 所以动态”。
- [ ] 建议提交：`test(m2): validate large virtual memory behavior`。

---

## Task 19 — M2 CLI、lifecycle、报告 schema 与 performance probe

**目标：** 在触碰真实磁盘前，用 fake 完整证明生命周期、回滚、报告和性能计算。

**文件：** Plan Task 19 列出的 `src/m2/*`、unit tests、CMake。

### JSON schema 红灯测试

- [ ] schema version 与 run ID 必填且稳定。
- [ ] build/toolchain、M1 baseline、machine/OS 必填。
- [ ] driver/service before/after 必填。
- [ ] geometry/options/initial state/initial and current backing generation 必填。
- [ ] 每 phase start/end/status/error/elapsed 必填。
- [ ] checksum/bytes written/read/deleted 必填。
- [ ] store stats、view install/generation conflict/stale-load counters 与 OS memory samples 必填。
- [ ] baseline/dynamic perf raw values、ratio、threshold 必填。
- [ ] exact device identity、cleanup actions、final inventory diff 必填。
- [ ] failure list 与唯一 verdict 必填。
- [ ] missing field、overflow/narrow integer、unknown enum 使验证失败。
- [ ] report 用 temp + atomic rename 发布；路径 run-unique，不覆盖旧报告。

### Lifecycle 红灯测试

- [ ] baseline capture fail：无 mutation。
- [ ] provider construct/start fail：按已拥有资源 rollback。
- [ ] device create fail/wait timeout：stop provider、恢复 service。
- [ ] setup/write fail、delete/retrim fail、perf fail：仍执行全部适用 cleanup。
- [ ] mount removal fail/device removal fail/service restore fail：最终 NO-GO。
- [ ] cleanup action 每项记录 attempted/result/error。
- [ ] phase deadline/cancellation 生效，无无限等待。
- [ ] fake success order 与 Plan Task 19 完全一致。

### Performance probe 红灯测试

- [ ] warmup 数据不进入计时。
- [ ] sequential region 恰好 32 MiB且对齐。
- [ ] random 恰好 10,000 次 4 KiB QD1，固定 seed/对齐。
- [ ] short I/O/error 使 probe 失败。
- [ ] fixed 和 dynamic 方法完全相同。
- [ ] seq ratio 70% 边界；random 60% 边界。
- [ ] division by zero/无 baseline 明确失败。

### 实现与 CLI

- [ ] 定义可注入 `IM2VolumeWorkload` phase API。
- [ ] lifecycle 每次 mutation 后立即注册 exact rollback guard。
- [ ] real M2 lifecycle 显式使用 generation `0` 或记录恢复 generation，并明确报告“未安装 durable successor backing”。
- [ ] 实现 report writer/reader semantic validator。
- [ ] 实现 raw QD1 probe，不用 filesystem cache 结果冒充 block device 结果；记录 flags。
- [ ] `--help`、`--print-schema`、`--local-fake`、real acceptance mode。
- [ ] real mode 无隐式默认目标；要求 evidence dir、expected driver/device、64 GiB、safety acknowledgement。
- [ ] real mode 要求显式 store options，或显式选择并记录 SPEC 允许的 test policy：soft=物理 RAM 25%、hard=50%、low=soft 的 80%，全部向 64 KiB 安全对齐。
- [ ] unknown options/不安全组合 fail closed。

### 验证与验收

- [ ] `unit.m2.lifecycle|report|performance` 通过。
- [ ] `m2 --help` 和 `--print-schema` 成功。
- [ ] `m2 --local-fake --evidence-dir <unique-temp>` 成功，JSON validator 通过。
- [ ] 所有 fake failure points 产生 NO-GO 且 baseline restored。
- [ ] 回读 Plan Task 19；确认报告不会因 cleanup fail 仍写 GO。
- [ ] 建议提交：`feat(m2): add acceptance lifecycle and evidence model`。

---

## Task 20 — fail-closed NTFS workload 与 verification orchestrator

**目标：** 在 provider 存活期间安全创建唯一 64 GiB 磁盘、完成真实文件工作负载、再按精确身份清理。

**文件：** `tests/e2e/Invoke-M2NtfsWorkload.ps1`、`run-m2-dynamic-disk.ps1`、preflight test、`tools/Verify-M2Json.ps1`、`Invoke-M2Verification.ps1`。

### Mock/preflight 红灯测试

- [ ] 非管理员拒绝 real mode。
- [ ] 缺 binary/driver/service/PowerShell cmdlet 拒绝。
- [ ] baseline 已有一个 candidate 或 create 后出现 0/2 个 candidates 拒绝。
- [ ] candidate 是 Disk 0 拒绝。
- [ ] boot/system/pagefile/clustered disk 拒绝。
- [ ] size 非精确 64 GiB 拒绝。
- [ ] bus/vendor/product/device identity 不匹配拒绝。
- [ ] 发现 `aimapi.dll`、`aim_ll`、AIM GUI 或 RamDyn 被加载/作为依赖时拒绝。
- [ ] candidate 已有 partition/volume 拒绝。
- [ ] mount directory 非空/冲突拒绝。
- [ ] service baseline 不可读取/恢复拒绝。
- [ ] evidence dir 不可写或会覆盖旧 run 拒绝。
- [ ] `-WhatIf` 不调用 Initialize-Disk/Format/RemoveDevice/Set-Service。

### Orchestrator 实现

- [ ] before inventory：physical disks、volumes、mounts、M2 devices、service state/start type、resources。
- [ ] 启动 C++ provider/lifecycle process 并等结构化 Ready，期间进程保持存活。
- [ ] 记录模块/依赖检查，真实 device path 只使用本项目二进制和 accepted `phdskmnt.sys`。
- [ ] new disk = before/after set difference + exact identity；必须恰好一个。
- [ ] 通过全部 safety predicate 后才允许 initialize。
- [ ] GPT → one partition → NTFS quick format → run-unique empty mount dir。
- [ ] 写 deterministic files 总计至少 4 GiB，flush/close/read/hash verify。
- [ ] 修改、rename 文件与目录，再 read/hash verify。
- [ ] 确认 NTFS DeleteNotify enabled。
- [ ] 删除 deterministic subset、flush、执行 ReTrim。
- [ ] 有限等待 store UNMAP count/bytes 增加。
- [ ] cleanup：close handles→remove mount/letter→exact device remove→provider stop→service exact restore→inventory compare。
- [ ] 任一失败仍执行已注册 cleanup；永不按宽泛名称批量删除设备。

### JSON/runner 实现

- [ ] validator 检查 schema + semantic gates，不只检查 JSON 可解析。
- [ ] 缺少 4 GiB、hash、UNMAP、memory、perf、cycle、cleanup 任一证据即 fail。
- [ ] runner 为每次调用建 timestamp+GUID evidence dir。
- [ ] stdout/stderr/exit code/command line/environment 摘要全部保存。
- [ ] Tier M2-V5/V6/V7 映射固定且输出唯一 terminal verdict。
- [ ] 不允许 `ContinueOnError` 把 required phase 变成 skip/pass。

### 验证与验收

- [ ] 全部 mocked preflight tests 通过。
- [ ] `Invoke-M2Verification.ps1 -Tier M2-V5 -WhatIf` 通过。
- [ ] before/after inventory 证明 WhatIf 零 mutation。
- [ ] 人工审查每个 destructive cmdlet 前都有 exact guard。
- [ ] 回读 Plan Task 20 和 Stop conditions；确认 cleanup target 不是模糊查询结果。
- [ ] 建议提交：`test(m2): add safe NTFS acceptance harness`。

---

## Task 21 — 执行 M2-A、M2-B 与 M2-C 功能门

**目标：** 在同一候选 Release build 上取得核心、并发资源和真实 NTFS 的可复现证据。

**允许修改：** 验收文档；若发现 defect，必须先补测试再做最小产品修复。不得现场编辑证据或降门槛。

### M2-A / V1-V2

- [ ] clean Release configure/build。
- [ ] 跑 M2-V1，全部 core unit tests 被实际选中且通过。
- [ ] 核查 CTest 输出没有 `No tests were found` 或 required skip。
- [ ] 跑 M2-V2，100,000 operations 完整通过。
- [ ] 64 GiB/1 TiB VM integration 通过。
- [ ] 记录 test list、命令、exit code、日志、seed、memory samples。
- [ ] 只有上述齐全才在 acceptance 标记 M2-A PASS。

### M2-B / V3-V4

- [ ] M2-V3 连续完整跑两次；任何一次失败都先调查，不能择优。
- [ ] concurrency、cancel/deadline、pressure、shutdown、invariant、resource 均被选中。
- [ ] M2-V4 local SHM 通过。
- [ ] 完整 M1 local regression 通过。
- [ ] 记录 handle/thread/reservation baseline before/after。
- [ ] 只有上述齐全才标记 M2-B PASS。

### M2-C / V5 real disk

- [ ] 再次运行 real preflight/WhatIf，并人工确认机器当前磁盘基线。
- [ ] 使用 exact 64 GiB device 与唯一 evidence dir 启动 M2-V5。
- [ ] provider 在 GPT/NTFS/文件/trim 全流程中持续存活。
- [ ] verified file data >=4 GiB。
- [ ] 初始写入、读回、修改、rename 的 hashes/bytes 全一致。
- [ ] DeleteNotify enabled，ReTrim 成功，store 观测到 UNMAP。
- [ ] mount/device/provider/service 全部 exact cleanup。
- [ ] after inventory 与 before baseline 一致。
- [ ] `Verify-M2Json.ps1` 通过。
- [ ] 只有上述齐全才标记“M2-C functional/cleanup PASS”；完整 M2-C 仍等待 Task 22 的资源与性能门，不得提前标记 M2-C PASS。

### Defect 回路与验收

- [ ] corruption/hang/dirty loss/resurrection/wrong device/cleanup fail 立即停止并保留 run。
- [ ] 在最小本地层添加红灯测试后修复。
- [ ] 从受影响 gate 开头重跑，产生新目录；失败目录继续保留。
- [ ] M2-A、M2-B 和 M2-C functional/cleanup 都有链接到 canonical passing evidence。
- [ ] 回读 Plan Task 21 与第 3、7 节；确认没有隐藏 skip/retry。
- [ ] 建议提交：`test(m2): complete core concurrency and ntfs gates`。

---

## Task 22 — 性能/故障、20 轮 soak 与最终 M2 verdict

**目标：** 用量化阈值和重复生命周期证明 M2 可交给 M3，而不只是“能挂载一次”。

**文件：** 最小 defect fixes/tests；最终 `docs/validation/m2-dynamic-block-store-acceptance.md`。

### M2-V6 memory/performance/fault

- [ ] empty 64 GiB Ready provider committed `<=128 MiB`。
- [ ] 1 GiB unique writes 新增 `<=1.10 GiB + measured fixed metadata`。
- [ ] 释放/unmap 1 GiB 后 10 秒内回收 `>=90%`。
- [ ] fixed M0/M1 provider 与 M2 使用同机、同 build policy、同 QD1 probe。
- [ ] sequential：32 MiB aligned，warmup excluded，M2 >= baseline 70%。
- [ ] random：10,000 × 4 KiB aligned QD1 fixed seed，M2 >= baseline 60%。
- [ ] short I/O/error/zero baseline 不是合法样本。
- [ ] 证明 request path 无 unbounded per-request allocation。
- [ ] 每个 lifecycle failure point 注入后 exact cleanup 与 service restore。
- [ ] JSON validator 对 threshold 和 cleanup 全部通过。

### M2-V7 20-cycle Release soak

- [ ] 用同一 accepted Release build 跑 20 个完整 cycle。
- [ ] 每轮：create→read/write verified data→ZERO/UNMAP→delete→remove exact device→stop provider。
- [ ] 每轮可用 64 MiB deterministic region；不得用它替代 V5 的 4 GiB gate。
- [ ] 每轮记录 checksum、stats、memory、handles、threads、device ID、cleanup。
- [ ] 每轮后 disk/volume/mount/service/device baseline 一致。
- [ ] 20 轮零 leaked provider device/store thread/arena reservation/mount/service mutation。
- [ ] 任一轮失败则整项 V7 FAIL；修复后从第 1 轮重新跑新 evidence run。

### 最终回归和 acceptance 文档

- [ ] final M1 full regression 通过。
- [ ] final M2-V1..V4 通过或与同一 commit 的 fresh canonical run 关联。
- [ ] 所有 canonical JSON 再跑 `Verify-M2Json.ps1`。
- [ ] Task 21 的 functional/cleanup 与本任务 memory/performance 全通过后，才标记 M2-C PASS。
- [ ] acceptance 每一项包含：command、exit code、evidence path、observed、threshold、result。
- [ ] 列出所有失败 attempts 与修复 commits，不删除历史。
- [ ] 写明稳定 M3 handoff：IBlockDevice、immutable IBackingStore、BackingView、snapshot/cursor/base generation、install-before-ack、exact version/kind/generation ack、stats、limits。
- [ ] 明确 M2 未实现 durable persistence/journal/checkpoint/file backing。
- [ ] 只有全部 gate 通过才写 `M2 GO`；否则必须写 `M2 NO-GO` 和 root cause。
- [ ] performance-only miss 仍为 `M2 NO-GO`。
- [ ] corruption、dirty loss、resurrection、cleanup ambiguity 不可 waiver。
- [ ] `git diff --check`、Release build、最终 worktree/status 审查完成。
- [ ] 回读完整 Plan Definition of done，逐句打勾并写最终偏航检查。
- [ ] 建议提交：`docs(m2): issue dynamic block store acceptance verdict`。

---

## D. 最终不可替代的 GO 清单

以下任何一项未打勾，都不能把总状态或验收报告写成 `M2 GO`：

- [ ] M1 在最终 M2 commit 上仍为 fresh PASS。
- [ ] 64 GiB 和 1 TiB reserve 不导致同比例 commit/per-block metadata。
- [ ] read/write/ZERO/UNMAP byte correctness 通过 model、proxy、NTFS 三层证明。
- [ ] 第一次访问才 load；same-block miss single-flight；AllZero 不 commit data page。
- [ ] ResidentClean 可回收；dirty/zero override 永不 eviction/丢失/backing resurrection。
- [ ] soft/hard limits、budget、read-through、resource exhaustion 正确。
- [ ] cancel/deadline/fault/shutdown 无 hang、无半状态、无泄漏。
- [ ] dirty snapshot/version/exact ack 在并发 mutation 下正确。
- [ ] immutable successor view 原子安装；ack-before-install 失败；exact ack 后 eviction/reload 不复活 predecessor 数据。
- [ ] deterministic 100,000 model PASS。
- [ ] M2-A、M2-B、M2-C 均 PASS。
- [ ] real NTFS verified data >=4 GiB，DeleteNotify/ReTrim/UNMAP 证据完整。
- [ ] empty/1 GiB growth/90% release 三项内存门槛 PASS。
- [ ] sequential 70% 与 random 60% 性能门槛 PASS。
- [ ] 20-cycle Release soak PASS。
- [ ] exact device cleanup、service restore、disk/volume/mount/resource baseline PASS。
- [ ] acceptance 报告和 JSON validator 均给出 `M2 GO`。

---

## E. 执行记录

Agent 从 Task 00 开始在下方追加，不删除旧记录。每次会话恢复时，先用记录、Git 和证据三方交叉确认状态；对话中的“已完成”不能单独作为依据。

<!-- Append immutable task execution records below this line. -->

Task: 00 — M1 基线与 M2 构建壳（开始记录）
Status: DOING
Start commit / end commit: `724a00e2cf22e65de0eab18f0a6423f8194156be` / pending Task 00 commit
Files changed: limited to the Task 00 allowlist
Red test and observed failure: `cmake --build --preset release --target myrd-m2-e2e` exited 1 with `ninja: error: unknown target 'myrd-m2-e2e'`
Focused verification command / exit code: pending green verification
Cumulative verification command / exit code: pre-change Release CTest 26/26 and M1-V2 PASS
Evidence paths: console transcript; canonical facts will be recorded in `docs/validation/m2-dynamic-block-store-acceptance.md`
Plan drift audit: build shell only; no M2 product behavior and no Task 01 work
Open risks: post-change verification and final self-review remain
Executor / timestamp: Codex / `2026-08-03T11:10:32+08:00`

Task: 00 — M1 基线与 M2 构建壳（完成记录）
Status: DONE
Start commit / end commit: `724a00e2cf22e65de0eab18f0a6423f8194156be` / Task 00 commit with the requested message
Files changed: `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `src/m2/main.cpp`, `docs/validation/m2-dynamic-block-store-acceptance.md`, and this Task 00 status/checklist/record
Red test and observed failure: exit 1, `ninja: error: unknown target 'myrd-m2-e2e'`; green target build exit 0, 2/2 steps
Focused verification command / exit code: `cmake --build --preset release --target myrd_storage --clean-first` exit 0 (6/6); label discovery 3/3/3/26; post-change M1-V2 exit 0 (14/14, fuzz 1/1, SHM 50/50)
Cumulative verification command / exit code: fresh post-change `cmake --preset msvc --fresh` exit 0; `cmake --build --preset release --clean-first` exit 0 (94/94); `ctest --preset release --output-on-failure` exit 0 (26/26)
Evidence paths: `docs/validation/m2-dynamic-block-store-acceptance.md`; pinned reference `out/reference/aim` is ignored
Plan drift audit: Task 00 only; no M1 behavior source, no duplicate storage target, no Task 01/M2 behavior, no CodeGraph initialization
Open risks: M2-V1..V4 behavior is intentionally unimplemented and must not be inferred from reserved prerequisite labels
Executor / timestamp: Codex / `2026-08-03T11:14:58+08:00`

Task: 00 — 规格审查修复记录
Status: DONE
Start commit / end commit: `6a96f2e` / review-fix commit with the requested message
Files changed: `tests/CMakeLists.txt`, `docs/validation/m2-dynamic-block-store-acceptance.md`, and this appended record
Red test and observed failure: exact-name CTest discovery returned `Total Tests: 0`, then the wrapper failed with `RED: required structural storage build test is absent`
Focused verification command / exit code: exact-name discovery found 1 test; `ctest --preset release --output-on-failure -R '^structural\.m2\.storage_independent_build$'` exit 0 (1/1)
Cumulative verification command / exit code: Release build exit 0; full CTest 27/27; M2-V1/V2/V3 4/4 each; M2-V4 27/27; fresh M1-V2 Debug PASS (14/14, fuzz 1/1, SHM 50/50)
Evidence paths: `docs/validation/m2-dynamic-block-store-acceptance.md`
Plan drift audit: closes the Task 00 continuous storage-boundary gate only; no Task 01 or product behavior
Open risks: repeated specification review remains
Executor / timestamp: Codex / `2026-08-03T11:29:32+08:00`

Task: 00 — 完整依赖闭包审查修复记录
Status: DONE
Start commit / end commit: `5fc8977` / closure-fix commit with the requested message
Files changed: `tests/CMakeLists.txt`, `tests/support/verify_storage_boundary.ps1`, `docs/validation/m2-dynamic-block-store-acceptance.md`, and this appended record
Red test and observed failure: temporarily injected `add_dependencies(myrd_storage myrd_driver)`; the prior structural CTest still passed, and the wrapper failed with `RED: storage boundary gate accepted ...`
Focused verification command / exit code: isolated probe passed normally; with the same temporary fault injected it rejected the resolved build after observing `myrd_driver`, CTest exit 8; fault injection removed afterward
Cumulative verification command / exit code: fresh configure exit 0; Release build exit 0; full CTest 27/27; M2-V1/V2/V3 4/4 each; M2-V4 27/27; fresh M1-V2 Debug PASS (14/14, fuzz 1/1, SHM 50/50)
Evidence paths: `docs/validation/m2-dynamic-block-store-acceptance.md`; isolated build under ignored `out/build/storage-boundary-probe`
Plan drift audit: strengthens only the Task 00 storage target boundary; no product behavior, Task 01 code, or persistent fault injection
Open risks: repeated specification review remains
Executor / timestamp: Codex / `2026-08-03T11:39:25+08:00`

Task: 00 — 文件白名单与工作树证据审查修复记录
Status: DONE
Start commit / end commit: `a116f45` / allowlist-fix commit with the requested message
Files changed: only `tests/CMakeLists.txt`, `docs/validation/m2-dynamic-block-store-acceptance.md`, and this appended record; the out-of-allowlist source script was deleted
Red test and observed failure: specification review found `tests/support/verify_storage_boundary.ps1` outside the Task 00 exact file list and found no explicit `git status --short` evidence in acceptance
Focused verification command / exit code: configure generated `out/build/msvc/tests/verify_storage_boundary_Release.ps1`; exact structural CTest exit 0 (1/1)
Cumulative verification command / exit code: Release build exit 0; full CTest 27/27; M2-V1/V2/V3 4/4 each; M2-V4 27/27; fresh M1-V2 Debug PASS (14/14, fuzz 1/1, SHM 50/50)
Evidence paths: `docs/validation/m2-dynamic-block-store-acceptance.md`
Plan drift audit: final source changes return to the Task 00 allowlist; generated probe remains under ignored `out/`; no Task 01 code
Open risks: repeated specification review remains
Executor / timestamp: Codex / `2026-08-03T11:49:24+08:00`

Task: 00 — 代码质量审查修复记录
Status: DONE
Start commit / end commit: `d8f4e46` / quality-fix commit with the requested message
Files changed: only `tests/CMakeLists.txt`, `docs/validation/m2-dynamic-block-store-acceptance.md`, and this appended record
Red test and observed failure: review identified config-expression, cross-process probe-directory, and hand-maintained M2-V4 baseline risks; a temporary Debug-only `myrd_driver` generator-expression dependency exercised the guard and was removed
Focused verification command / exit code: generator-expression dependency rejected at configure exit 1; normal structural CTest 1/1; two concurrent structural CTest processes both exit 0 and leave no unique run directories
Cumulative verification command / exit code: fresh configure exit 0; Release build exit 0; full CTest 27/27; M2-V1/V2/V3 4/4 each; M2-V4 27/27; fresh M1-V2 Debug PASS (14/14, fuzz 1/1, SHM 50/50)
Evidence paths: `docs/validation/m2-dynamic-block-store-acceptance.md`; generated unique probes under ignored `out/build/storage-boundary-probes`
Plan drift audit: Task 00 build/test shell only; no Task 01 product code and no retained fault injection
Open risks: repeated code-quality review remains
Executor / timestamp: Codex / `2026-08-03T12:05:44+08:00`

Task: 00 — 独立审查关闭记录
Status: DONE
Start commit / end commit: `724a00e` / `800748f`
Files changed: no product change; this appended audit record only
Red test and observed failure: see the preceding Task 00 TDD and review-remediation records
Focused verification command / exit code: specification review at `d8f4e46` returned COMPLIANT with no findings; code-quality re-review at `800748f` returned APPROVED with no Critical, Important, or Minor findings
Cumulative verification command / exit code: fresh configure exit 0; Release build exit 0; full CTest 27/27; M2-V1/V2/V3 4/4 each; M2-V4 27/27; fresh M1-V2 Debug PASS
Evidence paths: `docs/validation/m2-dynamic-block-store-acceptance.md`; Git commits `6a96f2e`, `5fc8977`, `a116f45`, `d8f4e46`, `800748f`
Plan drift audit: Task 00 is closed within its final source-file allowlist; no Task 01 implementation is present
Open risks: none for Task 00; later M2 tiers remain intentionally unimplemented
Executor / timestamp: Codex / `2026-08-03T12:09:23+08:00`

Task: 01 — checked geometry、options、状态和 stats
Status: DONE
Start commit / end commit: `bb33c444a253aca5b8223de4af612da875bfba51` / this Task 01 commit
Files changed: `src/storage/block_constants.h`, `src/storage/block_math.h`, `src/storage/dynamic_block_types.h`, `tests/unit/block_math_tests.cpp`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, and only this Task 01 checklist/status/record in `M2_TASKLIST.md`
Red test and observed failure: after registering `unit.m2.block_math`, `cmake --build --preset release --target myrd_m2_block_math_tests` failed in Ninja compilation with code 2 and MSVC `C1083` because `src/storage/block_constants.h` did not exist; the exact focused CTest was consequently Not Run and failed because its executable was absent
Focused verification command / exit code: `ctest --preset release --output-on-failure -R '^unit\.m2\.block_math$'` exit 0 (1/1)
Cumulative verification command / exit code: `ctest --preset release --output-on-failure -L m2-v1` exit 0 (5/5); full Release CTest exit 0 (28/28); `git diff --check` exit 0
Evidence paths: `tests/unit/block_math_tests.cpp`; Task 01 public headers under `src/storage/`; console transcript for the red/green commands
Plan drift audit: Task 01 only; `IBackingStore` is forward-declared and held as `shared_ptr<const IBackingStore>`; no Task 02 interface/implementation, AIM/driver dependency, M1 behavior change, durable persistence, or hidden product defaults
Open risks: backing normalization is intentionally only an explicit validation result until Task 02 supplies `ZeroBackingStore`; concurrent coherent stats production belongs to later store implementation, while the public stats snapshot remains a plain copyable value
Executor / timestamp: Codex / `2026-08-03T12:23:52+08:00`

Task: 01 — 规格审查测试覆盖修复
Status: DONE
Start commit / end commit: `70fc9c1bd5f14b3823c21e58fc5ee217fd830b19` / this Task 01 review-fix commit
Files changed: `tests/unit/block_math_tests.cpp` and this appended `M2_TASKLIST.md` record only
Review finding and resolution: added independent assertions for sector-aligned offset with misaligned length, `soft_limit_bytes > hard_limit_bytes`, an independently misaligned low watermark, and an independently misaligned hard limit; retained the existing `soft == hard` and misaligned-soft cases
Focused verification command / exit code: Release test target build exit 0; `ctest --preset release --output-on-failure -R '^unit\.m2\.block_math$'` exit 0 (1/1)
Cumulative verification command / exit code: Release build exit 0; `ctest --preset release --output-on-failure -L m2-v1` exit 0 (5/5); full Release CTest exit 0 (28/28); `git diff --check` exit 0
Evidence paths: `tests/unit/block_math_tests.cpp`; console transcript for all requested Release gates
Plan drift audit: test coverage and immutable execution record only; no production semantics, CMake, Task 02, M1, proxy, driver, or acceptance document changes
Open risks: none introduced; Task 01 remains DONE pending independent re-review
Executor / timestamp: Codex / `2026-08-03T12:34:04+08:00`

Task: 01 — 代码质量审查 allocation-free range descriptor 修复
Status: DONE
Start commit / end commit: `8a1cea266062b1b565a2dd40c8db196a4f684239` / this Task 01 quality-fix commit
Files changed: `src/storage/block_math.h`, comments only in the existing `DynamicBlockStoreStats` fields in `src/storage/dynamic_block_types.h`, `tests/unit/block_math_tests.cpp`, and this appended `M2_TASKLIST.md` record
Red test and observed failure: after changing only tests to require the descriptor API, `cmake --build --preset release --target myrd_m2_block_math_tests` exited 1; Ninja compilation failed with code 2 because the old result was not `noexcept`/trivially-copyable and lacked `segment_count`, `segment_at`, and `BlockMathError::SegmentIndexOutOfRange`
API resolution: `validate_and_split_block_range(...) noexcept` now returns an allocation-free O(1) `BlockRangeSplitResult`; `segment_at(index) noexcept` computes one `BlockRangeSegment` with checked addition/multiplication and returns stable `SegmentIndexOutOfRange` for an invalid index; invalid requests retain `segment_count == 0`; no request-size cap was added
Focused verification command / exit code: Release test target build exit 0; `ctest --preset release --output-on-failure -R '^unit\.m2\.block_math$'` exit 0 (1/1), including a full 1 TiB descriptor with 16,777,216 addressable segments and first/middle/last O(1) lookup
Cumulative verification command / exit code: Release build exit 0; `ctest --preset release --output-on-failure -L m2-v1` exit 0 (5/5); full Release CTest exit 0 (28/28); `git diff --check` exit 0
Evidence paths: `src/storage/block_math.h`, `tests/unit/block_math_tests.cpp`; console transcript for red and all requested green gates
Plan drift audit: Task 01 quality fix only; no vector, range-length loop, invented max request, changed stripe rule, renamed stats field, Task 02 implementation, M1/proxy/driver change, or SPEC change
Open risks: downstream Task 05 request execution must iterate the validated descriptor deliberately and retain its own bounded cancellation/deadline checks; no downstream consumer exists yet in Task 01
Executor / timestamp: Codex / `2026-08-03T12:52:06+08:00`

Task: 01 — 质量复审 logical sector allowlist 修复
Status: DONE
Start commit / end commit: `0c3fa9a9f817fdd411a94fe445ad1f0cd2d3f84d` / this Task 01 sector-size fix commit
Files changed: `src/storage/block_constants.h`, `src/storage/block_math.h`, `src/storage/dynamic_block_types.h`, `tests/unit/block_math_tests.cpp`, and this appended `M2_TASKLIST.md` record
Red test and observed failure: with validator tests added before production changes, the Release target compiled but exact CTest exited 1 because a validly aligned `sector_size_bytes = 1024` request returned success instead of `BlockMathError::InvalidSectorSize`; a second test-first build exited 1/Ninja code 2 because the required constexpr/noexcept `is_supported_sector_size` helper did not yet exist
API resolution: centralized `constexpr bool is_supported_sector_size(std::uint32_t) noexcept` accepts exactly 512 and 4096; range splitting and create-argument validation both call it, so zero and every unsupported nonzero sector size map to their existing `InvalidSectorSize` errors without changing later alignment semantics
Focused verification command / exit code: Release test target build exit 0; `ctest --preset release --output-on-failure -R '^unit\.m2\.block_math$'` exit 0 (1/1), including 512/4096 success and 0/1/1024/65536 rejection coverage
Cumulative verification command / exit code: Release build exit 0; `ctest --preset release --output-on-failure -L m2-v1` exit 0 (5/5); full Release CTest exit 0 (28/28); `git diff --check` exit 0
Evidence paths: `src/storage/block_constants.h`, `tests/unit/block_math_tests.cpp`; console transcript for runtime red, compile red, and requested green gates
Plan drift audit: Task 01 sector contract only; no new request/stripe limit, Task 02, M1/proxy/driver behavior, CMake, stats schema, or SPEC change
Open risks: none introduced; Task 01 remains DONE pending independent re-review
Executor / timestamp: Codex / `2026-08-03T13:00:50+08:00`

Task: 01 — 独立审查关闭记录
Status: DONE
Start commit / end commit: `bb33c44` / `e3420ba`
Files changed: no product change; this appended audit record only
Red test and observed failure: see Task 01 implementation and three remediation records above
Focused verification command / exit code: final specification re-review returned COMPLIANT with no findings; final code-quality re-review at `e3420ba` returned APPROVED with no Critical, Important, or Minor findings
Cumulative verification command / exit code: Release build exit 0; exact `unit.m2.block_math` 1/1; M2-V1 5/5; full Release CTest 28/28; `git diff --check` exit 0
Evidence paths: Task 01 source/tests; commits `70fc9c1`, `8a1cea2`, `0c3fa9a`, `e3420ba`
Plan drift audit: Task 01 is closed; no Task 02 interface or implementation exists yet
Open risks: none for Task 01; coherent live stats production and ZeroBacking construction remain correctly assigned to later tasks
Executor / timestamp: Codex / `2026-08-03T13:05:49+08:00`

Task: 02 — 不可变 backing contract、Zero/Fake 与审查修复
Status: DONE
Start commit / end commit: `4c47538360751f483ce9fe35d3b82ca7a78f6edd` / this Task 02 implementation commit
Files changed: `src/storage/backing_store.h`, `src/storage/zero_backing_store.h`, `src/storage/zero_backing_store.cpp`, `tests/support/fake_backing_store.h`, `tests/unit/backing_store_tests.cpp`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, and only this Task 02 checklist/record in `M2_TASKLIST.md`
Red test and observed failure: the takeover retained an uncommitted test-first patch whose initial red transcript was not returned by the interrupted implementer; the independently reviewed remediation was test-first and its Release target build exited 1 with MSVC `C2039` because the newly required distinct `fail_block` and global `fail_on_load` APIs did not yet exist
Focused verification command / exit code: CTest JSON inventory confirmed effective `TIMEOUT = 15`; `ctest --preset release --output-on-failure -R '^unit\.m2\.backing_store$'` exit 0 (1/1)
Cumulative verification command / exit code: `ctest --preset release --output-on-failure -L m2-v1` exit 0 (6/6); full Release CTest exit 0 (29/29); `git diff --check` exit 0
Evidence paths: Task 02 source/tests; console transcript for focused/cumulative gates; independent specification review returned COMPLIANT and independent quality re-review returned APPROVED with no Critical, Important, or Minor findings
Plan drift audit: Task 02 only; Fake remains test-only, published content is immutable, and no filesystem, VHD/VHDX, proxy, AIM/driver, persistence, or M3 implementation was introduced
Open risks: TSAN is unavailable in the accepted Windows/MSVC toolchain; synchronized counters, condition-variable predicates, concurrent const-load tests, cancellation/deadline tests, lifetime tests, and a 15-second outer CTest timeout provide the required Task 02 evidence
Executor / timestamp: Codex / `2026-08-03T13:58:56+08:00`

Task: 03 — Windows VirtualMemoryArena 与故障注入
Status: DONE
Start commit / end commit: `0ccc14f4f9fc4a0e6b5bb94bd9f0694f727ee54d` / this Task 03 implementation commit
Files changed: `src/storage/virtual_memory_arena.h`, `src/storage/virtual_memory_arena.cpp`, `tests/support/fake_virtual_memory_api.h`, `tests/unit/virtual_memory_arena_tests.cpp`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, and only this Task 03 checklist/record in `M2_TASKLIST.md`
Red test and observed failure: after registering the complete test-first target, Release compilation exited 1 with MSVC `C1083` because `src/storage/virtual_memory_arena.h` did not exist; the first production pass also exposed and fixed a missing test-support constant include before green
Focused verification command / exit code: clean Release target rebuild exit 0; `ctest --preset release --output-on-failure -R '^unit\.m2\.virtual_memory$'` exit 0 (1/1)
Cumulative verification command / exit code: pre-review `ctest --preset release --output-on-failure -L m2-v1` exit 0 (7/7); full Release CTest exit 0 (30/30); final submitted-state cumulative gates are run after this commit
Evidence paths: Task 03 source/tests and console transcript; specification review returned COMPLIANT with zero gaps; no existing Application Verifier/guard-page configuration was found; quality review found one geometry-read/move data race, the synchronized-helper fix was rebuilt cleanly, and final quality re-review returned APPROVED with no Critical, Important, or Minor findings
Plan drift audit: Task 03 only; the 1 TiB arena creates no per-block metadata, Fake remains test-only, and no sparse directory, store state machine, proxy, driver, persistence, or Task 04 behavior was introduced
Open risks: real 64 GiB/1 TiB `VirtualAlloc` and `VirtualQuery` integration remains intentionally assigned to Task 18; Task 03 proves exact calls and failure semantics through the injectable seam
Executor / timestamp: Codex / `2026-08-03T14:48:31+08:00`

Task: 07 — 单飞 lazy backing load 与 hard-pressure read-through
Status: DONE
Start commit / end commit: `dddc8dbc60c6ca7f31168e32361c2f0104b9c412` / this Task 07 commit
Files changed: `src/storage/dynamic_block_store.h`, `src/storage/dynamic_block_store_core.cpp`, `src/storage/dynamic_block_store_internal.h`, `src/storage/dynamic_block_store_read.cpp`, `tests/support/fake_backing_store.h`, `tests/unit/dynamic_block_store_read_tests.cpp`, `tests/unit/dynamic_block_store_limit_tests.cpp`, `tests/CMakeLists.txt`, and this Task 07 checklist/status/record in `M2_TASKLIST.md`
Red test and observed failure: after adding deterministic concurrency/alignment tests before the final production fix, Debug `unit.m2.read` failed because the private backing destination was not 64 KiB aligned, and `unit.m2.limits` failed because 8 simultaneous hard-limit readers produced more than one backing load
Focused verification command / exit code: fresh-layout Debug and Release builds exit 0; exact `unit.m2.read|unit.m2.limits` 2/2 in both configurations; final submitted-state Debug and Release `unit.m2.read` and `unit.m2.limits` each passed `--repeat until-fail:100`
Cumulative verification command / exit code: complete Debug and Release builds exit 0; Debug M2-V1 11/11; Release M2-V1 11/11; full Release CTest 34/34; `git diff --check` exit 0
Evidence paths: `tests/unit/dynamic_block_store_read_tests.cpp`, `tests/unit/dynamic_block_store_limit_tests.cpp`, Task 07 production files, and the console transcripts for the red test, focused gates, four 100-run loops, cumulative gates, and final two-reviewer APPROVED verdicts
Review findings and resolution: hard-pressure waiters now share one epoch-scoped aligned private result; AllZero clears the destination outside metadata locks before publication; waiter counts use exception-safe RAII; owner cancellation/failure wakes real registered waiters; a fail-fast guard prevents any published read-through pointer from becoming dangling during an exceptional synchronization failure
Plan drift audit: implementation remains inside Plan Task 7 and sections 1.3/1.4; backing I/O holds no metadata/leaf lock; generation publication is coherent; no WRITE, range mutation, dirty snapshot, eviction, filesystem backing, M3/M4 persistence, proxy, or driver behavior was introduced
Status table normalization: rows 01–06 were corrected from stale TODO values to DONE to match their already accepted commits `70fc9c1`, `0ccc14f`, `bc1619c`, `309239d`, `58ac6ee`, and `dddc8db`; no earlier implementation was changed by this documentation correction
Open risks: Task 12 must extend global invariant/accounting checks over all reservation/publication counters; Task 07 itself has no open Critical, Important, or Minor review findings
Executor / timestamp: Codex / `2026-08-03T17:32:37+08:00`

Task: 08 — WRITE、copy-on-load 与内容版本
Status: DONE
Start commit / end commit: `e55fc5b4b96709831b7da81c03f56dfb2de5b68e` / this Task 08 commit
Files changed: `src/CMakeLists.txt`, `src/storage/dynamic_block_store.h`, `src/storage/dynamic_block_store_core.cpp`, `src/storage/dynamic_block_store_internal.h`, `src/storage/dynamic_block_store_read.cpp`, `src/storage/dynamic_block_store_write.cpp`, `tests/CMakeLists.txt`, `tests/unit/dynamic_block_store_read_tests.cpp`, `tests/unit/dynamic_block_store_write_tests.cpp`, and this Task 08 checklist/status/record in `M2_TASKLIST.md`
Red test and observed failure: the first Debug `unit.m2.write` run failed because the previous WRITE stub returned not-implemented; deterministic follow-up red tests exposed an old reader falsely rejecting `Committing`, a mutation owner incorrectly failing ordinary hard-limit read waiters, the inverse owner order issuing a second backing load, and owner-scoped cancellation leaking to an independent waiter
Focused verification command / exit code: current-layout Debug and Release builds exit 0; exact `unit.m2.write|unit.m2.read|unit.m2.limits` 3/3 in both configurations; final submitted-state Debug and Release focused groups each passed 100 consecutive iterations
Cumulative verification command / exit code: final Debug full CTest 35/35; final Release full CTest 35/35; both report M2-V1 12 tests; `git diff --check` exit 0
Evidence paths: `src/storage/dynamic_block_store_write.cpp`, the shared load/mutation path in `src/storage/dynamic_block_store_read.cpp`, `tests/unit/dynamic_block_store_write_tests.cpp`, the tightened owner-cancellation case in `tests/unit/dynamic_block_store_read_tests.cpp`, and console transcripts for the red tests, focused gates, two 100-run loops, full gates, and reviewer rechecks
Review findings and resolution: partial Unresident mutation now patches the Task 07 private 64 KiB scratch and publishes `ResidentDirty` once instead of exposing an intermediate `ResidentClean`; hard-pressure read and mutation participants interpret one shared load according to their own policy in both owner orders; owner-only cancellation/timeout is no longer broadcast to valid waiters, while shared backing/commit errors remain epoch-scoped; both original reviewers returned `RESOLVED`
Plan drift audit: implementation remains inside Plan Task 8 and the approved state table; full writes never load backing, partial writes reuse the Task 07 single-flight state machine, M1 multi-block prefix/error semantics are unchanged, and no UNMAP/ZERO, dirty snapshot/ack, eviction, file backing, proxy, driver, M3, or M4 behavior was introduced
Open risks: synchronous clean eviction under hard pressure remains intentionally assigned to Task 12; snapshot/mutation and broader write/write race matrices remain assigned to Tasks 10, 14, and 15; Task 08 has no open review finding
Executor / timestamp: Codex / `2026-08-03T18:26:34+08:00`
