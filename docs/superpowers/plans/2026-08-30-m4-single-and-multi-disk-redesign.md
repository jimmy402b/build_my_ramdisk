# MyRamDisk M4 单盘核心与多盘隔离实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 基于 GitHub 已验收的 M3 快照，交付一个前台 CLI：可创建/恢复单个 AIM-backed NTFS RAM disk，自动提交 checkpoint，VM 异常断电后恢复最近成功 checkpoint，并可用多个独立实例实现多盘隔离。

**Architecture:** 每个 `myrd run` 进程封装一个 `DiskRuntime`，独占一个 M3 provider、AIM 设备、Windows 卷、bundle writer lock 和 checkpoint 序列。多盘由多个独立 `DiskRuntime` 实例组成，不共享持久化状态；Windows Service、IPC 和 worker 管理留到后续阶段。

**Tech Stack:** C++20、MSVC x64、CMake/Ninja Multi-Config、Win32/AIM API、PowerShell 5.1 provisioning、CTest、Windows 11 x64 VMware。

---

## 0. 固定规则

所有实现均在新 worktree 中进行，不修改当前 `master` 工作树。基线固定为远端：

```text
https://github.com/jimmy402b/build_my_ramdisk.git
main: df09c4f0ac310b00e5b53be594c4bfba6730aaec
M3 evidence freeze recorded by README: 4ece675
```

每个任务必须遵循：写 RED 测试 → 运行并记录真实失败 → 写最小实现 → 聚焦测试 → 受影响的 M3 回归 → `git diff --check` → 精确提交。不得删除测试、放宽断言或把错误文本解析当作状态判定。

## 1. 文件地图

生产新增文件：

```text
src/run/cli.{h,cpp}
src/run/windows_volume.{h,cpp}
src/run/provision_volume.ps1
src/run/run_resources.rc
src/run/disk_runtime.{h,cpp}
src/run/run_loop.{h,cpp}
src/run/main.cpp
```

测试和文档新增/修改：

```text
tests/unit/m4_cli_tests.cpp
tests/unit/m4_windows_volume_tests.cpp
tests/unit/m4_disk_runtime_tests.cpp
tests/unit/m4_run_policy_tests.cpp
tests/integration/m4_product_composition_tests.cpp
tests/integration/m4_multi_instance_tests.cpp
tests/e2e/m4_vm_lifecycle.ps1
tests/e2e/m4_vm_multi_disk.ps1
tests/CMakeLists.txt
src/CMakeLists.txt
README.md
docs/validation/m4-baseline.md
docs/validation/m4-local-verification.md
docs/validation/m4-single-multi-disk-acceptance.md
```

## Task 1: 固定 GitHub M3 基线

**Files:**
- Create: `docs/validation/m4-baseline.md`
- Modify: none

- [ ] **Step 1: 创建 worktree 和分支**

```powershell
git fetch https://github.com/jimmy402b/build_my_ramdisk.git main
git worktree add .worktrees/m4-single-multi-disk -b feature/m4-single-multi-disk df09c4f0ac310b00e5b53be594c4bfba6730aaec
git -C .worktrees/m4-single-multi-disk status --short --branch
```

预期分支为 `feature/m4-single-multi-disk`，工作树干净。

- [ ] **Step 2: 记录基线**

在 `docs/validation/m4-baseline.md` 记录远端完整 SHA、README 的 M3 验收声明、M3 persistence spec、构建 preset、M3 验收脚本路径，并明确不继承 Task23 的 `control/management/host` 服务平台。

- [ ] **Step 3: 运行基线验证**

```powershell
cmake --preset msvc
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel 1
ctest --preset release --output-on-failure
```

记录每条命令、退出码、测试总数和已有失败；基线失败未分类前停止后续任务。

- [ ] **Step 4: 提交**

```powershell
git add docs/validation/m4-baseline.md
git commit -m "docs(m4): freeze github m3 implementation baseline"
```

## Task 2: CLI 与 bundle 分类

**Files:**
- Create: `src/run/cli.h`, `src/run/cli.cpp`
- Create: `tests/unit/m4_cli_tests.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: 写 RED**

测试 `parse_cli` 的 `run`、`compact`、help、缺失/重复/未知选项、非法设备路径、盘符大小写、64M～1T 大小、checkpoint 范围和溢出。测试不得访问 Windows、AIM 或文件系统。

- [ ] **Step 2: 运行 RED**

```powershell
cmake --build --preset debug --parallel 1 --target myrd_m4_cli_tests
ctest --preset debug -R '^unit\.m4\.cli$' --output-on-failure
```

预期目标或实现缺失导致失败。

- [ ] **Step 3: 实现最小类型和解析器**

```cpp
struct RunOptions {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::chrono::seconds checkpoint_interval{60};
    std::uint64_t checkpoint_dirty_bytes{64ull * 1024 * 1024};
    std::uint64_t ram_limit_bytes{2ull * 1024 * 1024 * 1024};
};
struct CompactOptions { std::filesystem::path bundle; };
enum class CliAction : std::uint8_t { Run, Compact, Help, Error };
CliParseResult parse_cli(std::span<const std::wstring_view> args);
```

解析器只验证语法、范围和路径形状；不创建目录、不读取 manifest、不访问 SCM/AIM。

- [ ] **Step 4: GREEN 和提交**

```powershell
cmake --build --preset debug --parallel 1 --target myrd_m4_cli_tests
ctest --preset debug -R '^unit\.m4\.cli$' --output-on-failure
git diff --check
git add src/run/cli.h src/run/cli.cpp tests/unit/m4_cli_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(m4): add single-disk cli contract"
```

## Task 3: Windows 卷生命周期与 provisioning

**Files:**
- Create: `src/run/windows_volume.h`, `src/run/windows_volume.cpp`, `src/run/provision_volume.ps1`, `src/run/run_resources.rc`
- Create: `tests/unit/m4_windows_volume_tests.cpp`, `tests/unit/test-m4-provision-script.ps1`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: 写 RED seam**

冻结 `ProductVolume`、`provision_new_volume`、`find_recovered_volume`、`flush_volume`、`lock_volume`、`assign_drive_letter`、`remove_drive_letter`。覆盖唯一 extents、foreign mount、盘符占用、timeout、flush/lock/busy 和 cleanup 失败；foreign 资源不得删除。

- [ ] **Step 2: 写脚本合同**

`provision_volume.ps1` 只接受 `-DiskNumber` 和 `-DriveLetter`，固定执行未初始化磁盘 → GPT → 单分区 → NTFS → 盘符 → 固定结果输出。已有文件系统、多分区、盘符占用和越界参数必须拒绝。

- [ ] **Step 3: 运行 RED**

```powershell
ctest --preset debug -R '^unit\.m4\.windows_volume$' --output-on-failure
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-provision-script.ps1 -SourceRoot .
```

- [ ] **Step 4: 实现和验证**

生产适配层必须通过 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` 验证目标卷归属；flush 前后重新验证句柄/归属；子进程使用有界 stdout/stderr、超时和精确 PID 清理；失败只回滚本次创建的 mount，不删除 bundle。

```powershell
cmake --build --preset debug --parallel 1 --target myrd_m4_windows_volume_tests
ctest --preset debug -R '^unit\.m4\.windows_volume$' --output-on-failure
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-provision-script.ps1 -SourceRoot .
git diff --check
git add src/run/windows_volume.h src/run/windows_volume.cpp src/run/provision_volume.ps1 src/run/run_resources.rc tests/unit/m4_windows_volume_tests.cpp tests/unit/test-m4-provision-script.ps1 src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(m4): add exact windows volume lifecycle"
```

## Task 4: 单盘 DiskRuntime

**Files:**
- Create: `src/run/disk_runtime.h`, `src/run/disk_runtime.cpp`, `tests/unit/m4_disk_runtime_tests.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: 写 RED 生命周期测试**

用 fake M3 runtime、volume 和 filesystem 验证：

```text
Create: preflight -> classify empty -> M3 generation-zero -> AIM start
        -> provision GPT/NTFS -> verify volume -> flush -> checkpoint generation 1
Recover: preflight -> classify manifest -> M3 recovery -> AIM start
         -> find existing volume -> verify geometry -> assign letter
```

必须证明 recovery 不调用 provisioning；未知非空 bundle 不启动 AIM；size mismatch 不挂载；失败只清理本次拥有的资源；第二实例同 bundle 被 writer lock 拒绝。

- [ ] **Step 2: 冻结接口**

```cpp
class DiskRuntime final {
public:
    static Result<std::unique_ptr<DiskRuntime>> create(
        const OperationContext&, const RunOptions&);
    static Result<std::unique_ptr<DiskRuntime>> recover(
        const OperationContext&, const RunOptions&);
    Result<void> start();
    Result<M3MaintenanceStats> maintenance_stats() const;
    M3CheckpointOutcome checkpoint(const OperationContext&);
    Result<void> shutdown_clean(const OperationContext&);
    const ProductVolume& volume() const noexcept;
    const M3AimRuntimeInfo& info() const noexcept;
};
```

- [ ] **Step 3: 实现**

`create` 只调用 M3 的 generation-zero API，`recover` 只调用 M3 recovery API；M4 不解析 journal/manifest/VHDX 私有格式。成功 create 必须在卷 flush 后提交 generation 1 才返回 ready。

- [ ] **Step 4: 运行和提交**

```powershell
cmake --build --preset debug --parallel 1 --target myrd_m4_disk_runtime_tests
ctest --preset debug -R '^unit\.m4\.disk_runtime$' --output-on-failure
git diff --check
git add src/run/disk_runtime.h src/run/disk_runtime.cpp tests/unit/m4_disk_runtime_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(m4): compose single-disk create and recovery runtime"
```

## Task 5: 自动 checkpoint 和安全关闭

**Files:**
- Create: `src/run/run_loop.h`, `src/run/run_loop.cpp`, `tests/unit/m4_run_policy_tests.cpp`
- Modify: `src/run/disk_runtime.*`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: 写 RED 策略测试**

冻结：时间或 dirty threshold 触发 checkpoint；先 `FlushFileBuffers` 后 M3 checkpoint；`Committed` 更新 last generation；`NotCommitted` 最多重试一次；`CommitIndeterminate` 禁止盲目重试；journal 满时停止新 checkpoint 并返回非零；Ctrl+C 为 lock → verify → flush → final checkpoint → dismount → remove letter → stop；volume busy 时保持运行并提示重试。

- [ ] **Step 2: 冻结接口**

```cpp
struct MaintenanceState {
    std::chrono::steady_clock::time_point last_checkpoint;
    std::chrono::steady_clock::time_point checkpoint_suppressed_until;
    bool journal_full{};
    bool skip_final_checkpoint{};
    bool exit_failure{};
};
bool checkpoint_due(const RunOptions&, const M3MaintenanceStats&,
                   const MaintenanceState&,
                   std::chrono::steady_clock::time_point) noexcept;
int run_loop(DiskRuntime&, const RunOptions&, std::atomic_bool&);
```

- [ ] **Step 3: 实现和验证**

每秒读取 stats，checkpoint 串行化，普通 I/O 继续；容量判断使用 M3 结构化错误码，不解析人类错误文本。

```powershell
cmake --build --preset debug --parallel 1 --target myrd_m4_run_policy_tests
ctest --preset debug -R '^unit\.m4\.run_policy$' --output-on-failure
git diff --check
git add src/run/run_loop.h src/run/run_loop.cpp tests/unit/m4_run_policy_tests.cpp src/run/disk_runtime.h src/run/disk_runtime.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(m4): add checkpoint scheduling and clean shutdown"
```

## Task 6: CLI 组合入口和本地集成

**Files:**
- Create/replace: `src/run/main.cpp`
- Create: `tests/integration/m4_product_composition_tests.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

- [ ] **Step 1: 写 RED 组合测试**

覆盖 help 输出、错误 stdout/stderr、提权优先级、driver unavailable 的 zero-write、create/recover 单一入口和 compact 路由。测试通过 seam 断言预检失败时没有 bundle/AIM/volume 副作用。

- [ ] **Step 2: 实现 wmain**

`wmain` 只构造 args、调用 parser、检查提权、分类 bundle、选择 `DiskRuntime::create/recover`、安装 console handler、调用 `run_loop`，以及调用已有 M3 offline compact API。不得在入口复制持久化格式算法。

- [ ] **Step 3: GREEN 和提交**

```powershell
cmake --build --preset debug --parallel 1
ctest --preset debug -R '^integration\.m4\.product_composition$|^unit\.m4\.' --output-on-failure
git diff --check
git add src/run/main.cpp tests/integration/m4_product_composition_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(m4): expose single-disk foreground product"
```

## Task 7: 多实例隔离

**Files:**
- Create: `tests/integration/m4_multi_instance_tests.cpp`
- Modify: `tests/CMakeLists.txt`, `README.md`

- [ ] **Step 1: 写 RED**

用两个 fake/native child 验证 disk-a/R: 与 disk-b/S: 可以并行；A checkpoint 不改变 B；A shutdown 不卸载 S:；A/B 失败清理互不影响；同 bundle 第二实例被 writer lock 拒绝。

- [ ] **Step 2: 实现测试和文档**

比较每个实例独立的 bundle inventory、generation、volume identity 和 cleanup ledger；README 增加两个并行命令，并明确当前没有集中式 `disk list/start/stop`。

- [ ] **Step 3: 运行和提交**

```powershell
cmake --build --preset debug --parallel 1
ctest --preset debug -R '^integration\.m4\.multi_instance$' --output-on-failure
git diff --check
git add tests/integration/m4_multi_instance_tests.cpp tests/CMakeLists.txt README.md
git commit -m "test(m4): prove independent multi-instance isolation"
```

## Task 8: 本地 Debug/Release 关闭门

**Files:**
- Create: `docs/validation/m4-local-verification.md`

- [ ] **Step 1: 完整构建和测试**

```powershell
cmake --preset msvc --fresh
cmake --build --preset debug --parallel 1
cmake --build --preset release --parallel 1
ctest --preset debug --output-on-failure
ctest --preset release --output-on-failure
git diff --check
```

报告必须区分 M3 基线已有失败与 M4 新失败；不得通过删测试或放宽断言处理。

- [ ] **Step 2: 静态安全检查**

生产 `src/run` 不得包含 `MyRamDiskService`、`MyRamDiskHost`、`CreateNamedPipe`、`vmrun`、`ssh`、`scp`、`shutdown`、`reboot`、`pnputil /restart-device`，也不得复制 M3 journal/manifest/VHDX 格式代码或按名称 kill/删除未知设备。

- [ ] **Step 3: 提交**

```powershell
git add docs/validation/m4-local-verification.md
git commit -m "docs(m4): record local debug release verification"
```

## Task 9: VM 单盘创建、checkpoint 和异常恢复

**Files:**
- Create: `tests/e2e/m4_vm_lifecycle.ps1`
- Create: `docs/validation/m4-single-multi-disk-acceptance.md`
- Modify: `README.md` only for exact VM instructions

由用户在批准的 Windows 11 x64 VMware VM 中执行。脚本不得调用 VMware、SSH、SCP、shutdown、reboot 或 pnputil。

- [ ] **Step 1: 记录前置环境**

记录 VM UUID、MAC、Windows build、AIM/phdskmnt 版本、Administrator 身份、持久 NTFS bundle 父目录、空闲 R:/S:、无目标 bundle/AIM/目标盘符/myrd 进程。

- [ ] **Step 2: 创建单盘**

```powershell
myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
```

必须看到 `Ready: R: generation 1.`，R: 为目标 AIM 设备上的 GPT/NTFS，写入 `R:\A.txt` 后可读回，bundle 含合法 M3 文件，且无未知设备或第二盘符。

- [ ] **Step 3: 建立 checkpoint 承诺**

写入 `A.txt`，等待并记录 `Checkpoint committed generation N.` 与 SHA-256；再写入 `after-checkpoint.txt`，不等待下一次 checkpoint。

- [ ] **Step 4: 异常终止和人工 Reset**

只终止本次 `myrd.exe` 的精确 PID；用户在 VMware UI 执行一次 Reset 或 Power Off→Power On；禁止正常关机。

- [ ] **Step 5: 相同命令恢复**

Windows 启动后再次执行完全相同命令。必须满足：`A.txt` hash 与成功 checkpoint 一致；after-checkpoint 文件可存在或丢失；恢复 generation 是完整已提交 generation；未格式化/重新分区；R: 绑定本次 AIM 设备。

- [ ] **Step 6: 记录证据**

记录命令、退出码、stdout/stderr、generation、bundle tree inventory/hash、Reset 时间和残留状态。此步骤只能标记 VM-1/VM-2 PASS，不能单独标记 M4 总体通过。

## Task 10: VM 双盘并行、隔离和断电恢复

**Files:**
- Create: `tests/e2e/m4_vm_multi_disk.ps1`
- Modify: `docs/validation/m4-single-multi-disk-acceptance.md`

- [ ] **Step 1: 启动两个实例**

```powershell
myrd.exe run --bundle E:\m4\disk-a --size 64M --drive-letter R: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
myrd.exe run --bundle E:\m4\disk-b --size 64M --drive-letter S: --checkpoint-seconds 10 --checkpoint-dirty-mb 1
```

分别写入 `R:\A.txt`、`S:\B.txt`，等待并记录各自 checkpoint/generation/hash/volume identity。

- [ ] **Step 2: 验证 writer lock**

用第二个进程启动 disk-a，必须被 lock 拒绝，不得创建第二 AIM 设备或修改 A bundle。

- [ ] **Step 3: 验证双盘断电**

A/B 都成功 checkpoint 后，各写入一个未提交文件；用户人工执行一次 VMware Reset；启动后分别运行 A/B 原始命令。A/B 必须恢复各自最后成功 checkpoint，且一方失败不得删除、卸载或修改另一方资源。

- [ ] **Step 4: 清理和提交证据**

分别 Ctrl+C 关闭 A/B，确认无残留 AIM 设备、R:/S:、myrd 进程和未归属临时文件；记录 VM-3/VM-4 PASS 或精确失败分类。

## Task 11: VM 负面和完整性验收

**Files:**
- Modify: `tests/e2e/m4_vm_lifecycle.ps1` only for read-only observers
- Modify: `docs/validation/m4-single-multi-disk-acceptance.md`

- [ ] **Step 1: 输入和锁冲突**

逐项验证盘符占用、非空无 manifest、size mismatch、同 bundle writer lock。每项非零退出、不启动错误卷、不格式化、不删除用户数据。

- [ ] **Step 2: bundle 完整性**

在验收副本上做一次可恢复的 manifest/journal 最小截断或篡改；重新运行必须报完整性错误并拒绝挂载。恢复原始副本后才能继续，损坏副本不能写成 PASS。

- [ ] **Step 3: driver/AIM 不可用**

只在用户明确批准且有 VM 快照时执行。驱动不可用时，产品必须在 bundle 访问前失败且 tree inventory 不变；恢复驱动后重新验证。若环境不能恢复，记为 ENVIRONMENT_BLOCKED，不加入 pnputil 绕过。

- [ ] **Step 4: 分类**

```text
PRODUCT_FAIL       环境满足但产品行为违反 spec
ENVIRONMENT_BLOCKED VM/AIM/驱动/权限/盘符前提不满足
HARNESS_FAIL        验收脚本或观察工具错误
USER_ABORTED        人工 Reset/电源/命令步骤违反流程
```

## Task 12: 最终资格结论

**Files:**
- Modify: `docs/validation/m4-single-multi-disk-acceptance.md`
- Modify: `README.md` only for verified support boundary

- [ ] **Step 1: 核对所有门**

只有以下全部 PASS 才能写入 `M4 VM QUALIFICATION PASS`：

```text
fixed GitHub M3 baseline verified
Debug/Release build and CTest PASS
M4 local unit/integration PASS
VM-1 single-disk create PASS
VM-2 checkpoint + forced termination + reset recovery PASS
VM-3 dual-instance isolation PASS
VM-4 dual-disk reset recovery PASS
VM-5 negative/integrity fail-closed PASS
zero residual AIM devices/drive letters/myrd processes
```

- [ ] **Step 2: 排除替代证据**

self-test、fake runtime、旧 bundle/root、单纯出现 Ready、未提权 CTest 或断电后文件碰巧存在，均不能替代 VM 资格。

- [ ] **Step 3: 提交**

```powershell
git diff --check
git add docs/validation/m4-single-multi-disk-acceptance.md README.md
git commit -m "docs(m4): record single and multi-disk vm acceptance"
```

## 计划自检

- Spec §1–§4：Task 1、4、5、6 覆盖基线、DiskRuntime、checkpoint 和 M3 durable-view 语义。
- Spec §5–§6：Task 2、3、6 覆盖 bundle 完整性、CLI 和 provisioning。
- Spec §7：Task 4、5 覆盖 create/recover/checkpoint/clean shutdown。
- Spec §8：Task 7、10 覆盖多实例隔离、writer lock 和独立恢复。
- Spec §9–§13：Task 8–12 覆盖本地验证、VM 环境、VM-1～VM-5 和最终失败分类。
- Spec §14：Task 12 固定未来 Service/IPC 必须复用已验收的 DiskRuntime。
- 无 `TODO`、`TBD` 或未定义接口；所有任务包含明确文件、命令、预期和提交边界。

