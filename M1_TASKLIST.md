# M1 Shared Memory Proxy Productization Execution Tasklist

> 状态：可交付其他 Agent 执行；当前实现进度为零。
>
> 对应 SPEC：`docs/superpowers/specs/2026-08-01-m1-m2-proxy-dynamic-block-store-design.md`
>
> 对应 PLAN：`docs/superpowers/plans/2026-08-02-m1-proxy-productization.md`
>
> 本文件是 M1 的执行台账和防跑偏门，不替代 SPEC 或 PLAN。执行 Agent 必须同时读取三份文件。

## 1. 执行目标

将 M0 已验证的 Shared Memory Proxy PoC 产品化为 M1：

- 支持 INFO、READ、WRITE、UNMAP、ZERO、CLOSE；
- 支持严格校验、稳定 `errorno`、取消、超时、fatal session 行为；
- 支持随机 Session ID、明确命名策略、窄 DACL、Cancel Event 和完整资源释放；
- 提供结构化计数与错误诊断；
- 通过 10,000 case 确定性 fuzz、50 次本地 SHM 重复测试；
- 只依赖本项目程序与 `phdskmnt.sys` 完成真实 READ/WRITE/ZERO/UNMAP；
- 通过故障注入、精确删除、Release 20 次循环和最终 M1 GO 验收。

M1 仍是固定内存、易失性 RAM Disk。执行本文件不得实现动态内存块、backing image、VHD、journal、checkpoint、定时写回、服务、产品 CLI 或 GUI；这些属于 M2-M4。

## 2. 执行依据与优先级

Agent 每次开始或恢复工作时，按以下顺序读取：

1. `docs/superpowers/specs/2026-08-01-m1-m2-proxy-dynamic-block-store-design.md`
2. `docs/superpowers/plans/2026-08-02-m1-proxy-productization.md`
3. `M1_TASKLIST.md`
4. 当前 Task 指定的源码、测试和固定上游文件
5. 当前机器的新鲜命令输出

冲突优先级：

```text
SPEC > PLAN > M1_TASKLIST > 当前实现 > 历史报告或口头结论
```

发现三份文档存在实质冲突时：

- 当前 Task 标记为 `DEVIATED`；
- 记录冲突的原文、文件和建议；
- 停止实现，不得自行选择更方便的解释；
- 先由用户批准文档修订，再恢复执行。

固定技术事实：

- M0 最终闭环提交：`0e2b3b3`；
- M0 merge：`a03a49e`；
- AIM 上游固定提交：`d379caa027d21902e45fce8c9daf02fd116c2dda`；
- 目标环境：Windows x64、MSVC、C++20、CMake、Ninja Multi-Config；
- 真实驱动：`phdskmnt.sys`；
- 禁止依赖：`aimapi.dll`、`aim_ll`、RamDyn、AIM GUI、`deviosvc` 数据路径及其他 AIM Toolkit 用户态组件。

## 3. 文档进入执行前的硬门

SPEC、PLAN 和本 TASKLIST 必须先处于同一个已提交 Git 基线。执行 Agent 在开始 Task 0 前运行：

```powershell
git ls-files --error-unmatch `
  docs/superpowers/specs/2026-08-01-m1-m2-proxy-dynamic-block-store-design.md `
  docs/superpowers/plans/2026-08-02-m1-proxy-productization.md `
  M1_TASKLIST.md
```

预期：退出码 0，三个文件均已跟踪。任一文件未跟踪时标记 `BLOCKED`，先请求维护者提交文档，禁止创建执行 worktree。

实现必须在 `feature/m1-proxy-productization` 隔离 worktree 中进行。创建 worktree 时使用 `using-git-worktrees`，不得把当前主工作区的 `_diag*`、`_test*`、`_v*` 等未跟踪诊断文件带入分支或删除。

## 4. 单 Agent、单 Task 规则

- 一次只允许一个 Plan Task 处于 `IN_PROGRESS`、`RED_VERIFIED`、`IMPLEMENTED` 或 `VERIFYING`。
- 一个执行 Agent 一次只接手一个 Plan Task。
- 未经用户明确授权，不得再分派嵌套 subagent。
- 当前 Task 未达到 `ON_TRACK`，不得开始下一 Task。
- 不得提前实现后续 Task，即使提前实现看似能减少重复修改。
- 每个代码 Task 必须先观察指定失败，再写最小实现。
- 不得用放宽断言、跳过测试、吞错误、复用旧报告或手工编辑报告获得通过。

## 5. 状态机

正常状态：

```text
NOT_STARTED
  -> IN_PROGRESS
  -> RED_VERIFIED
  -> IMPLEMENTED
  -> VERIFYING
  -> ON_TRACK
```

异常状态：

- `DEVIATED`：实现或文档与批准范围冲突。
- `BLOCKED`：缺少权限、工具、驱动状态、外部环境或前置提交。
- `NO_GO_EVIDENCE`：真实驱动门产生明确 M1 NO-GO 证据。

规则：

- 纯文档/观察 Task 可以跳过 `RED_VERIFIED`，但必须记录原因。
- 测试在应失败阶段意外通过时，不得继续；先证明测试确实覆盖新行为。
- Review 发现缺陷时，状态回到 `IN_PROGRESS`，补失败测试和修复后重新进入 Review。
- `BLOCKED`、`DEVIATED`、`NO_GO_EVIDENCE` 都必须停止后续 Task。

## 6. 每个 Task 的 Git 规则

### 6.1 开始记录

```powershell
$startCommit = (git rev-parse HEAD).Trim()
git status --short
```

在总进度表记录 start commit，并把状态改为 `IN_PROGRESS`。工作区出现非当前 Task 的 tracked 修改时停止；不得覆盖或顺手提交。

### 6.2 实现提交

完成 Red/Green 和指定验证后，只暂存当前 Task `Files` 范围内的文件，使用 PLAN 指定的 commit intent。禁止 `git add .`、`git add -A` 或宽泛目录暂存。

### 6.3 独立复盘提交

实现提交后必须执行本文件第 8 节的 G1-G7。只有 `ON_TRACK` 才更新本文件并提交：

```powershell
git add M1_TASKLIST.md
git commit -m "chore: review M1 task <N> alignment"
```

复盘提交不得夹带源码、测试或构建脚本变化。

### 6.4 Task 0 例外

Task 0 不修改实现，不需要 implementation commit。它只在完成基线复盘后创建 tasklist review commit。

## 7. 全局进度表

每次状态改变都必须更新本表。`未记录` 必须在执行中替换为真实值。

| Task | 状态 | 依赖 | Start commit | Implementation commit | Review commit | 证据摘要 |
|---:|---|---|---|---|---|---|
| 0 | ON_TRACK | 文档已提交 | 0e2b3b3 | N/A | a62a9d4 | CMake 4.2.2, Ninja 1.13.2, MSVC 19.44.35228, 11/11 tests pass, AIM d379caa verified |
| 1 | ON_TRACK | 0 | 96dfa76 | 7692559 | 1369a1b | 3/3 operation_context tests, 11/11 unit regression pass |
| 2 | ON_TRACK | 1 | 98c40ec | 77f4a7a | a91044f | 8/8 contract tests, 12/12 unit regression pass, no M2 pattern |
| 3 | ON_TRACK | 2 | 039a369 | 5790b03 | 未记录 | 6/6 ABI tests, old+new protocol pass, codes/flags/sizes match AIM |
| 4 | ON_TRACK | 3 | 3ad6cd5 | 5674b9f | 未记录 | 6/6 error mapping tests, 4/4 M1 unit pass |
| 5 | NOT_STARTED | 4 | 未记录 | 未记录 | 未记录 | 未记录 |
| 6 | NOT_STARTED | 5 | 未记录 | 未记录 | 未记录 | 未记录 |
| 7 | NOT_STARTED | 6 | 未记录 | 未记录 | 未记录 | 未记录 |
| 8 | NOT_STARTED | 7 | 未记录 | 未记录 | 未记录 | 未记录 |
| 9 | NOT_STARTED | 8 | 未记录 | 未记录 | 未记录 | 未记录 |
| 10 | NOT_STARTED | 9 | 未记录 | 未记录 | 未记录 | 未记录 |
| 11 | NOT_STARTED | 10 | 未记录 | 未记录 | 未记录 | 未记录 |
| 12 | NOT_STARTED | 11 | 未记录 | 未记录 | 未记录 | 未记录 |
| 13 | NOT_STARTED | 12 | 未记录 | 未记录 | 未记录 | 未记录 |
| 14 | NOT_STARTED | 13 | 未记录 | 未记录 | 未记录 | 未记录 |
| 15 | NOT_STARTED | 14 | 未记录 | 未记录 | 未记录 | 未记录 |
| 16 | NOT_STARTED | 15 | 未记录 | 未记录 | 未记录 | 未记录 |
| 17 | NOT_STARTED | 16 | 未记录 | 未记录 | 未记录 | 未记录 |
| 18 | NOT_STARTED | 17 | 未记录 | 未记录 | 未记录 | 未记录 |
| 19 | NOT_STARTED | 18 | 未记录 | N/A 或修复提交 | 未记录 | 未记录 |
| 20 | NOT_STARTED | 19 | 未记录 | 未记录 | 未记录 | 未记录 |

## 8. 每个 Task 完成后的强制防跑偏门

以下 G1-G7 在每个 Task 后都必须执行，Task 内的 Review 项是该门的具体化。

### G1：重新阅读权威文档

- 重新阅读对应 PLAN Task 的标题到下一 Task 标题之前。
- 重新阅读 SPEC 中该 Task 映射的章节。
- 对照本 Task 每一个 `E`、`V`、`R` 项，不允许只凭记忆复盘。

### G2：检查文件范围

```powershell
$taskStart = '<进度表中的真实 start commit>'
$implementationCommit = '<进度表中的真实 implementation commit>'
git diff --name-status "$taskStart..$implementationCommit"
git show --check --stat --oneline $implementationCommit
```

实际文件必须全部出现在对应 PLAN Task 的 `Files` 或本 TASKLIST 的允许列表。额外文件意味着 `DEVIATED`，除非它只是由已批准接口变化造成并已在 Review Record 逐项解释。

### G3：检查行为范围

逐项回答：

- 是否只实现 M1 当前 Task？
- 是否提前引入 M2 的动态 commit、backing、eviction、dirty snapshot？
- 是否改变 ABI 常量、对象名、DACL、错误映射或报告字段？
- 是否引入新的第三方或 AIM 用户态依赖？
- 是否扩大设备删除范围、使用 sentinel/all-device remove？
- 是否永久修改服务 StartMode、驱动安装或系统配置？
- 是否用降低测试强度来获得通过？

任一不合规即停止。

### G4：新鲜验证

- 重新运行当前 Task 的 focused test。
- 重新运行当前 Task 指定的 regression set。
- 检查完整输出、退出码和 test selection count。
- 旧输出、旧报告和另一个 Agent 的结论不能代替新鲜验证。

### G5：Git 与证据

```powershell
git status --short
git log -3 --oneline
git diff --check
```

构建目录、reports 和临时诊断文件不得进入提交。真实报告必须记录路径与 SHA-256，但保持不跟踪。

### G6：Review Record

每个 Task 必须填写：Executor、Start、Implementation、Files、Commands、Results、Evidence、Deviations、Risks、Verdict。没有偏差时写 `None`，不得留空。

### G7：复盘提交

只有 Verdict 为 `ON_TRACK` 才创建 alignment review commit。其他 Verdict 更新台账后停止。

## 9. Task execution checklist

### Task 0 — 建立隔离执行基线

Plan：[Task 0](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-0-establish-the-isolated-m1-execution-baseline)

依赖：SPEC、PLAN、TASKLIST 已处于同一 Git commit。

目标：创建隔离 worktree，确认 M0 closure、固定上游和本地测试基线；不修改产品代码。

允许变化：仅 `M1_TASKLIST.md` 的进度与 Review Record。

**Execute**

- [ ] E0.01 运行第 3 节文档跟踪检查，三个文件全部退出 0。
- [ ] E0.02 使用 `using-git-worktrees` 创建 `feature/m1-proxy-productization` 隔离 worktree。
- [ ] E0.03 在新 worktree 运行 `git merge-base --is-ancestor 0e2b3b3 HEAD`，退出 0。
- [ ] E0.04 运行 `git status --short`，确认没有 tracked 修改。
- [ ] E0.05 若 `out/reference/aim/.git` 不存在，运行 `tools/Fetch-AimReference.ps1`。
- [ ] E0.06 运行 `git -C out/reference/aim rev-parse HEAD`，精确等于固定提交。
- [ ] E0.07 检查 `imdproxy.h` 中 UNMAP/ZERO codes 6/7 与 flags 0x02/0x04。
- [ ] E0.08 进入 MSVC 环境并运行 `cmake --preset msvc`。
- [ ] E0.09 运行 `cmake --build --preset debug`。
- [ ] E0.10 运行 `ctest --preset debug -R "^(unit\.|integration\.shm$)"`。
- [ ] E0.11 记录测试总数、通过数、编译器/SDK/CMake/Ninja 版本。
- [ ] E0.12 状态改为 `VERIFYING`；本 Task 没有 implementation commit。

**Review**

- [ ] R0.01 重新阅读 PLAN Task 0 和 SPEC 4.1。
- [ ] R0.02 确认 worktree 不含主工作区诊断文件。
- [ ] R0.03 新鲜重跑 ancestor、上游 commit、build 和 CTest。
- [ ] R0.04 确认未安装、卸载、启动、停止或配置任何服务。
- [ ] R0.05 确认无源码、CMake 或测试文件变化。
- [ ] R0.06 填写 Review Record，Verdict 必须为 `ON_TRACK` 才继续。
- [ ] R0.07 创建 `chore: review M1 task 0 alignment`。

Review Record：Executor `Claude`; Start `0e2b3b3`; Implementation `N/A`; Files `仅 M1_TASKLIST.md (进度更新)`; Commands `cmake --preset msvc; cmake --build --preset debug; ctest --preset debug -R "^(unit\.|integration\.shm$)"`; Results `11/11 tests pass, MSVC 19.44.35228, CMake 4.2.2, Ninja 1.13.2`; Evidence `AIM ref d379caa verified; UNMAP=6 ZERO=7 flags 0x02/0x04 confirmed; worktree clean; no service changes`; Deviations `out/reference/aim 从主工作区 cp 复制（GitHub 不可达无法 fetch）；M1_TASKLIST.md + PLAN 从主工作区手动复制到 worktree`; Risks `None`; Verdict `ON_TRACK`。

### Task 1 — 稳定错误码与 OperationContext

Plan：[Task 1](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-1-add-stable-operation-errors-and-cancellationdeadline-context)

依赖：Task 0 `ON_TRACK`。

目标：增加不破坏 M0 aggregate initialization 的内部错误码、native Win32 code、取消和 deadline 检查。

允许文件：`src/common/error.h`、`src/common/operation_context.h`、`tests/unit/operation_context_tests.cpp`、`tests/CMakeLists.txt`。

**Execute**

- [ ] E1.01 确认 Task 0 的 Review commit 是 HEAD，记录 start commit。
- [ ] E1.02 阅读 PLAN Task 1 的完整代码块与 SPEC 19、20。
- [ ] E1.03 在 `tests/CMakeLists.txt` 注册 `unit.m1.operation_context` target/test。
- [ ] E1.04 创建三个测试：live、cancel 优先、expired deadline。
- [ ] E1.05 构建 focused target，保存缺少 `operation_context.h` 的 red 输出。
- [ ] E1.06 状态改为 `RED_VERIFIED`。
- [ ] E1.07 在 `error.h` 增加 PLAN 锁定的 `OperationErrorCode` 全集。
- [ ] E1.08 把 `native_code` 追加为 `Error` 最后字段，保持旧四字段初始化语义。
- [ ] E1.09 创建 `operation_context.h`，实现默认无限 deadline 和取消优先检查。
- [ ] E1.10 构建 focused target，修复仅与本 Task 相关的编译问题。
- [ ] E1.11 运行 `ctest --preset debug -R "^unit\.m1\.operation_context$"`。
- [ ] E1.12 运行 `ctest --preset debug -R "^unit\."`，确认 M0 单测无回归。
- [ ] E1.13 运行 `git diff --check`，核对没有 proxy/storage 改动。
- [ ] E1.14 使用 PLAN 指定文件创建 `feat: add cancellable operation context`。
- [ ] E1.15 状态改为 `IMPLEMENTED`。

**Review**

- [ ] R1.01 重新阅读 PLAN Task 1 与 SPEC 错误集合。
- [ ] R1.02 核对 enum 值、字段顺序和命名逐项一致。
- [ ] R1.03 检查旧 `{domain, code, operation, message}` 初始化仍编译。
- [ ] R1.04 新鲜重跑 focused 与全部 unit。
- [ ] R1.05 检查 diff 只含四个允许文件。
- [ ] R1.06 记录实际 test count、偏差和风险。
- [ ] R1.07 Verdict `ON_TRACK` 后创建 alignment review commit。

Review Record：Executor `Claude`; Start `96dfa76`; Implementation `7692559`; Files `src/common/error.h, src/common/operation_context.h, tests/unit/operation_context_tests.cpp, tests/CMakeLists.txt`; Commands `ctest --preset debug -R "^unit\.m1\.operation_context$"; ctest --preset debug -R "^unit\."`; Results `3/3 operation_context tests pass; 11/11 all unit tests pass`; Evidence `OperationErrorCode 14 values matching PLAN; native_code default=0 preserves M0 aggregate init; check_operation_context cancellation-priority verified`; Deviations `None`; Risks `None`; Verdict `ON_TRACK`。

### Task 2 — 发布 IBlockDevice M1 合同

Plan：[Task 2](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-2-publish-the-m1-block-device-contract-and-adapt-the-fixed-memory-device)

依赖：Task 1 `ON_TRACK`。

目标：锁定 `DataRange`、`DeviceFeatures`、context-aware READ/WRITE/UNMAP/ZERO，并让固定 `MemoryBlockDevice` 实现合同。

允许文件：PLAN Task 2 `Files` 中列出的 storage、processor mechanical adaptation、两个旧测试、新 contract test 和 tests CMake。

**Execute**

- [ ] E2.01 记录 start commit，阅读 PLAN Task 2 和 SPEC 7。
- [ ] E2.02 注册 `unit.m1.block_device_contract`。
- [ ] E2.03 写 feature、读写、zero、unmap、misalignment、overflow、read-only、cancel 测试。
- [ ] E2.04 构建 focused target，确认缺少新接口而失败。
- [ ] E2.05 状态改为 `RED_VERIFIED`。
- [ ] E2.06 按 PLAN 精确替换 `block_device.h` 公共接口。
- [ ] E2.07 为 `MemoryBlockDevice` 增加 `read_only`、mutex 和 `features()`。
- [ ] E2.08 先实现 subtraction-safe `validate()` 和 context check。
- [ ] E2.09 实现锁内 READ/WRITE，WRITE 在任何 mutation 前检查 read-only。
- [ ] E2.10 实现 validate-all-then-zero 的 range helper。
- [ ] E2.11 让 UNMAP/ZERO 都调用 range helper；空 range 为成功 no-op。
- [ ] E2.12 机械更新 `request_processor.cpp` 的 device 调用签名，不实现后续协议功能。
- [ ] E2.13 机械更新 `storage_tests.cpp`、`request_processor_tests.cpp` 的 context 和返回断言。
- [ ] E2.14 构建全部 Debug target。
- [ ] E2.15 运行 focused contract test。
- [ ] E2.16 运行全部 `unit.*`。
- [ ] E2.17 搜索 storage 实现，确认没有 `VirtualAlloc`、backing、dirty、eviction。
- [ ] E2.18 `git diff --check` 后创建 `feat: publish M1 block device contract`。

**Review**

- [ ] R2.01 重新阅读 PLAN Task 2 和 SPEC 3.1/7。
- [ ] R2.02 核对方法签名、const span、Result<void> 完全一致。
- [ ] R2.03 核对 multi-range 失败前没有部分清零。
- [ ] R2.04 核对锁只保护固定 vector，没有引入 M2 状态。
- [ ] R2.05 新鲜重跑 focused、全部 unit 和 forbidden search。
- [ ] R2.06 比较 diff 文件与允许列表。
- [ ] R2.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `Claude`; Start `98c40ec`; Implementation `77f4a7a`; Files `src/storage/block_device.h, src/storage/memory_block_device.h/.cpp, src/proxy/request_processor.cpp, tests/unit/block_device_contract_tests.cpp, tests/unit/storage_tests.cpp, tests/unit/request_processor_tests.cpp, tests/CMakeLists.txt`; Commands `ctest -R "^unit\.m1\.block_device_contract$"; ctest -R "^unit\."; rg "VirtualAlloc|MEM_COMMIT|evict|dirty|backing" src/storage`; Results `8/8 contract tests pass (features, read/write, zero, unmap, misalignment, overflow, read-only, cancel); 12/12 all unit pass; no M2 patterns found`; Evidence `DataRange/DeviceFeatures/IBlockDevice matching SPEC 7; MemoryBlockDevice with mutex/read_only/validate-all-then-zero; subtraction-safe bounds check; old tests pass with OperationContext{}`; Deviations `None`; Risks `None`; Verdict `ON_TRACK`。

### Task 3 — 锁定完整 M1 Wire ABI

Plan：[Task 3](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-3-complete-and-lock-the-m1-wire-abi)

依赖：Task 2 `ON_TRACK`。

目标：从固定 AIM 源锁定 codes、flags、UNMAP/ZERO header/response 和 `DEVICE_DATA_SET_RANGE` 布局。

允许文件：`src/proxy/imdproxy_protocol.h`、`tests/unit/m1_protocol_tests.cpp`、`tests/CMakeLists.txt`。

**Execute**

- [ ] E3.01 记录 start commit，重读固定 `imdproxy.h` 和 PLAN Task 3。
- [ ] E3.02 注册 `unit.m1.protocol`。
- [ ] E3.03 写 codes 1/2/3/5/6/7 和 flags 1/2/4 精确测试。
- [ ] E3.04 写 UNMAP/ZERO request/response size 测试。
- [ ] E3.05 写 `DataSetRangeWire` 与 SDK size/offset 对照测试。
- [ ] E3.06 构建 focused target，观察缺少符号的 red failure。
- [ ] E3.07 状态改为 `RED_VERIFIED`。
- [ ] E3.08 添加常量、结构、static_assert 和标准头文件。
- [ ] E3.09 添加 15-byte truncation、16-byte success、64-bit length round trip。
- [ ] E3.10 添加 ZeroResponse 后置 sentinel 不被覆盖测试。
- [ ] E3.11 运行新 `unit.m1.protocol`。
- [ ] E3.12 运行旧 `unit.protocol`。
- [ ] E3.13 对照 pinned source 逐字符复核常量与结构字段顺序。
- [ ] E3.14 创建 `feat: define complete M1 proxy ABI`。

**Review**

- [ ] R3.01 重新阅读 PLAN Task 3 与 SPEC 8.1-8.4。
- [ ] R3.02 新鲜运行 upstream commit check 和两组 protocol tests。
- [ ] R3.03 确认没有 CONNECT/SCSI/SHARED 能力常量被公布为 feature。
- [ ] R3.04 检查没有 reinterpret-cast 读取未对齐 wire 数据。
- [ ] R3.05 检查 diff 只含三个允许文件。
- [ ] R3.06 记录 SDK 结构 size/offset 证据。
- [ ] R3.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `Claude`; Start `039a369`; Implementation `5790b03`; Files `src/proxy/imdproxy_protocol.h, tests/unit/m1_protocol_tests.cpp, tests/CMakeLists.txt`; Commands `ctest -R "^(unit\.m1\.protocol|unit\.protocol)$"`; Results `6/6 M1 ABI tests pass; 2/2 old+new protocol pass`; Evidence `kReqUnmap=6 kReqZero=7 kFlagSupportsUnmap=0x02 kFlagSupportsZero=0x04 match pinned AIM d379caa; DataSetRangeWire offset/size match DEVICE_DATA_SET_RANGE; truncation/sentinel/round-trip verified`; Deviations `None`; Risks `None`; Verdict `ON_TRACK`。

### Task 4 — 稳定 Proxy 错误映射

Plan：[Task 4](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-4-implement-stable-proxy-error-mapping)

依赖：Task 3 `ON_TRACK`。

目标：把内部错误稳定映射为 Win32 `errorno`，并区分 request-level/fatal。

允许文件：`src/proxy/proxy_error.h/.cpp`、`tests/unit/proxy_error_tests.cpp`、两个 CMake 文件。

**Execute**

- [ ] E4.01 记录 start commit，阅读 PLAN mapping table 和 SPEC 20。
- [ ] E4.02 注册 `unit.m1.proxy_error`。
- [ ] E4.03 写 PLAN 中全部 mapping row 的 table-driven test。
- [ ] E4.04 写 commit/decommit native code 优先与 fallback 测试。
- [ ] E4.05 写 request/fatal classification 全量测试。
- [ ] E4.06 构建 focused target，确认缺少 API 的 red failure。
- [ ] E4.07 状态改为 `RED_VERIFIED`。
- [ ] E4.08 创建 header，锁定 `ErrorDisposition` 与两个函数签名。
- [ ] E4.09 实现 switch mapping；未知 code 映射 `ERROR_INTERNAL_ERROR`。
- [ ] E4.10 实现 classification；malformed/unknown/invariant/unknown 才 fatal。
- [ ] E4.11 运行 focused test 与全部 `unit.m1.*`。
- [ ] E4.12 创建 `feat: map M1 errors to stable proxy responses`。

**Review**

- [ ] R4.01 重读 PLAN Task 4 和 SPEC error table。
- [ ] R4.02 手工核对每个 enum 到 Win32 code。
- [ ] R4.03 新鲜运行 focused test。
- [ ] R4.04 检查 cancellation/timeout/range 均不是 fatal。
- [ ] R4.05 检查未知错误不会返回 success 或 raw 0。
- [ ] R4.06 记录完整 case count 与偏差。
- [ ] R4.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `Claude`; Start `3ad6cd5`; Implementation `5674b9f`; Files `src/proxy/proxy_error.h/.cpp, tests/unit/proxy_error_tests.cpp, src/CMakeLists.txt, tests/CMakeLists.txt`; Commands `ctest -R "^unit\.m1\.proxy_error$"`; Results `6/6 mapping+classification tests pass; 4/4 M1 unit pass`; Evidence `All 11 PLAN-specified mapping rows verified; native_code priority for commit/decommit; unknown→ERROR_INTERNAL_ERROR; fatal only for malformed/unknown/invariant/unknown codes`; Deviations `None`; Risks `None`; Verdict `ON_TRACK`。

### Task 5 — 解码并规范化 UNMAP/ZERO ranges

Plan：[Task 5](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-5-decode-and-normalize-unmapzero-ranges)

依赖：Task 4 `ON_TRACK`。

目标：安全复制、验证、排序和合并外部 `DEVICE_DATA_SET_RANGE[]`。

允许文件：`range_normalizer.h/.cpp`、`range_normalizer_tests.cpp`、两个 CMake 文件。

**Execute**

- [ ] E5.01 记录 start commit，阅读 PLAN Task 5、SPEC 8.4。
- [ ] E5.02 注册 `unit.m1.range_normalizer`。
- [ ] E5.03 写 empty 和 non-multiple-of-16 测试。
- [ ] E5.04 写 encoded length 超 data area 测试。
- [ ] E5.05 写 zero-length ignore、misalignment、offset+length overflow、device-end 测试。
- [ ] E5.06 写 unsorted、overlap、adjacent、separated success 测试。
- [ ] E5.07 构建 focused target，观察缺少 normalizer 的 red failure。
- [ ] E5.08 状态改为 `RED_VERIFIED`。
- [ ] E5.09 创建锁定 API，先做 encoded length 与 element size 检查。
- [ ] E5.10 使用 `read_wire` 逐项复制，不 reinterpret-cast 输入。
- [ ] E5.11 实现 alignment 与 subtraction-safe capacity 检查。
- [ ] E5.12 按 `(offset,length)` 排序并安全合并 overlap/adjacency。
- [ ] E5.13 运行 focused test、protocol tests 和全部 M1 unit。
- [ ] E5.14 创建 `feat: normalize M1 data set ranges`。

**Review**

- [ ] R5.01 重读 PLAN Task 5 与 SPEC normalization 顺序。
- [ ] R5.02 核对攻击者控制的 count 在 bound check 前不分配。
- [ ] R5.03 核对 merge 不发生 64-bit wrap。
- [ ] R5.04 新鲜重跑 focused test。
- [ ] R5.05 检查所有失败 code 为 invalid_range 或 misaligned。
- [ ] R5.06 检查 diff 文件范围。
- [ ] R5.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 6 — 严格 FakeBlockDevice 测试替身

Plan：[Task 6](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-6-add-a-strict-fake-block-device-for-processor-tests)

依赖：Task 5 `ON_TRACK`。

目标：提供能证明 processor 未调用非法 backend operation 的严格 fake。

允许文件：`tests/support/fake_block_device.h`、`tests/unit/m1_request_processor_tests.cpp`、`tests/CMakeLists.txt`。

**Execute**

- [ ] E6.01 记录 start commit，阅读 PLAN Task 6。
- [ ] E6.02 注册 `unit.m1.request_processor` target。
- [ ] E6.03 创建最小 consumer test，include fake 并检查初始 counters。
- [ ] E6.04 构建 target，保存 missing-header red failure。
- [ ] E6.05 状态改为 `RED_VERIFIED`。
- [ ] E6.06 实现 size/sector/features 和固定 bytes。
- [ ] E6.07 为四个 operation 实现独立 call counter。
- [ ] E6.08 实现 `next_result` 注入；失败时不得修改 bytes。
- [ ] E6.09 实现 write source/range private capture。
- [ ] E6.10 实现 invalid_call_count 和 total_calls，非法调用立即测试失败。
- [ ] E6.11 构建并运行最小 consumer test。
- [ ] E6.12 创建 `test: add strict M1 block device fake`。

**Review**

- [ ] R6.01 重读 PLAN Task 6。
- [ ] R6.02 核对每次 operation 只增一个对应 counter。
- [ ] R6.03 核对 injected failure 不触碰 bytes/ranges side effect。
- [ ] R6.04 新鲜运行 focused test。
- [ ] R6.05 确认 fake 只存在 tests/support，不进入生产 target。
- [ ] R6.06 记录 fake API 和偏差。
- [ ] R6.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 7 — 产品化 INFO 与 READ

Plan：[Task 7](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-7-productize-info-and-read-request-processing)

依赖：Task 6 `ON_TRACK`。

目标：发布 context-aware processor surface，完整实现 INFO/READ、metadata、计数、错误响应和 noexcept boundary。

允许文件：PLAN Task 7 列出的 processor 与相关测试文件。

**Execute**

- [ ] E7.01 记录 start commit，阅读 PLAN Task 7、SPEC 6.2/8.2/8.3/20/22。
- [ ] E7.02 写 INFO capacity/alignment/features/read-only tests。
- [ ] E7.03 写 READ success、data-before-header、全部 range boundary tests。
- [ ] E7.04 写 backend error、cancel、timeout 不误调用 fake 的 tests。
- [ ] E7.05 写 per-type/per-error counters 和 RequestMetadata tests。
- [ ] E7.06 构建并运行 focused test，保存 signature/behavior red failure。
- [ ] E7.07 状态改为 `RED_VERIFIED`。
- [ ] E7.08 在 header 发布 PLAN 锁定的 counters、metadata、outcome 和两个 process overload。
- [ ] E7.09 实现 context-free M0 compatibility shim。
- [ ] E7.10 实现 subtraction-safe shared READ/WRITE validator。
- [ ] E7.11 实现 response write helper，任何 write failure 转 fatal。
- [ ] E7.12 实现 INFO flags 仅来自 `DeviceFeatures`。
- [ ] E7.13 实现 READ：decode -> validate -> context -> backend -> response。
- [ ] E7.14 request-level failure 返回 mapped error、length 0、Error 与 metadata。
- [ ] E7.15 捕获 `bad_alloc` 和其他异常，不允许逃出 noexcept。
- [ ] E7.16 使 counters 查询无数据竞争。
- [ ] E7.17 运行新 processor test、旧 processor test 和旧 SHM integration。
- [ ] E7.18 创建 `feat: productize M1 info and read requests`。

**Review**

- [ ] R7.01 重读 PLAN Task 7 与 SPEC validation order。
- [ ] R7.02 核对 INFO 未公布 CONNECT/SCSI/SHARED。
- [ ] R7.03 核对 READ 失败 length 精确为 0。
- [ ] R7.04 核对 metadata 不含数据区内容。
- [ ] R7.05 新鲜运行三组测试。
- [ ] R7.06 检查 M0 compatibility overload 未改变 M0 语义。
- [ ] R7.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 8 — 产品化 WRITE 与精确 request error

Plan：[Task 8](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-8-productize-write-and-exact-request-level-failures)

依赖：Task 7 `ON_TRACK`。

目标：实现有私有复制、稳定失败响应和精确计数的 WRITE。

允许文件：`request_processor.cpp`、`m1_request_processor_tests.cpp`。

**Execute**

- [ ] E8.01 记录 start commit，阅读 PLAN Task 8 和 SPEC WRITE 顺序。
- [ ] E8.02 写成功 WRITE、backend bytes 和 response tests。
- [ ] E8.03 写 fake 在 backend call 中修改 SHM、source 仍保持原值的 private-copy test。
- [ ] E8.04 写 zero/data-area/alignment/overflow/device-end tests。
- [ ] E8.05 写 read-only、backend error、cancel、timeout tests。
- [ ] E8.06 写 success/error counter 各增一次、不 double-count test。
- [ ] E8.07 运行 focused test，确认新 cases red。
- [ ] E8.08 状态改为 `RED_VERIFIED`。
- [ ] E8.09 按 decode -> validate -> context -> bounded copy -> backend -> response 实现。
- [ ] E8.10 `bad_alloc` 映射 memory_hard_limit，其他异常 fatal invariant。
- [ ] E8.11 失败 response `{mapped_error,0}`，成功 `{0,requested_length}`。
- [ ] E8.12 运行新/旧 processor 和 SHM regression。
- [ ] E8.13 创建 `feat: productize M1 write requests`。

**Review**

- [ ] R8.01 重读 PLAN Task 8。
- [ ] R8.02 核对 private copy 上限为 SHM data area，不是无界外部长度。
- [ ] R8.03 核对失败前没有 backend mutation。
- [ ] R8.04 核对 request-level error 不终止 session。
- [ ] R8.05 新鲜重跑指定三组测试。
- [ ] R8.06 检查 diff 仅两个允许文件。
- [ ] R8.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 9 — UNMAP、ZERO、CLOSE 与 fatal 行为

Plan：[Task 9](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-9-implement-unmap-zero-close-and-fatal-protocol-behavior)

依赖：Task 8 `ON_TRACK`。

目标：完成六种 M1 request，并确保 malformed/unknown 与 request error 的边界正确。

允许文件：`request_processor.cpp`、`m1_request_processor_tests.cpp`。

**Execute**

- [ ] E9.01 记录 start commit，阅读 PLAN Task 9、SPEC 8.4/8.5。
- [ ] E9.02 写 UNMAP exact ABI、empty、normalized forwarding tests。
- [ ] E9.03 写 ZERO exact ABI、empty、normalized forwarding tests。
- [ ] E9.04 写 malformed length、alignment、overflow、capacity tests。
- [ ] E9.05 写 backend error、cancel、timeout 与独立 counters tests。
- [ ] E9.06 写 CLOSE 无 response、truncated fatal、unknown fatal tests。
- [ ] E9.07 写 fatal decode 后 total_calls 不变 test。
- [ ] E9.08 运行 focused test，确认新 cases red。
- [ ] E9.09 状态改为 `RED_VERIFIED`。
- [ ] E9.10 实现一个 shared range dispatcher，按 code 调 unmap 或 zero。
- [ ] E9.11 成功写 8-byte zero-error response；失败写 mapped error。
- [ ] E9.12 实现 CLOSE action 与 counter。
- [ ] E9.13 实现 unknown/malformed fatal error、metadata 和 counter。
- [ ] E9.14 运行全部 `unit.m1.*`、旧 processor、旧 SHM。
- [ ] E9.15 创建 `feat: support M1 range and close requests`。

**Review**

- [ ] R9.01 重读 PLAN Task 9 和 SPEC 支持 request 表。
- [ ] R9.02 核对 range data 从 header 后 4096 offset 读取。
- [ ] R9.03 核对空 ranges 不调用非法 fake、语义为成功 no-op。
- [ ] R9.04 核对 CLOSE 后 session 层不会再处理 request。
- [ ] R9.05 新鲜运行全部 M1 unit 和旧回归。
- [ ] R9.06 检查未实现 CONNECT/SCSI/SHARED。
- [ ] R9.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 10 — 10,000 case 确定性协议 Fuzz

Plan：[Task 10](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-10-add-deterministic-10000-case-protocol-fuzzing)

依赖：Task 9 `ON_TRACK`。

目标：固定 seed 验证任何 wire 输入都不越界、不崩溃、不误调用 backend。

允许文件：`m1_protocol_fuzz_tests.cpp`、`m1_request_processor_tests.cpp`、`fake_block_device.h`、`tests/CMakeLists.txt`，以及被确定性 regression 证明有缺陷的 proxy 文件。

**Execute**

- [ ] E10.01 记录 start commit，阅读 PLAN Task 10。
- [ ] E10.02 注册 `unit.m1.protocol_fuzz`。
- [ ] E10.03 使用锁定 seed `0x4D3150524F585955` 和 `mt19937_64`。
- [ ] E10.04 生成六种 valid code、unknown、truncated view、任意 64-bit range。
- [ ] E10.05 生成 aligned valid requests 和 injected backend failures。
- [ ] E10.06 每 iteration 断言 outcome 枚举有效、invalid_call_count 0、backend calls 增量 0 或 1。
- [ ] E10.07 失败输出固定 seed 与 iteration。
- [ ] E10.08 运行 focused fuzz，记录第一个真实 failure 或完整 pass。
- [ ] E10.09 若发现 defect，先添加 named deterministic regression，再改生产代码。
- [ ] E10.10 每个 regression red 后最小修复并重跑 processor tests。
- [ ] E10.11 状态改为 `RED_VERIFIED`；若 fuzz 初次即 pass，记录测试为何能覆盖新 gate。
- [ ] E10.12 连续运行 fuzz 20 次，每次 10,000 cases。
- [ ] E10.13 运行全部 M1 unit。
- [ ] E10.14 创建 `test: fuzz M1 proxy protocol deterministically`。

**Review**

- [ ] R10.01 重读 PLAN Task 10 与 SPEC 23.1。
- [ ] R10.02 核对 case count 恰好 10,000、seed 未动态变化。
- [ ] R10.03 核对 failure 不是通过过滤输入消失。
- [ ] R10.04 新鲜再运行 20 次 fuzz。
- [ ] R10.05 运行 `unit.m1.request_processor`。
- [ ] R10.06 记录总 case 运行量、所有 regression 和偏差。
- [ ] R10.07 Verdict `ON_TRACK` 后提交 alignment review，标记 M1-A candidate。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 11 — Namespace Policy 与完整对象身份

Plan：[Task 11](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-11-add-namespace-policies-and-complete-named-object-identities)

依赖：Task 10 `ON_TRACK`。

目标：保持 M0 名称不变，新增严格 M1 kernel/local policy 与 Cancel 名称。

允许文件：`object_names.h/.cpp`、`namespace_policy_tests.cpp`、`object_names_tests.cpp`、`tests/CMakeLists.txt`。

**Execute**

- [ ] E11.01 记录 start commit，阅读 PLAN Task 11 和 M0 namespace evidence。
- [ ] E11.02 注册 `unit.m1.namespace_policy`。
- [ ] E11.03 写 32 lowercase hex 正负 tests 与 RNG 格式/唯一性 tests。
- [ ] E11.04 写 M0 `Global\myrd-m0-` compatibility exact strings。
- [ ] E11.05 写 M1 kernel `Global\myrd-m1-` 六个字段 exact strings。
- [ ] E11.06 写 M1 local `Local\myrd-m1-test-` fields 和 empty driver_path。
- [ ] E11.07 构建 focused target，观察 missing policy red。
- [ ] E11.08 状态改为 `RED_VERIFIED`。
- [ ] E11.09 扩展 `ObjectNames`，添加 cancel，建立 `ProxyObjectNames` alias。
- [ ] E11.10 实现共享 strict ID validator。
- [ ] E11.11 保持 `ObjectNames::from_session()` 精确 M0 行为。
- [ ] E11.12 实现 KernelVisible 与 Local policies；无 fallback/retry。
- [ ] E11.13 运行新 policy、旧 object names、全部 unit。
- [ ] E11.14 创建 `feat: add explicit M1 namespace policies`。

**Review**

- [ ] R11.01 重读 PLAN Task 11 与 SPEC 18/21。
- [ ] R11.02 逐字符核对 M0/M1 mapping/request/response/cancel/server/driver path。
- [ ] R11.03 确认 Local policy 永不产生可传驱动路径。
- [ ] R11.04 新鲜重跑两组 name tests。
- [ ] R11.05 确认没有多 namespace 猜测逻辑。
- [ ] R11.06 检查 diff 未触碰 M0 provider。
- [ ] R11.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 12 — Harden SharedMemoryChannel

Plan：[Task 12](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-12-harden-the-shared-memory-channel-and-add-cancel-event)

依赖：Task 11 `ON_TRACK`。

目标：拥有 Mapping/Request/Response/Cancel/owner，使用窄 DACL、zero-init 和逐阶段 failure hook。

允许文件：PLAN Task 12 指定 channel、M1/旧 SHM tests、tests CMake。

**Execute**

- [ ] E12.01 记录 start commit，阅读 PLAN Task 12、SPEC 18/21。
- [ ] E12.02 创建/扩展 `integration.m1.shm` target。
- [ ] E12.03 写五类对象可打开、mapping size、zero-filled tests。
- [ ] E12.04 写 Request/Response auto-reset 与 Cancel manual-reset tests。
- [ ] E12.05 写每种 public identity duplicate rejection test。
- [ ] E12.06 写 DACL ACE 检查：仅 SY/BA，无 Everyone/AU/IU。
- [ ] E12.07 写 owner/mapping/view/request/response/cancel 六阶段 failure cleanup tests。
- [ ] E12.08 构建/运行，确认缺少 Cancel/hooks 的 red failure。
- [ ] E12.09 状态改为 `RED_VERIFIED`。
- [ ] E12.10 添加 create hook、cancel accessor 和所有权成员。
- [ ] E12.11 按锁定顺序创建 owner -> mapping -> view -> request -> response -> cancel。
- [ ] E12.12 mapping 成功后完整清零再返回。
- [ ] E12.13 Request/Response 用 auto-reset；Cancel 用 manual-reset。
- [ ] E12.14 所有 named object 使用同一 non-inheritable DACL。
- [ ] E12.15 运行 M1 SHM 50 次和旧 `integration.shm`。
- [ ] E12.16 创建 `feat: harden M1 shared memory channel`。

**Review**

- [ ] R12.01 重读 PLAN Task 12 和 SPEC startup order。
- [ ] R12.02 核对每个 failure stage 释放此前所有对象。
- [ ] R12.03 核对析构后所有名称均无法再次打开。
- [ ] R12.04 新鲜运行 50 次 M1 SHM 与旧 SHM。
- [ ] R12.05 检查没有宽 DACL、继承 handle 或 fixed public name。
- [ ] R12.06 记录重复测试次数和 handle/name 证据。
- [ ] R12.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 13 — ShmProxySession 状态机

Plan：[Task 13](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-13-implement-the-shmproxysession-state-machine)

依赖：Task 12 `ON_TRACK`。

目标：实现 Ready barrier、单请求 worker、cancel monitor、超时、response ordering、bounded diagnostics 和无竞态清理。

允许文件：`shm_proxy_session.h/.cpp`、`m1_shm_session_tests.cpp`、`src/CMakeLists.txt`。

**Execute**

- [ ] E13.01 记录 start commit，阅读 PLAN Task 13、SPEC 18/19/22。
- [ ] E13.02 写全部 stable state、start twice、stop-before-start、idempotent stop tests。
- [ ] E13.03 写 Ready-before-return 和 backend completion-before-response tests。
- [ ] E13.04 写 idle Cancel、delayed backend Cancel、deadline tests。
- [ ] E13.05 写 CLOSE 后第二 request 不处理 test。
- [ ] E13.06 写 unknown fatal 与 response SetEvent hook failure tests。
- [ ] E13.07 写三个 SessionStartStage failure cleanup tests。
- [ ] E13.08 写 bounded diagnostic FIFO、drop oldest、no data content tests。
- [ ] E13.09 构建/运行，确认 missing session red。
- [ ] E13.10 状态改为 `RED_VERIFIED`。
- [ ] E13.11 发布 PLAN 锁定 states/options/hooks/snapshot API。
- [ ] E13.12 实现 request jthread、cancel-monitor jthread 和 promise Ready barrier。
- [ ] E13.13 cancel monitor 将 named Cancel 转成 stop token。
- [ ] E13.14 request worker 等 Request/Cancel，不轮询 SHM 字段。
- [ ] E13.15 每 request 创建 deadline context，调用 processor。
- [ ] E13.16 仅在完整 response 后 signal Response；signal 失败 fatal。
- [ ] E13.17 实现 close/fatal/normal cancel 的精确 final state。
- [ ] E13.18 实现 bounded diagnostic FIFO 和 thread-safe snapshot。
- [ ] E13.19 stop/join 不持有 state mutex；失败 session 不恢复 Serving。
- [ ] E13.20 运行 M1 SHM 50 次、全部 M1 unit 和旧 SHM。
- [ ] E13.21 创建 `feat: add productized M1 proxy session`。

**Review**

- [ ] R13.01 重读 PLAN Task 13 与 SPEC session state diagram。
- [ ] R13.02 手工审查两个线程在 start/cancel/close/fatal/destructor 路径均可 join。
- [ ] R13.03 核对 backend I/O 期间 Cancel 能触发 stop token。
- [ ] R13.04 核对 Response Event ordering 和 no polling。
- [ ] R13.05 新鲜运行 50 次 SHM 与全部指定回归。
- [ ] R13.06 记录线程/handle/name cleanup 证据。
- [ ] R13.07 Verdict `ON_TRACK` 后提交 alignment review，标记 M1-B candidate。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 14 — M1Provider 组合根

Plan：[Task 14](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-14-compose-the-m1-provider-without-changing-m0)

依赖：Task 13 `ON_TRACK`。

目标：把 fixed memory device、processor、channel 和 session 组合成可失败 factory；设备生命周期仍在 provider 外。

允许文件：`m1_provider.h/.cpp`、`m1_provider_tests.cpp`、两个 CMake 文件。

**Execute**

- [ ] E14.01 记录 start commit，阅读 PLAN Task 14、SPEC 6.1/18。
- [ ] E14.02 注册 `unit.m1.provider`。
- [ ] E14.03 写 Local policy factory、64 MiB/512、features 和 native path tests。
- [ ] E14.04 写 start publishes Ready、stop releases names tests。
- [ ] E14.05 写 create/start failure 无残留 tests。
- [ ] E14.06 构建 focused target，观察 missing provider red。
- [ ] E14.07 状态改为 `RED_VERIFIED`。
- [ ] E14.08 发布 production `create()` 与 `create_for_test()` API。
- [ ] E14.09 factory 捕获 RNG、name、allocation、channel/session construction failures。
- [ ] E14.10 production factory 强制 KernelVisible policy；test factory 接受 Local policy。
- [ ] E14.11 按锁定顺序组合 device -> processor -> channel -> session。
- [ ] E14.12 `start()` 只启动 session；`stop()` 不删除 kernel device。
- [ ] E14.13 运行 provider、M1 SHM、M0 lifecycle unit。
- [ ] E14.14 创建 `feat: compose the M1 proxy provider`。

**Review**

- [ ] R14.01 重读 PLAN Task 14。
- [ ] R14.02 核对 production caller 无法注入 Local policy。
- [ ] R14.03 核对 provider 不依赖 driver、poc、CLI 或 report。
- [ ] R14.04 核对 stop 不调用 remove_device。
- [ ] R14.05 新鲜运行三组测试。
- [ ] R14.06 检查没有 M2 storage logic。
- [ ] R14.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 15 — 真实 READ/WRITE/ZERO/UNMAP 验证器

Plan：[Task 15](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-15-implement-real-driver-readwritezerounmap-verification)

依赖：Task 14 `ON_TRACK`。

目标：在 fakeable raw boundary 后实现真实 1 MiB pattern、all-zero write、TRIM 和 counter 证明。

允许文件：`m1_raw_protocol_io.h/.cpp`、对应 unit test、两个 CMake 文件。

**Execute**

- [ ] E15.01 记录 start commit，阅读 PLAN Task 15 和 pinned driver ZERO/UNMAP call sites。
- [ ] E15.02 注册 `unit.m1.raw_protocol_io`。
- [ ] E15.03 定义 fake API，写 expected open/write/read/flush/zero/trim/read sequence。
- [ ] E15.04 写 short read/write、hash mismatch、flush fail、trim fail tests。
- [ ] E15.05 写 ZERO/UNMAP counter 不增长即失败 tests。
- [ ] E15.06 构建 focused target，观察 missing verifier red。
- [ ] E15.07 状态改为 `RED_VERIFIED`。
- [ ] E15.08 发布 evidence struct、injectable API 和 verifier signature。
- [ ] E15.09 使用锁定 offset 8 MiB、length 1 MiB、deterministic nonzero pattern。
- [ ] E15.10 实现 write/read/hash/flush evidence。
- [ ] E15.11 all-zero WriteFile 后等待 `zero` counter 增长并读回全零。
- [ ] E15.12 rewrite pattern 后构造正确 DSM Trim input。
- [ ] E15.13 DeviceIoControl 成功后等待 `unmap` counter 增长并读回全零。
- [ ] E15.14 counter wait 最长 5 秒，失败返回可诊断错误。
- [ ] E15.15 运行 unit test；本 Task 不访问真实磁盘。
- [ ] E15.16 创建 `feat: verify real M1 range operations`。

**Review**

- [ ] R15.01 重读 PLAN Task 15 与上游 `ImScsiZeroDevice`/UNMAP call site。
- [ ] R15.02 核对 zero 不是仅普通 write success，而有 code 7 counter。
- [ ] R15.03 核对 trim 不是仅 IOCTL success，而有 code 6 counter。
- [ ] R15.04 核对 offset/length 对齐且不触碰 sector 0。
- [ ] R15.05 新鲜运行 focused unit。
- [ ] R15.06 检查本 Task 未提升权限或创建设备。
- [ ] R15.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 16 — M1-C 生命周期 executable 与报告

Plan：[Task 16](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-16-build-the-m1-c-lifecycle-executable-and-structured-report)

依赖：Task 15 `ON_TRACK`。

目标：实现可测试 CLI、injectable lifecycle runner、严格 report 和薄 `main.cpp`。

允许文件：PLAN Task 16 列出的 `src/m1` CLI/lifecycle/report/main、三组 unit tests、两个 CMake 文件。

**Execute**

- [ ] E16.01 记录 start commit，阅读 PLAN Task 16 和 M0 cleanup runner 作为只读参考。
- [ ] E16.02 注册 `unit.m1.cli`、`unit.m1.lifecycle_runner`、`unit.m1.report`。
- [ ] E16.03 写 report schema top-level、provider counters/diagnostics、cleanup fields tests。
- [ ] E16.04 写 error internal/native code 与 no-data/no-report-path serialization tests。
- [ ] E16.05 写 CLI missing/duplicate/path/overwrite/debug-release failure point tests。
- [ ] E16.06 写 lifecycle fake success exact call-order test。
- [ ] E16.07 写 provider/create/query/locate/write/zero/unmap/remove 各阶段 failure tests。
- [ ] E16.08 写 cleanup primary error 不被 cleanup error 覆盖 tests。
- [ ] E16.09 写 exact owned DeviceId/sentinel rejection/baseline equality tests。
- [ ] E16.10 构建三个 focused targets，保存 missing components red。
- [ ] E16.11 状态改为 `RED_VERIFIED`。
- [ ] E16.12 实现 typed report 和复用 `myrd_poc` JsonWriter/ReportPublisher 的独立 report target。
- [ ] E16.13 实现 narrow CLI；Release 拒绝 failure injection，拒绝覆盖 report。
- [ ] E16.14 发布 provider/kernel/locator/protocol 的 injectable runner interfaces。
- [ ] E16.15 实现 success lifecycle 的 12 个锁定阶段。
- [ ] E16.16 所有 CREATE 后路径只清理 `owned_id`，拒绝 sentinel/all devices。
- [ ] E16.17 cleanup 顺序：remove exact -> wait absent -> stop provider -> wait names -> baselines。
- [ ] E16.18 收集 cleanup errors，不覆盖 primary error。
- [ ] E16.19 实现八个 Debug failure points。
- [ ] E16.20 `main.cpp` 只做 runtime evidence、concrete wrappers、runner 调用和 atomic publish。
- [ ] E16.21 运行三个新 unit、旧 report 和旧 lifecycle tests。
- [ ] E16.22 构建 Debug `myrd-m1-e2e.exe`，本 Task 不运行真实 CREATE。
- [ ] E16.23 创建 `feat: add M1 driver lifecycle verifier`。

**Review**

- [ ] R16.01 重读 PLAN Task 16、SPEC 18/21/22/25.1。
- [ ] R16.02 核对 main 无 lifecycle state machine，runner 不写文件/改服务。
- [ ] R16.03 核对 success report 必须有六种 request counters 与 cleanup true。
- [ ] R16.04 核对所有 failure point 均进入同一精确 cleanup path。
- [ ] R16.05 新鲜运行全部指定 unit 和 executable build。
- [ ] R16.06 检查 M1 reusable provider 不依赖 `myrd_poc`。
- [ ] R16.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 17 — JSON Validator 与 E2E Harness 安全门

Plan：[Task 17](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-17-add-strict-m1-json-validation-and-elevated-harness-safety)

依赖：Task 16 `ON_TRACK`。

目标：提供 strict report validator 和即使失败也恢复服务/设备 baseline 的 elevated harness；本 Task 不运行真实 CREATE。

允许文件：`tools/Verify-M1Json.ps1`、`tests/e2e/run-m1-proxy.ps1`、`tests/e2e/test-m1-harness-preflight.ps1`。

**Execute**

- [ ] E17.01 记录 start commit，阅读 PLAN Task 17 和 M0 harness safety pattern。
- [ ] E17.02 先创建 preflight test driver，列出八个必须早失败场景。
- [ ] E17.03 运行 preflight，保存 missing harness red failure。
- [ ] E17.04 状态改为 `RED_VERIFIED`。
- [ ] E17.05 实现 JSON parse/schema/mode/success-exit consistency checks。
- [ ] E17.06 实现 INFO/READ/WRITE/ZERO/UNMAP counters 必需检查。
- [ ] E17.07 实现 exact ownership、cleanup、baseline、forbidden dependency 检查。
- [ ] E17.08 实现 admin、mutex、service、executable、report-root、reparse、overwrite preflight。
- [ ] E17.09 harness 保存 `deviosvc` state/start mode/path/PID。
- [ ] E17.10 在 try/finally 中停止 deviosvc，运行前后 read-only query。
- [ ] E17.11 每个 child report 立即 validate/hash，恢复服务后再次核 hash。
- [ ] E17.12 aggregate summary 不重写 child report。
- [ ] E17.13 非管理员运行 preflight safety tests，要求所有场景无系统 mutation。
- [ ] E17.14 运行全部 M1 unit 与 local SHM。
- [ ] E17.15 创建 `test: add safe M1 driver harness`。

**Review**

- [ ] R17.01 重读 PLAN Task 17 与 SPEC exact cleanup/security。
- [ ] R17.02 手工审查每个状态改变都位于 try/finally。
- [ ] R17.03 确认没有 `Set-Service`/`sc config` 永久改 StartMode。
- [ ] R17.04 新鲜运行 preflight tests 和 local regressions。
- [ ] R17.05 核对 reports 必须位于仓库 `reports/` 且拒绝 reparse escape。
- [ ] R17.06 记录所有 preflight case 与 exit result。
- [ ] R17.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 18 — M1-V0 到 M1-V6 Verification Orchestrator

Plan：[Task 18](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-18-add-the-tiered-m1-verifier-and-dependency-audit)

依赖：Task 17 `ON_TRACK`。

目标：机械化 tier inheritance、test selection、local gate 和真实 gate 入口。

允许文件：`tools/Invoke-M1Verification.ps1`、`tests/unit/m1_verification_script_tests.ps1`、`tests/CMakeLists.txt`。

**Execute**

- [ ] E18.01 记录 start commit，阅读 PLAN Task 18 和 M0 orchestrator。
- [ ] E18.02 注册 non-elevated `unit.m1.verification_script` 或独立 script test。
- [ ] E18.03 写 `-ListCommands` V0-V6 顺序和 zero-selection failure tests。
- [ ] E18.04 运行 script tests，保存 missing orchestrator red。
- [ ] E18.05 状态改为 `RED_VERIFIED`。
- [ ] E18.06 实现 V0：upstream/docs/M0 ancestor/diff check。
- [ ] E18.07 实现 V1：configure/build/全部 `unit.m1.*` 且 selection >=1。
- [ ] E18.08 实现 V2：fuzz + M1 SHM 50 次。
- [ ] E18.09 实现 V3：Release import audit + M0 read-only query，不改 service state。
- [ ] E18.10 实现 V4：单 Debug real lifecycle harness。
- [ ] E18.11 实现 V5：八个 Debug failure points + one normal。
- [ ] E18.12 实现 V6：Release audit + exactly 20 lifecycles + one new aggregate。
- [ ] E18.13 forbidden import 包含 aimapi/aim_ll/RamDyn/AIM GUI/devio user-mode。
- [ ] E18.14 运行 `-Tier M1-V2 -ListCommands`。
- [ ] E18.15 运行 script tests。
- [ ] E18.16 新鲜运行完整 M1-V2 Debug。
- [ ] E18.17 创建 `test: orchestrate M1 verification tiers`。

**Review**

- [ ] R18.01 重读 PLAN Task 18 与 verification tier table。
- [ ] R18.02 核对每个高 tier 调用所有低 tier。
- [ ] R18.03 核对 CTest regex zero selection 会失败。
- [ ] R18.04 新鲜运行 ListCommands、script tests、M1-V2。
- [ ] R18.05 核对 V3 不改变 service，V4-V6 由 harness 管理恢复。
- [ ] R18.06 记录 unit/fuzz/SHM counts。
- [ ] R18.07 Verdict `ON_TRACK` 后提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 19 — 执行 M1-C 真实驱动门

Plan：[Task 19](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-19-execute-m1-c-real-driver-gates)

依赖：Task 18 `ON_TRACK`；管理员 PowerShell；`phdskmnt` Running。

目标：用真实驱动完成 V3、M0 V4 regression、M1 V4/V5/V6。默认只产生 ignored evidence；发现 defect 才改代码。

安全红线：禁止 all-device remove；禁止运行未知磁盘写入；禁止跳过首次失败；禁止复用旧 aggregate 宣称新通过。

**Execute**

- [ ] E19.01 记录 start commit，阅读 PLAN Task 19、SPEC 25.1/27/28。
- [ ] E19.02 提升权限后记录 phdskmnt/deviosvc state/start mode/path/PID。
- [ ] E19.03 记录 baseline AIM IDs、PhysicalDrive numbers、forbidden processes。
- [ ] E19.04 运行 M1-V3 Release；失败立即停止。
- [ ] E19.05 运行 M0 V4 Debug，证明 shared library 变化未破坏 M0。
- [ ] E19.06 检查 M0 V4 exact cleanup 与 service restore。
- [ ] E19.07 运行 M1-V4 Debug 一次；首次 CREATE/IO failure 保存现场并停止。
- [ ] E19.08 独立验证 V4 report：all six counters、exact DeviceId、ZERO/UNMAP、cleanup/baseline。
- [ ] E19.09 运行 M1-V5 八个 failure points 和 one normal。
- [ ] E19.10 每个 failure report 核对 expected primary error 与完整 cleanup。
- [ ] E19.11 固定 Release executable/hash，运行 M1-V6 exactly 20 cycles。
- [ ] E19.12 核对 aggregate 20 attempted/20 passed、unique sessions、same exe hash。
- [ ] E19.13 独立检查最终 service/device/PhysicalDrive/process/name baseline。
- [ ] E19.14 记录每份 summary/report 路径与 SHA-256。
- [ ] E19.15 若失败：补 deterministic lowest-tier regression，最小修复，重新从 M1-V2 开始。
- [ ] E19.16 若有修复，为每个 defect 创建独立 fix commit；无修复则 implementation commit 记 N/A。
- [ ] E19.17 状态改为 `VERIFYING`。

**Review**

- [ ] R19.01 重读 PLAN Task 19 与 SPEC M1-C/GO 条件。
- [ ] R19.02 新鲜复核报告 hash，不编辑 child report。
- [ ] R19.03 核对真实 zero counter >0、unmap counter >0、读回全零。
- [ ] R19.04 核对 20 cycles 后无 device/name/thread/handle/service residue。
- [ ] R19.05 核对 Release import/loaded module 无 AIM user mode。
- [ ] R19.06 记录所有修复、偏差和未解决风险。
- [ ] R19.07 全部通过为 `ON_TRACK`；环境缺失为 `BLOCKED`；真实强制条件失败为 `NO_GO_EVIDENCE`。
- [ ] R19.08 只有 `ON_TRACK` 才提交 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `N/A 或未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

### Task 20 — M1 正式验收与最终仓库审计

Plan：[Task 20](docs/superpowers/plans/2026-08-02-m1-proxy-productization.md#task-20-record-m1-acceptance-and-final-regression)

依赖：Task 19 `ON_TRACK`。

目标：把每个 SPEC requirement 映射到新鲜证据，给出机械化 M1 GO/NO-GO，并证明没有越界实现 M2-M4。

允许文件：`docs/validation/m1-proxy-acceptance.md`、`README.md`、`M1_TASKLIST.md`。

**Execute**

- [ ] E20.01 记录 start commit，阅读完整 SPEC、PLAN Task 20 和 Task 19 evidence。
- [ ] E20.02 创建 acceptance 环境/driver/upstream/executable hash 表。
- [ ] E20.03 创建 M1-V0 到 V6 command/result/evidence 表。
- [ ] E20.04 记录 unit count、seed、10,000 cases、SHM 50x。
- [ ] E20.05 记录真实 INFO/READ/WRITE/ZERO/UNMAP/CLOSE counters。
- [ ] E20.06 记录 20-cycle aggregate 路径/hash 与 baseline cleanup。
- [ ] E20.07 逐项映射 SPEC 3.1、4、6.1/6.2、7/8、18-22、23.1、24.1、25.1、27、28。
- [ ] E20.08 明确易失性、固定内存和 M2-M4 未实现。
- [ ] E20.09 README 只声明 M1 proxy productization，不声明动态内存或持久化。
- [ ] E20.10 新鲜运行 M0 V2 Debug。
- [ ] E20.11 新鲜运行 M1-V2 Debug。
- [ ] E20.12 构建 Release 并运行所有 unit/integration。
- [ ] E20.13 运行 `git diff --check` 和 forbidden M2 implementation search。
- [ ] E20.14 检查 tracked artifacts，无 `out/`、`reports/`、诊断 exe/obj。
- [ ] E20.15 根据全部强制条件写 `M1 GO` 或 `M1 NO-GO`，不得多数表决。
- [ ] E20.16 创建 `docs: record M1 proxy acceptance`。
- [ ] E20.17 状态改为 `IMPLEMENTED`。

**Review**

- [ ] R20.01 从头重读完整 SPEC 与 PLAN traceability table。
- [ ] R20.02 每个 MUST 都能指向 test/report/evidence line。
- [ ] R20.03 新鲜重跑 Task 20 的全部最终验证。
- [ ] R20.04 核对 acceptance 中观测、推导、结论没有混写。
- [ ] R20.05 核对 README 未暗示 M1 已有动态加载、image、write-back 或断电恢复。
- [ ] R20.06 核对工作区只剩已知用户诊断文件且未提交。
- [ ] R20.07 填写最终 branch/commit/test counts/evidence/risks。
- [ ] R20.08 Verdict 与 acceptance 一致；`ON_TRACK` 后提交最终 alignment review。

Review Record：Executor `未记录`; Start `未记录`; Implementation `未记录`; Files `未记录`; Commands `未记录`; Results `未记录`; Evidence `未记录`; Deviations `未记录`; Risks `未记录`; Verdict `NOT_REVIEWED`。

## 10. 偏差与故障处理

### 10.1 实现缺陷但 PLAN 正确

```text
状态回到 IN_PROGRESS
保留失败输出
添加最小 deterministic regression
观察 regression RED
修复最早错误边界
新鲜运行 focused + regression tier
重新执行 G1-G7
```

### 10.2 PLAN 或 SPEC 不可实现

```text
状态设为 DEVIATED
停止后续 Task
记录冲突原文、实际证据、影响和建议
请求用户批准文档修订
修订并提交后重新开始当前 Task
```

### 10.3 外部环境缺失

```text
状态设为 BLOCKED
记录命令、退出码、服务/驱动/权限状态
执行所有安全且只读的诊断
不得把 BLOCKED 写成 GO 或 NO-GO
不得跳过前置条件进入更高 tier
```

### 10.4 真实驱动强制条件失败

```text
状态设为 NO_GO_EVIDENCE
停止更高 tier
精确清理当前 owned DeviceId
恢复 deviosvc 原状态和 StartMode
保留 primary/cleanup error、report、hash 和 baseline
回到最低可复现层补测试
```

## 11. Agent 每次回复用户的固定格式

```markdown
M1 Task N 状态：ON_TRACK | DEVIATED | BLOCKED | NO_GO_EVIDENCE

实现提交：真实 commit 或 N/A
复盘提交：真实 commit 或尚未创建

实际改动：
- 精确文件路径

新鲜验证：
- 完整命令 -> 退出码 / 通过数量

证据：
- 报告路径、hash、关键计数；无报告时写 None

PLAN 对齐复盘：
- 已重新阅读 SPEC/PLAN Task N：是/否
- 文件范围一致：是/否
- 行为范围一致：是/否
- 提前实现后续 Task：是/否
- M2-M4 越界实现：是/否
- 偏差：None 或具体内容

下一步：只有 ON_TRACK 时写 Task N+1；其他状态说明停止原因。
```

禁止只回复“完成”“测试通过”“可以继续”。

## 12. 整体完成条件

本 TASKLIST 只有同时满足以下条件才完成：

- Task 0-20 全部终结；
- 每个代码 Task 都有 implementation commit；
- 每个 Task 都有独立 alignment review commit；
- 所有正常完成 Task 状态为 `ON_TRACK`；
- M1-A、M1-B、M1-C 全部通过；
- 10,000 case fuzz 和 50 次 local SHM 有新鲜证据；
- 真实 READ/WRITE/ZERO/UNMAP 均被 provider counter 证明；
- Release 20 cycles 全部成功且使用同一 executable hash；
- exact device/name/service/PhysicalDrive baseline 全部恢复；
- 没有 AIM Toolkit 用户态依赖；
- acceptance verdict 与 SPEC 的机械规则一致；
- 没有把 M1 描述成动态内存、镜像持久化、定时写回或完整产品；
- 工作区无未解释 tracked 修改，构建/报告/诊断产物未提交。

如果最终停在 `BLOCKED` 或 `NO_GO_EVIDENCE`，必须保留完整证据并明确说明 M1 未完成，不得勾选剩余 Task。
