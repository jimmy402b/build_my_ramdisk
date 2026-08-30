# MyRamDisk M4 重设计执行台账

> 本台账执行已批准的 `docs/superpowers/specs/2026-08-30-m4-single-and-multi-disk-redesign-design.md` 与实施计划 `docs/superpowers/plans/2026-08-30-m4-single-and-multi-disk-redesign.md`。每次只允许一个任务为 `IN_PROGRESS`；每项必须先 RED、再 GREEN、再独立复盘。

## 状态定义

```text
NOT_STARTED -> IN_PROGRESS -> IMPLEMENTED -> REVIEWING -> ON_TRACK
                                  |                 |
                                  +-> DEVIATED      +-> BLOCKED
```

`DEVIATED`/ `BLOCKED`/ `NO_GO_EVIDENCE` 停止后续任务，先更新 spec/plan。Fake、自测和本地测试不能替代真实 VM 资格证据。任何实现任务不得直接修改 M3 journal、manifest、VHDX 或 recovery 算法。

## 固定执行规则

- 实现分支：`feature/m4-single-multi-disk`，基于 GitHub `main@df09c4f0ac310b00e5b53be594c4bfba6730aaec`。
- 所有实现工作在 `.worktrees/m4-single-multi-disk`；当前 `master` 只保存文档。
- 子代理不并行修改共享实现工作树；每项实现后必须先做 spec review，再做 code-quality review。
- 测试顺序：最小 RED → 精确目标 GREEN → 受影响 M3 标签 → 必要时 Debug/Release。
- 每项提交只包含台账列出的文件；提交前运行 `git diff --check`。
- 不得使用 `vmrun`、SSH、SCP、脚本 shutdown/reboot、`pnputil /restart-device` 或按名称 kill/删除未知设备。

## 总进度

| ID | 状态 | 任务 | 主要产物 |
|---|---|---|---|
| T00 | NOT_STARTED | 固定远端 M3 基线和 worktree | `docs/validation/m4-baseline.md` |
| T01 | NOT_STARTED | CLI RED 合同 | `tests/unit/m4_cli_tests.cpp` |
| T02 | NOT_STARTED | CLI 实现 | `src/run/cli.*` |
| T03 | NOT_STARTED | provisioning RED/脚本合同 | `tests/unit/test-m4-provision-script.ps1` |
| T04 | NOT_STARTED | Windows volume 纯策略和 seam | `tests/unit/m4_windows_volume_tests.cpp` |
| T05 | NOT_STARTED | Windows volume Win32 实现 | `src/run/windows_volume.*` |
| T06 | NOT_STARTED | DiskRuntime 生命周期 RED/接口 | `tests/unit/m4_disk_runtime_tests.cpp` |
| T07 | NOT_STARTED | DiskRuntime 创建路径 | `src/run/disk_runtime.*` |
| T08 | NOT_STARTED | DiskRuntime 恢复路径 | `src/run/disk_runtime.*` |
| T09 | NOT_STARTED | DiskRuntime 归属清理与 lock | `src/run/disk_runtime.*` |
| T10 | NOT_STARTED | checkpoint 策略 RED | `tests/unit/m4_run_policy_tests.cpp` |
| T11 | NOT_STARTED | 自动 checkpoint 实现 | `src/run/run_loop.*` |
| T12 | NOT_STARTED | Ctrl+C 安全关闭实现 | `src/run/run_loop.*` |
| T13 | NOT_STARTED | 单一 CLI 组合入口 | `src/run/main.cpp` |
| T14 | NOT_STARTED | 单盘组合集成测试 | `tests/integration/m4_product_composition_tests.cpp` |
| T15 | NOT_STARTED | 多实例隔离测试 | `tests/integration/m4_multi_instance_tests.cpp` |
| T16 | NOT_STARTED | 多盘 CLI 使用文档 | `README.md` |
| T17 | NOT_STARTED | Debug 本地验证 | `docs/validation/m4-local-verification.md` |
| T18 | NOT_STARTED | Release 本地验证 | `docs/validation/m4-local-verification.md` |
| T19 | NOT_STARTED | 单盘 VM 观察脚本和 runbook | `tests/e2e/m4_vm_lifecycle.ps1` |
| T20 | NOT_STARTED | 单盘 VM 创建/checkpoint/恢复 | acceptance evidence |
| T21 | NOT_STARTED | 双盘 VM 并行/隔离 | `tests/e2e/m4_vm_multi_disk.ps1` |
| T22 | NOT_STARTED | 双盘 VM 断电恢复 | acceptance evidence |
| T23 | NOT_STARTED | VM 负面和完整性验收 | acceptance evidence |
| T24 | NOT_STARTED | 最终 M4 资格报告 | `docs/validation/m4-single-multi-disk-acceptance.md` |

## T00 — 固定远端 M3 基线和 worktree

**Files:** Create `docs/validation/m4-baseline.md`; no source changes.

**RED/GREEN:** 验证远端完整 SHA 为 `df09c4f0ac310b00e5b53be594c4bfba6730aaec`，记录 M3 README 的验收声明和 M3 证据路径；Debug/Release 基线构建与 CTest 结果逐条记录。基线已有失败必须停止并分类。

**Commands:**
```powershell
git fetch https://github.com/jimmy402b/build_my_ramdisk.git main
git worktree add .worktrees/m4-single-multi-disk -b feature/m4-single-multi-disk df09c4f0ac310b00e5b53be594c4bfba6730aaec
cmake --preset msvc
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel 1
ctest --preset release --output-on-failure
```

提交：`docs(m4): freeze github m3 baseline for redesign`。

## T01 — CLI RED 合同

**Files:** Create `tests/unit/m4_cli_tests.cpp`; register `unit.m4.cli` in `tests/CMakeLists.txt`。

先写失败测试，覆盖 `run`/ `compact`/help、缺失/重复/未知选项、设备路径拒绝、盘符规范化、64M～1T 大小、checkpoint/ram-limit 范围和整数溢出。测试不能访问文件系统、SCM、AIM 或卷。

**RED command:** `ctest --preset debug -R '^unit\\.m4\\.cli$' --output-on-failure`；预期目标或 parser 缺失。

提交：`test(m4): specify cli parser contract`。

## T02 — CLI 实现

**Files:** Create `src/run/cli.h`, `src/run/cli.cpp`；modify `src/CMakeLists.txt`。

实现 `RunOptions`、`CompactOptions`、`CliAction` 和 `parse_cli(span<wstring_view>)`。解析器只验证输入，不创建目录、不读取 manifest、不访问 Windows。运行 T01 全部 GREEN、`git diff --check` 后提交：`feat(m4): implement cli parser`。

## T03 — provisioning RED 与脚本合同

**Files:** Create `tests/unit/test-m4-provision-script.ps1`；Create `src/run/provision_volume.ps1` 的失败合同测试；modify `tests/CMakeLists.txt`。

测试固定脚本只接受 `-DiskNumber`/ `-DriveLetter`，拒绝已有文件系统、多分区、盘符占用、越界输入，并要求固定结果字段和 CRLF。先通过 fake command resolver 看到 RED，再提交：`test(m4): specify safe volume provisioning contract`。

## T04 — volume 纯策略和 seam RED

**Files:** Create `tests/unit/m4_windows_volume_tests.cpp`；Create `src/run/windows_volume.h`；modify `tests/CMakeLists.txt`。

冻结 `ProductVolume`、`VolumeLockAttempt` 及 provision/find/flush/lock/assign/remove API。用 fake observation 覆盖唯一 extents、foreign mount、盘符占用、timeout、flush 失败、busy lock 和 cleanup 失败；断言 foreign 资源不删除。预期因实现缺失 RED。提交：`test(m4): specify exact volume ownership boundaries`。

## T05 — Windows volume 实现

**Files:** Create `src/run/windows_volume.cpp`, `src/run/run_resources.rc`；complete `src/run/provision_volume.ps1`；modify `src/CMakeLists.txt`。

实现精确 extents 校验、FlushFileBuffers、卷锁定、盘符归属、嵌入 provisioning 子进程、有界输出和超时。失败只回滚本次拥有 mount，不删除 bundle。运行 T03/T04 及相关 Windows volume 测试到 GREEN，提交：`feat(m4): implement exact windows volume lifecycle`。

## T06 — DiskRuntime 生命周期 RED/接口

**Files:** Create `tests/unit/m4_disk_runtime_tests.cpp`；Create `src/run/disk_runtime.h`；modify `tests/CMakeLists.txt`。

冻结 `DiskRuntime::create/recover/start/maintenance_stats/checkpoint/shutdown_clean`。用 fake M3/volume/filesystem 断言 create 顺序、recover 不 provisioning、unknown nonempty 不启动 AIM、size mismatch 不挂载、第二实例 lock 拒绝。先运行 RED。提交：`test(m4): specify disk runtime lifecycle`。

## T07 — DiskRuntime 创建路径

**Files:** Create/modify `src/run/disk_runtime.cpp`。

只组合已有 M3 `create_generation_zero_image`、`M3AimRuntime` 和 T05 volume API；创建 GPT/NTFS 后 flush，再提交 generation 1 才返回 Ready。不得复制持久化格式代码。运行 T06 focused GREEN，提交：`feat(m4): implement disk runtime creation`。

## T08 — DiskRuntime 恢复路径

**Files:** Modify `src/run/disk_runtime.cpp`, `tests/unit/m4_disk_runtime_tests.cpp`。

恢复只调用 M3 `recover_complete_image`、AIM runtime、`find_recovered_volume` 和盘符分配；请求 geometry 必须匹配；绝不调用 provisioning。增加 recovery-after-checkpoint fake 测试并运行 GREEN，提交：`feat(m4): implement disk runtime recovery`。

## T09 — DiskRuntime 归属清理与 lock

**Files:** Modify `src/run/disk_runtime.*`, `tests/unit/m4_disk_runtime_tests.cpp`。

增加本次拥有资源 ledger：AIM device、volume、drive letter、runtime。所有失败按反向顺序清理；foreign identity、第二 writer lock、dismount/remove 失败都返回非零且保留 bundle。运行 T06～T08 全部测试，提交：`fix(m4): harden disk runtime ownership cleanup`。

## T10 — checkpoint 策略 RED

**Files:** Create `tests/unit/m4_run_policy_tests.cpp`；Create `src/run/run_loop.h`；modify `tests/CMakeLists.txt`。

写失败测试覆盖 due 条件、Committed/NotCommitted/CommitIndeterminate/JournalCapacityExhausted、flush-before-checkpoint、busy volume 和 final checkpoint 顺序。测试使用 fake clock/runtime/volume，不能 sleep 或访问真实设备。提交：`test(m4): specify checkpoint and shutdown policy`。

## T11 — 自动 checkpoint 实现

**Files:** Create `src/run/run_loop.cpp`；modify `src/run/disk_runtime.*` only for declared calls。

每秒读取 stats；时间或 dirty threshold 触发；先卷 flush 再 M3 checkpoint；NotCommitted 最多重试一次；CommitIndeterminate 禁止盲目重试；journal 满时停止新 checkpoint并返回非零。运行 T10 GREEN，提交：`feat(m4): schedule durable checkpoints`。

## T12 — Ctrl+C 安全关闭

**Files:** Modify `src/run/run_loop.cpp`, `tests/unit/m4_run_policy_tests.cpp`。

实现 lock → verify → flush → final checkpoint → dismount → remove letter → stop。busy 时保持运行并提示用户重试；清理失败不删除 bundle。增加 exact order 和 failure tests，运行 GREEN，提交：`feat(m4): implement owned clean shutdown`。

## T13 — 单一 CLI 组合入口

**Files:** Create/replace `src/run/main.cpp`；modify `src/CMakeLists.txt`。

`wmain` 只做 parser、提权/服务前置检查、bundle 分类、`DiskRuntime::create/recover`、console handler、run loop 和已有 M3 compact 路由。driver unavailable 时不得访问 bundle/AIM/盘符。提交：`feat(m4): compose foreground cli product`。

## T14 — 单盘组合集成测试

**Files:** Create `tests/integration/m4_product_composition_tests.cpp`；modify `tests/CMakeLists.txt`。

通过 seam 验证 help/error stdout-stderr、提权优先级、create/recover 单入口、zero-write、退出码和 cleanup。运行 Debug M4 focused 与所有受影响 M3 标签，提交：`test(m4): verify single-disk product composition`。

## T15 — 多实例隔离测试

**Files:** Create `tests/integration/m4_multi_instance_tests.cpp`；modify `tests/CMakeLists.txt`。

用两个 fake/native child 验证 A/R: 与 B/S: 并行；generation、bundle、AIM、盘符和 cleanup ledger 独立；A 失败/关闭不影响 B；同 bundle 第二实例被 lock 拒绝。提交：`test(m4): prove multi-instance isolation`。

## T16 — 多盘 CLI 使用文档

**Files:** Modify `README.md`。

增加两个独立 `myrd run` 命令、单进程单盘边界、同 bundle 禁止并发启动、当前没有集中式 `disk list/start/stop`。明确 checkpoint 后内容允许丢失、恢复目标为最后成功 checkpoint。提交：`docs(m4): document independent multi-disk usage`。

## T17 — Debug 本地验证

**Files:** Create `docs/validation/m4-local-verification.md`。

执行并记录：

```powershell
cmake --preset msvc --fresh
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure
git diff --check
```

记录测试总数、失败项、编译器版本和 commit；M3 旧失败与 M4 新失败分开。提交：`docs(m4): record debug verification`。

## T18 — Release 本地验证

**Files:** Modify `docs/validation/m4-local-verification.md`。

执行：

```powershell
cmake --build --preset release --parallel 1
ctest --preset release --output-on-failure
```

记录 Release artifact SHA-256、CTest 结果和安全扫描结果。Debug/Release 任一新增失败都停止 VM 任务。提交：`docs(m4): record release verification`。

## T19 — 单盘 VM 观察脚本和 runbook

**Files:** Create `tests/e2e/m4_vm_lifecycle.ps1`；modify `README.md` only for exact commands。

脚本只能做只读观察、子进程捕获和证据写入；禁止 VMware/SSH/SCP/shutdown/reboot/pnputil。固定记录 VM UUID、MAC、Windows/AIM/phdskmnt 版本、管理员身份、NTFS bundle 父目录、R:/S: 前置状态。提交：`test(m4): add single-disk vm runbook`。

## T20 — 单盘 VM 创建、checkpoint、异常恢复

**Files:** Create `docs/validation/m4-single-multi-disk-acceptance.md`；evidence remains outside source tree。

用户执行：

```powershell
myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
```

写入 `A.txt`，等待明确 checkpoint generation，记录 hash；写入未提交文件；精确终止本次 PID；人工 VMware Reset/Power Off→On；用相同命令恢复。A.txt 必须恢复，未提交文件可丢失；不得重新格式化。记录完整 stdout/stderr、退出码、generation、bundle tree/hash 和残留状态。提交只包含验收记录。

## T21 — 双盘 VM 并行/隔离

**Files:** Create `tests/e2e/m4_vm_multi_disk.ps1`；modify acceptance report。

启动：

```powershell
myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
myrd.exe run --bundle E:\m4\disk-b --size 64M --drive-letter S: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
```

分别写入并 checkpoint；尝试第二个 A 实例确认 lock 拒绝；A 正常关闭不得卸载 S:。记录独立 generation、hash、AIM/volume identity。

## T22 — 双盘 VM 断电恢复

A/B 各成功 checkpoint 后分别写入未提交内容；用户人工执行一次 Reset；分别使用原始命令恢复。A、B 都必须恢复各自最后成功 checkpoint；一方失败不得改变另一方；最终无残留设备、盘符或进程。记录 PASS 或精确失败分类。

## T23 — VM 负面和完整性验收

在可恢复副本上逐项验证：盘符占用、非空无 manifest、size mismatch、writer lock、manifest/journal 最小篡改或截断、driver/AIM 不可用。驱动实验必须记录禁用前状态、`sc.exe config phdskmnt start= demand`、`sc.exe start phdskmnt`、恢复后的服务状态和 query 结果；不得使用 `pnputil /restart-device`。每项非零、fail closed、不格式化、不删除 bundle、不影响另一实例。若 `sc start` 无法恢复 query，分类 `ENVIRONMENT_BLOCKED` 并停止后续 VM 资格，不得擅自添加绕过。

## T24 — 最终 M4 资格报告

**Files:** Modify `docs/validation/m4-single-multi-disk-acceptance.md`、`README.md`。

只有以下全部 PASS 才能写 `M4 VM QUALIFICATION PASS`：

```text
fixed GitHub M3 baseline
Debug and Release build/CTest
M4 unit/integration
VM-1 create
VM-2 checkpoint + forced termination + reset recovery
VM-3 dual-instance isolation
VM-4 dual-disk reset recovery
VM-5 negative/integrity fail-closed
zero residual AIM devices/drive letters/myrd processes
```

`PRODUCT_FAIL`、`ENVIRONMENT_BLOCKED`、`HARNESS_FAIL`、`USER_ABORTED` 必须区分；self-test、fake、旧 root、Ready 文本和“文件碰巧存在”均不能替代 VM 资格。

## 自审查记录

- 覆盖 spec §1–§4：T00、T06–T13。
- 覆盖 spec §5–§6：T01–T05、T13–T14。
- 覆盖 spec §7：T06–T12。
- 覆盖 spec §8：T15–T16、T21–T22。
- 覆盖 spec §9–§13：T17–T24。
- 覆盖 spec §14：T24 的后续演进约束。
- 每个代码任务有明确文件、RED/GREEN、命令和固定提交主题；VM 任务明确人工边界和证据。
- 计划中没有未定义的占位实现；`git diff --check` 通过。
