# 已知缺陷：≥8 GiB 的盘在 create 阶段卡死（2026-11-04 实用发现）

**状态**：开放（Open） · **影响版本**：1.0.0 · **发现方式**：宿主机实际安装使用
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