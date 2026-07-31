# RamDisk CLI — Phase 1 & 2 Optimization Spec

> 对 MVP 的优化迭代：bug 修复 + UX 增强 (Phase 1) + 增量保存与延迟加载 (Phase 2)
>
> 版本: v2.0 | 日期: 2026-07-31 | 关联 spec: `2026-07-31-ramdisk-cli-design.md`

---

## 目录

- [Phase 1: Bug 修复 + UX 增强](#phase-1)
  - [1.1 修复动态 VHD 数据完整性](#11-修复动态-vhd-数据完整性)
  - [1.2 挂载后自动分配盘符](#12-挂载后自动分配盘符)
  - [1.3 --daemon 后台模式 + 日志文件](#13---daemon-后台模式--日志文件)
- [Phase 2: Proxy 模式（增量保存 + 延迟加载）](#phase-2)
  - [2.1 架构变化](#21-架构变化)
  - [2.2 StorageServer](#22-storageserver)
  - [2.3 数据流](#23-数据流)
  - [2.4 Pipe 协议提取方案](#24-pipe-协议提取方案)
  - [2.5 增量估算](#25-增量估算)
- [实施顺序](#实施顺序)
- [与现有代码的关系](#与现有代码的关系)

---

## Phase 1

### 1.1 修复动态 VHD 数据完整性

**问题**: 动态 VHD 经过 save → remount 后，文件内容出现空字节（null bytes 混入）。文件系统元数据幸存（GPT + NTFS 可识别、文件列表可见），但文件正文数据有偏差。

**根因剖析**: `VhdEngine::ReadSectors` / `WriteSectors` 遍历多个块时的扇区偏移计算或 BAT 块分配存在边界条件，导致块内扇区偏移累加错误。

**诊断方法**:
- 用模式化数据（递增 byte pattern）写入完整 VHD → save → reopen → 逐字节比对
- 覆盖：单块边界（0, 4095, 4096）、跨块边界、最后一个扇区
- 固定 VHD 作为对照组（确保不是 aim_ll 或文件系统层的问题）

**修复范围**: 仅 `vhd_engine.cpp`。不改变外部接口头文件。

**验证标准**:
```
创建 10MB VHD → 写入 0x00 0x01 0x02 ... → save → close → reopen → ReadSectors → 逐字节与原值比对 → 100% 一致
```

### 1.2 挂载后自动分配盘符

**当前行为**: create/mount 成功后，Windows 识别磁盘和 GPT 分区，但不会自动分配盘符。用户需手动 diskpart 或磁盘管理 assign letter。

**目标**: mount/create 完成后，检测磁盘上的分区并自动分配第一个可用盘符。

**实现方案**: mount 完成后轮询磁盘状态 → 找到分区号 → 调用 `SetVolumeMountPoint` 或通过 DeviceIoControl 发送 `IOCTL_DISK_SET_DRIVE_LAYOUT` 分配盘符。或者使用更简单的方式——调用 diskpart 脚本：
```
select disk N
select partition 1
assign letter=R
```

**备选方案**: 用 Windows `Virtual Disk Service (VDS)` COM API 做磁盘操作，比 diskpart 更稳定、可编程。但引入 COM 依赖。

**推荐方案**: 先用 `DeviceIoControl` / `GetVolumeNameForVolumeMountPoint` / `SetVolumeMountPoint` 纯 Win32 API，轻量无依赖。

**接口**:
```cpp
// 新增函数
bool AssignDriveLetter(int diskNumber, std::string& outDriveLetter);
// 返回分配的盘符如 "R:"，失败返回 false
```

**调用点**: `CmdCreate` 和 `CmdMount` 尾部，挂载成功后自动调用。

### 1.3 --daemon 后台模式 + 日志文件

**当前行为**: create/mount 在主线程 `while(Sleep(1000))` 阻塞终端，必须 Ctrl+C 才能退出。用户不能在同一终端做其他事。

**目标**: `--daemon` 标志使进程后台运行，父进程立即返回控制权给终端。`unmount` 命令触发守护进程退出。

**实现方案**:

```
用户执行: ramdisk-cli create -s 4G -o D:\disk.vhd --daemon
    │
    ├── 父进程:
    │     CreateProcess(自身, "--internal-daemon ...", DETACHED_PROCESS)
    │     return 0  // 终端立即可用
    │
    └── 子进程 (守护):
          --internal-daemon: 不暴露给用户，内部标志
          InitLogFile(opts.logFile)
          // ... 正常 create/mount 流程 ...
          while (running) Sleep(1000)
```

退出流程:
```
用户执行: ramdisk-cli unmount
    │
    ├── 直接调用 aim_ll -d 卸载
    ├── 同时通过互斥锁/命名 event 通知守护进程退出
    └── 守护进程收到信号 → DoSave → Stop → 退出
```

**守护进程互斥**:
- 启动时 `CreateMutex("Global\\RamDiskCli_Daemon")` — 确保只有一个守护进程
- `unmount` 命令通过 `CreateEvent` + `SetEvent` 发信号通知守护退出

**日志文件** (`ramdisk-cli.log`):
```
[2026-07-31 09:30:00] === RamDisk CLI v1.0.0 daemon started ===
[2026-07-31 09:30:00] Mounted: D:\disk.vhd → \\?\PhysicalDrive1 (4.00 GB)
[2026-07-31 09:30:01] Drive letter assigned: R:
[2026-07-31 09:30:01] Save interval: 300s
[2026-07-31 09:35:01] Auto-save: 4.00 GB → D:\disk.vhd (8.2s)
[2026-07-31 09:40:01] Auto-save: 4.00 GB → D:\disk.vhd (7.9s)
[2026-07-31 09:42:15] Unmount signal received, saving...
[2026-07-31 09:42:22] Saved 4.00 GB → D:\disk.vhd (7.1s)
[2026-07-31 09:42:22] Unmounted, exiting.
[2026-07-31 09:42:22] === RamDisk CLI daemon stopped ===
```

**新增 CLI 参数**:
| 参数 | 说明 |
|------|------|
| `--daemon` | create/mount 时后台运行 |
| `--log-file <path>` | 日志文件路径，默认 `./ramdisk-cli.log` |

**新增文件**: `src/daemon.cpp/h`（日志初始化 + 守护进程管理 + 互斥/信号）

---

## Phase 2

### 2.1 架构变化

Phase 1 使用 `aim_ll -t vm`——AIM 驱动预加载整个镜像文件到虚拟内存。我们的程序不在 I/O 路径中，无法追踪变更。

Phase 2 切换到 `aim_ll -t proxy`——AIM 驱动通过命名管道将所有 SCSI 请求转发给我们。我们的程序成为 I/O 路径上的存储后端，可以：
- **延迟加载**: 只在 Windows 实际请求时才从 VHD 读取扇区
- **增量保存**: 记录哪些扇区被写过，只写回变更部分

```
Phase 1:                                           Phase 2:
                                                   
 [VHD] → aim_ll 预加载 → [内核 RAM] ← Windows       [VHD] ←→ [StorageServer] ←pipe→ [AIM驱动] ← Windows
                                                   
 一次全量，不在 I/O 路径                           按需 + 增量，在 I/O 路径
```

### 2.2 StorageServer

新增模块 `src/storage_server.h/.cpp`，负责：

```
┌──────────────────────────────────────────────┐
│              StorageServer                    │
│                                               │
│  Run(pipeName, vhdPath)                      │
│    │                                          │
│    ├─ CreateNamedPipe("\\.\pipe\ramdisk")    │
│    ├─ aim_ll -a -t proxy -f \\.\pipe\ramdisk │
│    │                                          │
│    └─ 事件循环:                               │
│         while (connected) {                   │
│           ReadFile(pipe, request);            │
│           switch (request.op) {               │
│             case READ:                        │
│               if (!cache[sector])             │
│                 vhd.ReadSectors(sector);      │
│               WriteFile(pipe, cache[sector]); │
│             case WRITE:                       │
│               memcpy(cache[sector], data);    │
│               dirty[sector] = 1;              │
│               WriteFile(pipe, ACK);           │
│           }                                   │
│         }                                     │
│                                               │
│  DirtyBitmap                                   │
│    ├─ std::vector<uint8_t> (1 bit/sector)     │
│    ├─ Set(sector) → 标记脏                    │
│    ├─ Get(sector) → 查询脏                    │
│    └─ CountDirty() → 统计变更扇区             │
│                                               │
│  SectorCache                                   │
│    ├─ 仅缓存被访问过的扇区（稀疏）            │
│    └─ unordered_map<sectorIdx, sectorData>     │
│                                               │
│  VhdEngine (复用)                              │
│    ├─ ReadSectors: 从 VHD 加载到缓存          │
│    └─ WriteSectors: 增量保存时写回 VHD        │
└──────────────────────────────────────────────┘
```

**保存流程 (增量)**:
```
SaveNow():
  for each dirty sector:
    vhd.WriteSectors(sector, 1, cache[sector])
  dirty.Clear()
  Log("Incremental save: %d sectors (%s) in %.1fs", 
      dirtyCount, FormatSize(dirtyCount*512).c_str(), elapsed)
```

**保存流程 (首次/全量——脏块太多时回退)**:
```
if (dirty.Count() > totalSectors / 4):  // > 25% 变更
  vhd.WriteAll(fullBuffer)  // 全量更快
else:
  增量保存
```

### 2.3 数据流

**延迟加载（首次读扇区 X）**:
```
Windows 读 R:\file.txt → NTFS 读 MFT 扇区 → AIM 驱动发送 READ(X) via pipe
    → StorageServer: cache[X] 不存在 → VhdEngine::ReadSectors(X, 1, buf)
    → cache[X] = buf → 返回数据 via pipe → Windows 得到数据
```

**增量保存（定时触发）**:
```
SaveScheduler 触发 → StorageServer::SaveDirty()
    → 遍历 DirtyBitmap → 每个 dirty sector → VhdEngine::WriteSectors
    → 清除 bitmap → Log
```

**关机保存**: 同上，由 `SaveScheduler::RegisterShutdownHandler` 触发。

### 2.4 Pipe 协议提取方案

AIM proxy pipe 协议无公开文档。Phase 2 实施时使用 **CodeGraph** 分析 AIM 源码：

```
步骤:
1. codegraph_clone ArsenalRecon/Arsenal-Image-Mounter
2. codegraph_search "Devio" → 定位协议相关文件
3. codegraph_explore "DevioServer DevioClient pipe" → 提取帧格式
4. 定位: request header layout, response codes, operation types
```

预期发现的关键结构（从 AIM 源码层次推断）:
```
Devio/Server/  — 服务端抽象（我们实现这侧）
Devio/Client/  — 客户端（驱动侧）
协议层: 可能是简单的 length-prefixed 二进制帧
```

### 2.5 增量估算

以一个 4GB RAM 盘为例:

| 场景 | Phase 1 (全量) | Phase 2 (增量) |
|------|---------------|---------------|
| 盘装满 50% | 每次 4GB, ~30-60s | ~2GB, ~15-30s |
| 轻度编辑 (改几个文件) | 每次 4GB | ~几十MB, <1s |
| 首次保存 (全盘格式化后) | 4GB | 4GB (退回到全量路径) |
| 启动加载 | ~30-60s (全量预读) | ~即时 (按需加载, 启动秒级) |

### 2.6 看门狗 (Watchdog)

Proxy 模式下 StorageServer 是单点故障——如果进程崩溃，虚拟磁盘直接消失，Windows 报错，未保存数据永久丢失。

**设计**: 一个轻量级守护进程 `watchdog.exe`，独立的监控进程。

```
┌─ watchdog.exe ────────────────────────────┐
│                                             │
│  启动时:                                     │
│    1. CreateProcess(storage_server.exe)     │
│    2. 记录子进程 PID + 启动时间              │
│                                             │
│  运行中 (每 2 秒):                           │
│    1. WaitForSingleObject(hProcess, 0)      │
│       → 返回 WAIT_OBJECT_0 = 子进程已退出   │
│    2. 查询命名 pipe 是否仍然监听             │
│       CreateFile("\\\\.\\pipe\\ramdisk")      │
│       → 失败 = pipe 断开                    │
│                                             │
│  检测到崩溃:                                 │
│    1. 记录崩溃时间 + 写日志                  │
│    2. 检查是否有正在进行的保存:              │
│       - 有 → 等待保存锁释放或超时 (30s)     │
│       - 无 → 立即重启                        │
│    3. CreateProcess 重新启动 storage_server  │
│    4. 记录重启时间                           │
│    5. 如果 60 秒内崩溃超过 5 次 → 放弃      │
│       → 写错误日志 + 通知用户               │
│                                             │
│  退出:                                       │
│    - 收到 stop 信号 → 通知 server 退出       │
│    - 等待 server 优雅退出 (最多 10s)         │
│    - server 超时 → TerminateProcess          │
│    - watchdog 退出                           │
└─────────────────────────────────────────────┘
```

**日志示例**:
```
[09:30:00] Watchdog started, launching storage_server (PID 4216)
[09:30:01] Storage server online, pipe connected
[10:15:33] CRASH: storage_server (PID 4216) exited unexpectedly (code 0xC0000005)
[10:15:33] No save in progress, restarting...
[10:15:34] Storage server restarted (PID 8920)
[10:15:34] Pipe reconnected, device recovered
```

**文件**: `src/watchdog.h/.cpp`

**与 daemon 的关系**:
- Phase 1 `--daemon` 的后台守护是 Phase 1 的简易版
- Phase 2 watchdog 替换它——daemon 逻辑合并进 watchdog
- vm 模式下 watchdog 退化到 Phase 1 行为（无 pipe 检查）

**CLI 接口**:
```bash
ramdisk-cli create -s 4G -o D:\disk.vhd --mode proxy --daemon
# watchdog 自动启动，管理 storage_server 生命周期
```

---

## 实施顺序

```
Phase 1 (一次迭代):
  1.1 修复 VHD 数据完整性  ← 最先，正确性基础
  1.2 自动分配盘符          ← 第二，UX
  1.3 --daemon 后台模式     ← 第三，UX
  
Phase 2 (独立迭代):
  2.1 CodeGraph 分析 AIM 源码提取 proxy 协议
  2.2 实现 StorageServer
  2.3 实现 DirtyBitmap + SectorCache
  2.4 集成到 main.cpp (--mode proxy)
  2.5 端到端测试
```

---

## 与现有代码的关系

| 文件 | Phase 1 | Phase 2 |
|------|---------|---------|
| `vhd_engine.cpp` | **修改** — 数据完整性修复 | 不变 |
| `main.cpp` | **修改** — 盘符分配 + daemon 分发 | **修改** — `--mode proxy` 分支 |
| `aim_bridge.cpp` | 不变 | **修改** — 新增 `MountProxy()` |
| `save_scheduler.cpp` | 不变 | **修改** — 回调改为增量 |
| `utils.h/.cpp` | 不变 | **修改** — 新增 GetAvailableDriveLetter |
| `daemon.h/.cpp` | **新建** | 不变 |
| `storage_server.h/.cpp` | — | **新建** |
| `watchdog.h/.cpp` | — | **新建** |
| `dirty_bitmap.h/.cpp` | — | **新建** |
| `CLAUDE.md` | **修改** — 更新命令参考 | **修改** — proxy 模式文档 |

---

## 待确认项

- [ ] AIM proxy 协议结构（Phase 2 用 CodeGraph 分析后确定）
- [ ] 盘符分配方案：DeviceIoControl vs diskpart vs COM VDS（推荐 DeviceIoControl）
- [ ] daemon 退出信号机制：命名 Event vs 互斥锁轮询（推荐 Event）
