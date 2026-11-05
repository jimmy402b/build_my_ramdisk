# 已知缺陷：≥8 GiB 的盘在 create 阶段卡死（2026-11-04 实用发现）

**状态**：**已修复，1.0.1 候选**。§11 create 三处缺陷（`7a1d4d1`）；§13 是本轮对 §12 的收尾：
写入停顿的真实原因是 checkpoint 分类与恢复的四个缺陷（非 AIM 写路径），客机 16 GiB/12 GiB
全流程（create → 7 GiB 写 → checkpoint → 硬断电 → verify）已有读数。
**影响版本**：1.0.0 · **发现方式**：宿主机实际安装使用 · **修复目标**：1.0.1
**严重性**：高（8 GiB 及以上容量无法创建；4 GiB 正常） · **发现日期**：2026-11-04

## 1. 摘要

在宿主机（Windows 11 26200，63.66 GB 内存，空闲 49.6 GB）上安装 1.0.0 后：

| 容量 | 结果 |
|---|---|
| 4 GiB（automatic 内存模式） | ✅ 创建成功，卷挂载、`disk list` 可见 |
| **8 GiB**（automatic） | ❌ 卡在 create-bundle，worker 空闲，CLI `the control pipe exchange failed` |
| **16 GiB**（automatic） | ❌ 更早失败：`M3Provider::open provider limits are invalid` |
| **16 GiB**（fixed 8 GiB / fixed 16 GiB） | ❌ 卡在 create-bundle；服务侧等待超时 `code=4107 WorkerPipeChannel::read` |

发布时的容量验证覆盖 64 MiB–1 GiB（4 GiB 是本次首次验证成功），**≥8 GiB 是未测区间**。

## 2. 环境

| 项 | 值 |
|---|---|
| 宿主机 | Windows 11 专业版 build 26200，x64，63.66 GB RAM（commit limit 67.66 GB） |
| 驱动 | `phdskmnt` 1.2.28.93，RUNNING（未安装/未替换） |
| 安装 | `tools/install/Install-MyRamDisk.ps1 -SourceDirectory out/build/msvc/src/Release`（提权） |
| 软件列表 | 安装后 `MyRamDisk 1.0.0` 条目正常（卸载指向 `C:\Program Files\MyRamDisk\Uninstall-MyRamDisk.ps1`） |
| 代码 | `feature/m5-lifecycle` @ `f710583`（tag v1.0.0，已推送 GitHub） |
| 客机 | `myrd-vm`，4 vCPU，**内存已调为 20480 MB**（原 8192，`.vmx` 备份 `Windows 11 x64.vmx.bak-20261104-013944`），C: 剩余 25.25 GB |
| 宿主机现状 | 已 `Uninstall-MyRamDisk.ps1 -PurgeData` 卸载干净（服务/文件/注册表/数据/卷均已清） |

## 3. 复现步骤（宿主机，需管理员）

```powershell
# 1) 安装（仓库构建产物）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install\Install-MyRamDisk.ps1 `
    -SourceDirectory out\build\msvc\src\Release
# 2) 建盘（4 GiB 成功；把 8GiB/16GiB 换进去就复现）
New-Item -ItemType Directory -Force -Path C:\MyRamDisk
& 'C:\Program Files\MyRamDisk\myrd-cli.exe' disk create --name t16 --bundle C:\MyRamDisk `
    --size 16GiB --memory-mode fixed --memory-hard-limit 16GiB --mount Z: --wait --timeout 1800
```

**8 GiB（automatic）的完整观察**：

```text
disk.create: operation queued
operation 0429a3bd-...: running (phase admitting)
myrd-cli: the control pipe exchange failed          (约 30.2 s 后)
卷 Z: 已挂载（8,572,104,704 B，NTFS，MyRamDisk）
operation 0429a3bd-...: running (phase create-bundle)   <- 10 分钟后仍在此
worker PID 6048: CPU delta 0、IO delta 0（观察 10 s）
bundle: base-0000.vhdx 4,194,304 B（不增长）；disk.rdm 8,192 B；journal-e0001.rdj 8,192 B
service.log: action=create-bundle code=4107 step=WorkerPipeChannel::read
             detail=pipe transfer exceeded its bounded wait
```

**16 GiB（automatic）**：

```text
service.log: action=create-bundle code=1 step=M3Provider::open
             detail=provider limits are invalid
```

**16 GiB（fixed 16 GiB）**：

```text
CLI: disk.create ... running (phase admitting) -> control pipe exchange failed
service.log: action=create-bundle code=4107 step=WorkerPipeChannel::read
             detail=pipe transfer exceeded its bounded wait
之后 profile 处于：Provisioning blocked/needs-attention runtime=Starting
                  activity=ResolvingCommit persistence=CommitIndeterminate health=NeedsAttention
此时 disk forget 也失败：myrd-cli: the control pipe exchange failed
```

**`--no-start` 变体**：create 本身也卡；重试时（bundle 有残留）出现
`code=4110 step=DiskWorkerRuntime::record_bundle_identity detail=the bundle's image uuid is not the one this worker was launched for`。

## 4. 已排除的因素

- **内存不足**：失败时宿主机空闲 49.6 GB，且 worker 零 CPU/零 IO——不是 OOM/换页。
- **盘符冲突**：一次重试报 `requested drive letter is occupied`，那是上一次卡死留下的 **Z: 卷残留**；
  用 `mountvol Z: /d` 清掉后重试仍是 create-bundle 卡死。
- **体积导致的慢**：base 镜像停在 4 MiB 不增长、worker 无 IO，不是“写得慢”。
- **服务整体死掉**：`disk list` 在卡死期间仍 16 ms 返回；只有**长操作派发**被占住（`forget` 也超时）。

## 5. 可能原因（按优先级）

1. **create-bundle 动作内部在 provisioning 之后等待一个永不发生的事件**：
   卷已挂载（说明 `provision_volume.ps1` 的 Initialize/Format/Set-Partition 已跑完），
   之后 worker 等待的步骤（generation-zero 镜像创建 / record_bundle_identity / 发布 profile）
   零 CPU 零 IO。>4 GiB 才出现，指向 **image/store 的大小相关分支**。
2. **store 预算与虚拟容量交互**：`src/run/runtime_adapter.cpp` 的 `store_options_for(ram_limit)`：
   - `ram_limit == 0` → 用 production 默认；
   - 否则 `hard = ram_limit, soft = ram_limit/2, low = ram_limit/4`。
   `--memory-mode fixed` 要求 hard limit ≤ 盘大小；16 GiB 盘用 16 GiB 上限仍卡，
   而 4 GiB 盘 + automatic 可以 → 可能是镜像创建时需要 **超过 hard limit** 的驻留内存而永久等待。
3. **automatic 模式对大容量的预算计算产生非法值**：16 GiB automatic 直接
   `M3Provider::open provider limits are invalid`（见 `src/m3/m3_provider.cpp:594` 与
   `src/storage/dynamic_block_types.h` 的 `validate_dynamic_block_store_options`）。
   4 GiB automatic 正常 → automatic 的公式在大容量下可能给出 soft/hard/low 次序错误或未按块对齐。
4. **服务派发线程被长操作占住**：create 在服务侧同步执行，客户端 `operation.wait` 交换超时
   （`worker_pipe` 的 bounded wait 与客户端交换超时）；这解释了 “control pipe exchange failed”
   与后续 `forget` 失败，但**不是** create-bundle 卡死的根因。
5. **残留 bundle 的 image uuid 校验**（`record_bundle_identity`）：重试时如果 bundle 是上一次
   半成品，会报 `4110`；修复测试用例时要先清 bundle。

## 6. 建议的调查路径

1. **先做复现工具**：新增 `integration.m5.bigdisk`（host 驱动/客机执行）：
   - 安装并启动服务；`disk create --size <可配> --memory-mode <automatic|fixed> --memory-hard-limit <可配> --mount Z: --wait`；
   - 记录 operation 状态、bundle 文件大小、worker CPU/IO、`service.log` 尾部；
   - 支持 `cleanup` 模式（停盘/forget/卸载）；不自动清理，便于抓现场。
2. **在 20 GB 客机上复现 16 GiB**：确认与宿主机一致（预期一致），并对比 4/5/6/7/8 GiB 找阈值。
3. **定位卡点**：
   - 在 `m5_worker_runtime.cpp` 的 create-bundle 各步骤前后加 `OutputDebugString`/文件 trace（或临时 `MYRD_RECLAIM_TRACE` 风格的文件日志）；
   - 用 `procdump -ma -h`（若可用）抓 worker 的调用栈，看它在等哪个句柄/事件；
   - 对照 `WorkerPipeChannel::read` 的 deadline 来源（`m5_worker_private_pipe.cpp:140`，
     `remaining_ms(context)`）与 create 操作的 context deadline；
   - 检查 `store_options_for` / automatic 预算公式在大容量下的 soft/hard/low 与块对齐
     （`validate_dynamic_block_store_options`）。
4. **验证修复**：
   - 8/16 GiB create 成功（automatic 与 fixed 各一次）；
   - 16 GiB 盘 + 12 GiB 上限：写 > 上限、checkpoint、`vmrun stop hard`、重启、校验数据；
   - 观察 `hard_limit_rejections`、`evicted_blocks_total`、worker 工作集；
   - 全量门禁：A1/A2 各 158、B 5、fault 6；重新冻结 manifest、重新打包、tag **v1.0.1**。

## 7. 关键代码位置

| 位置 | 作用 |
|---|---|
| `src/host/m5_worker_runtime.cpp:495+` | reclaim 步骤 trace；create 延续（`resume_existing_create`）与 `record_bundle_identity` 的入口 |
| `src/run/runtime_adapter.cpp:45` | `store_options_for(ram_limit)`；`ram_limit==0` 用默认，否则 hard/soft/low = L, L/2, L/4 |
| `src/run/disk_runtime.cpp:176+` | provisioning 调用与 `drive_letter_assigned_` |
| `src/run/provision_volume.ps1` | Initialize-Disk → New-Partition → Format-Volume → Set-Partition（已确认执行到挂载） |
| `src/m3/m3_provider.cpp:594` | `M3Provider::open` 的 “provider limits are invalid” |
| `src/storage/dynamic_block_types.h` | `validate_dynamic_block_store_options` 的严格次序/对齐规则 |
| `src/host/m5_worker_private_pipe.cpp:140` | `WorkerPipeChannel::read` 的有界等待（`code=4107`） |
| `src/management/service_core.cpp` | create 的派发/单盘检查/预算校验（`0x5609`、`0x560A`、`0x5603`） |

## 8. 现场状态（新会话接手时）

- 宿主机：**已卸载干净**（服务/注册表/文件/数据/卷均无），可直接重装复现；
- 客机：`myrd-vm` 已按 20 GB 内存启动（memsize=20480，备份见 §2），仓库已 stage 过（179 用例）；
- 仓库：`feature/m5-lifecycle` @ `f710583`，工作树干净；`out/m5-evidence` 是 1.0.0 的冻结证据；
- 发布：v1.0.0 已推送 GitHub（main + tag + Release 资产 `MyRamDisk-1.0.0-win11-x64.zip`）；
- 复现所需权限：宿主机安装/建盘需管理员（`Start-Process -Verb RunAs`，会弹 UAC）；客机用 harness agent。

## 9. 与 1.0.0 的关系

1.0.0 已发布，此缺陷属于**未覆盖容量区间**（发布验证 64 MiB–1 GiB）。修复后应发布 **1.0.1**，
并在 `m5-release-1.0.0.md` 的已知限制中写明；本文档是修复会话的入口。

## 10. 调查 checklist（可直接执行，建议按顺序）

> 每条命令的工作目录都是 `C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m5-lifecycle`。
> 客机命令统一用 `ssh`（别名 `myrd-vm`，密钥 `%USERPROFILE%\.ssh\id_ed25519`）。
> 宿主机提权命令用 `Start-Process -Verb RunAs`（会弹 UAC，需要有人在桌面上点“是”）。

### 阶段 0：环境确认（~10 分钟）

```powershell
# 0.1 仓库干净、HEAD、tag
cd C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m5-lifecycle
git status --short; git log --oneline -1; git describe --tags --exact-match

# 0.2 客机在跑、内存 20 GB
& 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe' -T ws -vp 12345678 list
ssh -o BatchMode=yes -i $env:USERPROFILE\.ssh\id_ed25519 usr@myrd-vm "powershell -NoProfile -Command (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB"

# 0.3 干净构建（改过头文件就加 --clean-first；这是仓库 AGENTS.md 的硬规则）
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build out/build/msvc --config Release --clean-first

# 0.4 重新 stage（179 用例）
tools\m5vm\Invoke-M5Vm.ps1 -Stage -Configuration Release
```

### 阶段 1：写复现用例 `integration.m5.bigdisk`（~30–45 分钟）

要求（照 `tools/m5vm/Test-M5NonAdmin.ps1` + `tests/e2e/test-m5-nonadmin-prepare.ps1` 的写法）：

1. `tests/e2e/test-m5-bigdisk.ps1`（guest，agent 执行）：
   - 读 `C:\m5\bigdisk\size.txt`（如 `16GiB`）和 `memory.txt`（如 `fixed:12GiB` / `automatic`）；
   - 确保服务已安装并 RUNNING（复用 drive_letter 用例的 `Ensure-Service`）；
   - `disk create --name bigdisk --bundle C:\m5\bigdisk\bundle --size <size> --mount Z: --wait --timeout 1800`（按 memory 追加 `--memory-mode/--memory-hard-limit`）；
   - 每 5 s 打印一次：operation 状态、bundle 文件大小、worker CPU/IO（最多 120 s），然后**保留现场**退出（不要自动清理）；
   - `cleanup` 模式：停盘 → forget → 卸载服务 → 删 `C:\m5\bigdisk`。
2. `tests/CMakeLists.txt` 注册 `integration.m5.bigdisk`（guest-only，`RUN_SERIAL`，排除在发布门禁之外）。
3. 跑法：

```powershell
# 写配置（客机）
ssh -i $env:USERPROFILE\.ssh\id_ed25519 usr@myrd-vm "powershell -NoProfile -Command \"New-Item -ItemType Directory -Force C:\m5\bigdisk | Out-Null; Set-Content C:\m5\bigdisk\mode.txt stability; Set-Content C:\m5\bigdisk\size.txt 16GiB; Set-Content C:\m5\bigdisk\memory.txt fixed:12GiB\""
# 跑
tools\m5vm\Invoke-M5Vm.ps1 -Run -Filter '^integration\.m5\.bigdisk$' -MinTimeoutSeconds 1800 -DeadlineSeconds 2400
# 抓现场
ssh -i $env:USERPROFILE\.ssh\id_ed25519 usr@myrd-vm "powershell -NoProfile -Command \"Get-Content C:\ProgramData\MyRamDisk\logs\service.log -Tail 40; Get-Process myrd* | Select Name,Id,CPU\""
```

### 阶段 2：在 20 GB 客机上复现（~15 分钟）

依次跑 **4 GiB（对照，应成功）→ 8 GiB → 16 GiB fixed:12GiB → 16 GiB automatic**，每次先 `Reset-M5Guest.ps1`（或 bigdisk 的 cleanup）清场：

```powershell
ssh -i $env:USERPROFILE\.ssh\id_ed25519 usr@myrd-vm "powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1"
```

记录：哪一步开始卡、卡在 create-bundle 的哪个子步（用阶段 3 的 trace）、automatic 16 GiB 是否仍报 `provider limits are invalid`。

### 阶段 3：定位卡点（~1–3 小时，关键）

1. **加 trace**（临时、可回滚）：在 `src/host/m5_worker_runtime.cpp` 的 create-bundle 动作各步骤前后、
   `src/run/runtime_adapter.cpp` 的 `store_options_for`/provisioning 调用前后、`record_bundle_identity`
   前后，各写一行到 `C:\ProgramData\MyRamDisk\logs\worker-trace.log`（用 `std::ofstream` 追加，别忘了 flush），
   把算出的 `hard/soft/low` 值也打出来。
2. 重建 + stage + 重跑 16 GiB，读 trace：最后一行就是卡住的那一步。
3. 如果 trace 不够，抓 worker 调用栈：
   - 若装了 Sysinternals：`procdump -ma -h <worker-pid> C:\tmp\worker.dmp`，再用 WinDbg `~*k` / `!analyze -v`；
   - 没有 procdump：`Get-Process -Id <pid> | Select-Object -ExpandProperty Threads | Select Id,WaitReason,ThreadState`，
     看是不是全部 `Wait`/`UserRequest`。
4. 对照服务侧 deadline：`src/host/m5_worker_private_pipe.cpp:140` 的 `remaining_ms(context)` 来自
   create 操作的 `OperationContext`；在 `src/management/service_core.cpp` 找 create 的 context 构造处，
   确认 300 s 之类的上限（这决定“卡死”还是“超时”）。

### 阶段 4：逐条验证假设（每条 ~15 分钟）

| # | 假设 | 快速判别 |
|---|---|---|
| H2 | store 预算 < 镜像创建所需 | 8 GiB 盘 + `--memory-mode fixed --memory-hard-limit 1GiB`：若成功→预算相关；若照卡→不是 |
| H3 | automatic 大容量算出非法 limits | 加 trace 打印 limits；或直接跑 16 GiB automatic 抓 `provider limits are invalid` 前的值 |
| 阈值 | 4 GiB 行、8 GiB 不行 | 依次测 5/6/7 GiB，找第一个失败的容量 |
| 格式 | Format-Volume 太慢 | 16 GiB + `--cluster-size 64KiB`；若仍卡→与格式无关 |
| 残留 | 半成品 bundle 影响 | 每次先删 `C:\m5\bigdisk\bundle` 再跑 |

### 阶段 5：修复与验收

1. 修复后 `integration.m5.bigdisk` 必须覆盖：
   - 8 GiB、16 GiB（fixed 12 GiB、automatic）create 成功、卷挂载、`disk list` 可见；
   - 写 > 内存上限（如 6 GiB）→ 观察 `evicted_blocks_total` 增长、worker 工作集不越上限；
   - checkpoint → `vmrun stop hard` → 重启 → 数据校验；
   - 单盘语义：第二块盘仍被拒。
2. 全量门禁（按 `docs/operations/m5-release-rehearsal.md` §9）：
   A1/A2 各 158、B 5、fault 组 6（每例干净重启）。
3. 重新冻结 manifest → `Verify-M5Evidence.ps1` accepted → 重新打包 → tag **v1.0.1** → 更新
   `m5-release-1.0.0.md`（或新建 1.0.1 说明）、INDEX、handoff。

## 11. 修复（2026-11-04，`7a1d4d1`）：三个独立缺陷，每个都有读数

调查用的是新增的 `integration.m5.bigdisk` 用例（`tests/e2e/test-m5-bigdisk.ps1`，在
`tests/CMakeLists.txt` 注册，**不进默认门禁**）加 `MYRD_TRACE_FILE` 逐步骤 trace
（`M4Adapter.start` 打 hard/soft/low/journal 限额；create 各步骤 begin/ok；
`M3Provider::open` 拒绝时打是哪一项、什么值）。

### 11.1 automatic 预算不是块对齐的 → `provider limits are invalid`

客机 20 GiB 内存、8 GiB automatic，trace 第一行就是答案：

```text
M4Adapter.start: virtual=8589934592 ram_limit=5368381440 hard=5368381440
                 soft=2684190720 low=1342095360 journal_limit=2147483648 journal_records=524287
```

`materialize_memory_policy` 的 automatic 是 `min(物理内存/4, 容量)` 再按 **64 KiB**
（`kProfileMemoryAlignmentBytes`，恰好等于 store 的 `kBlockSizeBytes`）向下取整，所以它通常是
块的**奇数倍**（这里是 `65,536 × 81,915`）。`store_options_for()` 直接把它除以 2 和 4，
soft/low 就不再是 64 KiB 对齐；`validate_dynamic_block_store_options` 返回
`LimitNotBlockAligned`，`M3Provider::open` 报 `provider limits are invalid`。
4 GiB 盘拿到的是 2 的幂预算（4 GiB），除 2 除 4 仍然对齐——这就是"≤4 GiB 全过、8 GiB 失败"的全部原因。
宿主机 16 GiB automatic 的 `15.9 GiB` 也是奇数倍，同一个错误。

**修复**：`store_options_for` 对 hard/soft/low 三者都做 `align_down(kBlockSizeBytes)`
（`src/run/runtime_adapter.cpp`）。

### 11.2 create 是一条 worker 命令，而它的 generation-zero checkpoint 随盘大小增长 → `4107`

客机 16 GiB fixed:12GiB 的 trace：

```text
18:11:15.293 checkpoint: begin          <- 之后 123 s 没有任何 trace
18:13:08.056 service: worker action returned
```

service.log 同时是 `code=4107 step=WorkerPipeChannel::read detail=pipe transfer exceeded its
bounded wait`。8 GiB 的 checkpoint 实测 98 s，16 GiB 123 s；之后 `record_bundle_identity`
还要把整本 journal 读回来校验（8 GiB 约 20 s、16 GiB 约 40 s）。而服务给每条 worker 命令的
上限是 `kMaximumWorkerCommandTimeoutMs = 120 s`（`service_worker_session.h`），
`register_bundle` 用的就是它。于是 worker 还在干活、服务已经放弃：operation 进
`CommitIndeterminate`、profile 进 NeedsAttention——和宿主机 16 GiB 的观察一致。

**修复**：新增 `kProvisioningWorkerCommandTimeoutMs = 15 min`，只给
`create_and_verify_bundle`（`ServiceWorkerControl::provisioning_context()`），
普通命令仍是 120 s；session 的绝对上限同步到 15 min，worker 自己的命令 deadline 也从 5 min
提到 15 min，避免两边对"谁先放弃"有不同答案。

### 11.3 CLI 的 30 s 交换预算和服务的 30 s wait 上限撞在同一刻

`myrd-cli --wait` 每次 `operation wait` 最多等 30 s（`cli_main.cpp`），而
`ServiceCore` 对 `operation wait` 的 `kMaximumWaitMs` 也是 30 s——服务在恰好 30 s 时回
`wait_timeout`，客户端先超时，打印 `the control pipe exchange failed` 并以 3 退出，而
**operation 之后其实成功了**。4 GiB 在客机上 69 s、8 GiB 125 s，所以每次都会撞上。

**修复**：服务侧 `kMaximumWaitMs` 降到 20 s，给客户端留 10 s 余量；等待者本来就会重问。

### 11.4 修复后的客机读数（`integration.m5.bigdisk`，`--wait`，exit=0）

| 配置 | create 耗时 | operation | 卷 | budget |
|---|---|---|---|---|
| 8 GiB automatic | 125 s | succeeded | 8,572,104,704 B NTFS `MyRamDisk` | 5,368,381,440 |
| 16 GiB fixed:12GiB | 314 s | succeeded | 17,162,039,296 B NTFS | 12,884,901,888 |
| 16 GiB automatic | 327 s | succeeded | 17,162,039,296 B NTFS | 5,368,381,440 |

三组都是 CLI exit 0、`disk list` 可见、`mem=` 正常；`journal-e0001.rdj` 分别写到
560,250,880 / 1,117,429,760 B 后由 checkpoint 提交。

## 12. 未完成：压力/断电阶段被一个**独立的写入停顿**挡住（2026-11-04 04:00）

16 GiB fixed:12GiB 创建成功后，`integration.m5.bigdisk` 的 `pressure` 阶段（向 `Z:` 写 7 GiB）：

- **普通缓存写**：1 GiB 用 1 s 写进文件缓存，然后 `WriteFile` 阻塞 **>25 min**；
  worker 工作集停在 **16.8 MB**（说明字节根本没到 store），journal 停在
  **1,117,429,760 B**（创建时的大小，不再增长）；`cmd /c dir Z:\` 同样阻塞；
  WMI/PowerShell 查询挂起 >5 min。客机存储栈被拖住。
- **hard reset 后重新 start（recovery）**：64 MiB 的 write-through 写仍挂；连
  `echo > Z:\tiny.txt`（16 字节）也挂——说明"挂载成功之后写路径就不可用"。
- **新建一块 16 GiB automatic 盘**：16 字节写**成功**（0 s）；随后 3 GiB 写再次把客机
  存储栈挂住（ssh 命令超时）。
- **换 8 GiB 盘 + 4 GiB 预算**（2026-11-04 04:50）：create 成功（192 s）；
  create 后写 1 GiB → 挂 >7 min；hard reset 后 `disk start` 成功，再写 **64 MiB** → 又挂
  >4 min。挂住时 worker 工作集仍是 **16.7 MB**，服务侧 interval checkpoint 的 `flush-volume`
  同样以 `4107` 超时；期间 `cmd`/`tasklist` 可用，而 PowerShell/`Get-Process` 直接挂起。
- 反证：同一客机、同一二进制，`integration.m5.cli_e2e`（**256 MiB** 盘，写 marker）
  1/1 PASS（101 s）。

结论：create 死锁已经修好并有读数，但"写到一定体量 / 恢复后写"会挂住是**另一个缺陷**，
且 2026-11-04 04:50 的对照把它收窄了：**与预算无关**（4 GiB 预算同样挂）、**与 16 GiB 无关**
（8 GiB 盘写 64 MiB 就挂）、**与写入大小无关**（64 MiB 就够），只与**卷容量档位**有关
（256 MiB 卷写 marker 正常）。挂住时 worker 工作集停在 ~16.7 MB，说明字节没有到达 store；
`flush-volume` 也跟着 4107，PowerShell/`Get-Process` 挂起而 `cmd` 仍可用——像是 AIM
驱动/共享内存侧的写路径在大卷上停住（候选：大卷的共享内存窗口/块分配路径、驱动的队列）。
尚未定性。在它定性之前：

- 验收 §3（写 > 上限 → 驱逐 → checkpoint → 硬断电 → 数据校验）**没有通过**；
- 因此全量门禁、manifest 重新冻结、打包、tag v1.0.1 **都没有做**。

**下一步的调查线索**（2026-11-04 05:00 读代码得到，尚未验证）：

- AIM 的数据通路不是内存直映射，而是走 `proxy` 的请求协议：`kMappingSize = 4 KiB 头
  + 8 MiB 数据区`（`src/proxy/imdproxy_protocol.h:25-27`），由 `ShmProxySession` 的 request
  线程持续服务（`src/proxy/shm_proxy_session.cpp:60-90`）；`RequestProcessor::process` 用一把
  互斥量串行化，且**只返回错误、不阻塞**（`request_processor.cpp`）。
- 所以挂住的位置只剩两处：① `device_.write()`（M3 store 的写/分配路径）阻塞；② 驱动侧在等
  一个永远不会来的完成。worker 工作集恒为 ~16.7 MB 更支持"请求还没进到分配就卡住"。
- 建议下一步：给 request 线程加 per-request trace（序号/offset/length/返回码），或直接抓 worker
  线程栈（`procdump -ma -h`）看 request 线程卡在 `device_.write`、`WaitForSingleObject` 还是
  驱动 IOCTL；再用 256 MiB / 1 GiB / 2 GiB 卷做容量二分，找第一个会挂的档位。

**需要人决定**：是先把范围收窄成"只发 create 修复"（仍然缺压力/断电读数与全量门禁），
还是先投入定位这个写入停顿。建议：**先不要发 1.0.1**，把写停顿定性后再走完整门禁。

---

## 13. 根因确认与修复（2026-11-04 晚，1.0.1 候选）

§12 的"写入停顿"最终确认**不在 AIM/驱动的写路径**：7 GiB write-through 实测 **12 s 完成**
（7,516,192,768 B，digest `AF76B0B8A76FDAB57DA646151D07E028D89D594B98756D0A44EBAF143E528554`），
卡住的是 **checkpoint 之后的分类与恢复**。四个独立缺陷，每个都有客机读数。

### 13.1 `checkpoint: disposition=2` 是 `committed`，不是 `CommitIndeterminate`

服务日志打印的是**管理层**枚举 `CheckpointCommitDisposition`
（`not_committed=0, commit_indeterminate=1, committed=2`，`service_core.h:40`）；
`DiskCheckpointDisposition`（`committed=0, not_committed=1, commit_indeterminate=2`，`disk_runtime.h:42`）
是另一个枚举，日志从未打印它。日志已改为 `to_string` 打枚举名；同一段日志现在读
`checkpoint: disposition=committed observed_dirty_epoch=… worker_error=0`，而且
`valid_checkpoint_evidence` 对 `committed` 要求"无 error"，旧日志一直满足。

### 13.2 提交成功却永远 Dirty：非 stop checkpoint 的"干净"判据用了一个不存在的 epoch

旧判据（`service_core.cpp`）：`live_dirty_epoch == cut_dirty_epoch`。
worker 的 `observe()` **从不设置** `dirty_epoch`（只有 `m5_worker_protocol.cpp` 的 wire 解码会读它），
所以 `live` 恒为 0，而 `cut` 是 `maintenance_stats().generation`（backing generation 1,2,…）。
后果：每个**已提交**的 checkpoint 都被记成 `succeeded_dirty`，profile 永远 `persistence=Dirty`，
维护 checkpoint 每 60 s 再来一次。

**读数（旧）**：`checkpoint: disposition=2 observed_dirty_epoch=1..14 generation=2..15`，
journal 长到 9,112,297,472 B。
**读数（新，1.0.1 最终验收）**：create `cut=1 post_commit_dirty=0` →
第一次维护 checkpoint `cut=2 post_commit_dirty=1`（7 GiB 写在 cut 之后，**仍脏**，正确）→
第二次 `cut=3 post_commit_dirty=0`（**干净**）→ 之后 interval checkpoint 保持干净，
最终 `generation=6 activity=Idle persistence=Clean`。

**修法**：两条路径都用 worker 能回答的块级事实 `!post_commit_dirty`；epoch 只记录、不比较。

### 13.3 恢复读端仍是 2 GiB（`b80a6e5` 只修了一半）

`b80a6e5` 缩放了 M3 provider 的 recovery limits，但 `disk start` 先走
`M4PersistenceAdapter::recover_complete`，它仍用 `production_persistence_limits()`（固定 2 GiB）。

**读数**：`recover_complete_image: journal bound readback=9111699456 limit=2147483648 file=9111707648`
→ `disk.start … select_complete_manifest_generation: no fully valid manifest generation exists`。
**修法**：`recovery_persistence_limits(virtual, ram)` 成为所有读端的**唯一**规则
（M3 provider、M4 adapter、worker 的 `default_recovery_options`）：2×dirty + 64 MiB，
无预算的 profile 保持 2 GiB 历史下限。

### 13.4 断电落在 start 的 `Starting/Recovering` 窗口 → profile 永久卡死

**读数**：启动扫描只 retire `running`，`starting` 被跳过（`admit_auto_starts`），于是
`runtime=Starting activity=Recovering mem=0/0` 永久；之后每次 `disk start` 都被
`0x5605 Another exclusive profile operation is active` 拒绝（`disk show` 仍 `health=Healthy`），
`disk repair-profile` 也无法编码命令（同一个门）。
**修法**：① sweep 也 retire `starting`；② `recover_after_restart` 把非终止的 `disk.start`
终止并释放 profile gate。**修复读数**：
`restart: an interrupted start was terminalized and its profile gate released` →
`verify.start_state=succeeded`、`generation=5`。

### 13.5 验收用例的等待判据修正

`thencheckpoint` 原先只等 `activity=Idle && persistence=Clean`；而 `persistence` 在第一次 checkpoint
之前也是 `Clean`。第一次正式跑因此在 `generation=2`、`mem=7,518,027,776`（7.5 GB 未持久化）时就"通过"了。
现在要求 **generation 超过写入前代次** 且 Idle/Clean（实测 2→5）。

### 13.6 验收读数（16 GiB / fixed:12GiB，7 GiB write-through，同一用例）

| 项 | 读数 |
|---|---|
| create | `disk create --wait` exit 0；卷 17,162,039,296 B NTFS；checkpoint generation 2 |
| 写入 | 12 s；7,516,192,768 B；digest `AF76B0B8…E528554` |
| checkpoint | gen 3 (`post_commit_dirty=1`) → gen 4 (0) → gen 6 `Idle/Clean`；journal 9,111,715,840 B 已提交 |
| 硬断电后 | 自动恢复为 `already-running`（generation 6）；文件长度与 digest 一致；第二块盘被 `0x5609` 拒绝 |

全量门禁、manifest 冻结与 1.0.1 打包见 `m5-release-1.0.1.md` 与 `m5-release-rehearsal.md`。

---

## 14. 后续缺陷：中止的 checkpoint 把 journal 写长，之后永远无法 checkpoint（2026-11-05，1.0.2）

**触发**（宿主机，1.0.1）：写 10 GiB → 在下一个 checkpoint 前删除 → 写 1 GiB → checkpoint。

**现象与读数**：
- 10 GiB write-through **14 s**（generation 不变，说明确实没有中间 checkpoint），删除瞬间完成，
  1 GiB 写 **2 s**；
- 随后每次自动 checkpoint 都失败：`action=flush-volume code=11 step=checkpoint.prepare
  detail=snapshot was aborted`（`SnapshotResourceExhausted`，10 GiB 删除留下的 discard backlog）；
- 之后每次重试都变成：`code=15 step=checkpoint.prepare
  detail=opened bundle file identity differs from the expected identity`，永久失败；
- 重启服务后盘回到 generation 929，**1 GiB 文件消失**（中止前接受的写入只在 RAM）；
- 再手动 checkpoint 在 publish 失败：`code=8 step=checkpoint.publish
  detail=reopened journal identity changed before publish`，profile 进入
  `blocked/needs-attention/CommitIndeterminate`；`disk stop`、`start`、`forget`、`repair-profile`
  全部被拒（`0x5605`/`0x5608`），没有产品级出路。

**根因**：journal writer 在切快照时已经追加了记录；中止的 checkpoint 没有把文件截回**已提交长度**
（实测 journal 1,414,406,144 → 1,904,615,424 B，多出 490 MB）。下一次打开 journal 时比较的是完整
Windows 文件 identity（含 size），于是永久不匹配；重启后走 publish，size != 新的 committed length，
同样失败。

**修复（1.0.2，`4d219f2`）**：
1. `M3Provider::checkpoint_prepare_impl`：打开 journal 后若文件长于已提交长度，先截断；准备过程
   没有走到 pending transaction 时，由回滚守卫再截一次；
2. `PreparedCheckpointTransaction::abort()`：截回自己的 `previous_committed_length_`；
3. compaction 的新 journal 产物没有尾巴可裁，保持不动。

**修复后读数（1.0.2，同一场景，宿主机）**：checkpoint 先中止 2 次
（`SnapshotResourceExhausted`），第 3 次提交 generation 12；再经 interval 到 `Idle/Clean`；
journal 2,945,921,024 → 4,841,418,752 B（已提交）；`disk stop` + `disk start` 后 generation 13，
`Z:\one.bin` 1,073,741,824 B、SHA-256 `1460863F…D092F` 一致，10 GiB 文件不存在。

**仍存在的限制**：大删除会让紧接着的 checkpoint 先自动中止 1–2 次（`SnapshotResourceExhausted`），
这是重试代价；checkpoint 现在能恢复，而不是永久卡死。已写进 `m5-release-1.0.2.md` §6。