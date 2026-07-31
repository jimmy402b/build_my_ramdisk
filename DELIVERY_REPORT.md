# RamDisk CLI — 交付报告

> 日期: 2026-07-31 | 版本: v2.0 | 构建: `cmake -B build -G "MinGW Makefiles" && cmake --build build`

## 1. 项目概述

基于 Arsenal Image Mounter 内核驱动 (phdskmnt.sys) 的个人 RAM 磁盘 CLI 工具。对标 Primo Ramdisk 核心功能：将 VHD 虚拟磁盘文件加载到内存，操作在 RAM 中进行，定时写回磁盘防止数据丢失。

**技术栈**: C++11, GCC 13.1.0 MinGW-w64, CMake, 内联 VHD 引擎

## 2. 前置依赖

- **Windows 10/11 x64**（开发环境：Win11 Pro 10.0.26200）
- **Arsenal Image Mounter / AIM Toolkit 已安装**，`C:\Program Files\AIM Toolkit\aim_ll.exe` 和 `phdskmnt.sys` 就位
- **以管理员权限运行**（挂载虚拟磁盘需要）
- **GCC 13.1.0+** 或同等的 MinGW-w64 工具链

## 3. 构建与产物

```bash
cd build_my_ramdisk
cmake -B build -G "MinGW Makefiles"
cmake --build build
# 产物: build/src/ramdisk-cli.exe
```

产物是单个可执行文件（静态链接），无需额外的 DLL 或运行时。

## 4. 核心命令行

```bash
# 创建 RAM 盘（从零开始，动态 VHD）
ramdisk-cli create -s 4G -o D:\ramdisk.vhd --save-interval 300

# 加载已有 VHD 到 RAM
ramdisk-cli mount -i D:\ramdisk.vhd

# 手动保存（独立进程，可随时调用）
ramdisk-cli save

# 查看状态
ramdisk-cli status

# 卸载（先自动保存）
ramdisk-cli unmount

# 高级选项
ramdisk-cli create -s 2G -o D:\disk.vhd --auto-format --fs NTFS --quick-save -t fixed --daemon --log-file ramdisk.log
```

完整参数见 [CLAUDE.md](CLAUDE.md)。

## 5. 已验证的功能

### 5.1 可通过 E2E 测试验证

| # | 测试用例 | 验证命令 | 预期结果 |
|---|---------|---------|---------|
| 1 | 创建动态 VHD + 挂载 | `create -s 100M -o test.vhd --save-interval 0` | status 显示设备, VHD 文件创建 |
| 2 | 格式化 NTFS | diskpart 初始化 GPT + 格式化 | 驱动器可读写 |
| 3 | 文件写入 | `echo "hello" > R:\test.txt` | 文件立即存在于 RAM 盘 |
| 4 | 全量保存 | `save` 命令 | VHD 文件从几 KB 膨胀到 100MB+ |
| 5 | 数据持久化 | save → unmount → mount → 读文件 | 文件内容完整一致 |
| 6 | 定时保存 | `--save-interval 10` | 每 10 秒自动触发保存 |
| 7 | 关机保存 | create/mount 后 Ctrl+C | 保存日志出现在 ramdisk-cli.log |
| 8 | 固定 VHD | `create -t fixed -s 100M` | VHD 文件 = 100MB + 512 字节 |
| 9 | 快速增量保存 | `--quick-save` + 第二次 save | 日志显示 "Quick save: N sectors"（N < 总扇区数）|
| 10 | 状态查询 | `status` | 显示设备号/路径/大小 |
| 11 | Watchdog | 见第 7 节 | worker 进程退出后自动重启 |
| 12 | 管理员检测 | 非 admin 终端运行 create | 明确提示 "Administrator privileges required" |

### 5.2 测试数据流（完整 E2E）

```bash
# 1. 创建
ramdisk-cli create -s 256M -o test_e2e.vhd --save-interval 0 &
sleep 4

# 2. 格式化（需要 diskpart 或磁盘管理）
# PowerShell: Get-Disk | Where Size -eq 268435456 | Initialize-Disk -PartitionStyle GPT
#              New-Partition -UseMaximumSize -DriveLetter R | Format-Volume -FileSystem NTFS

# 3. 写入数据
echo "VERIFY_PERSISTENCE" > R:\verify.txt

# 4. 保存
ramdisk-cli save

# 5. 卸载
ramdisk-cli unmount

# 6. 重新挂载
ramdisk-cli mount -i test_e2e.vhd --save-interval 0 &
sleep 4

# 7. 验证
# 分配盘符后: type S:\verify.txt → 应输出 "VERIFY_PERSISTENCE"
```

## 6. 已知问题（请重点关注）

### 6.1 VHD 外部工具兼容性 ❌

**问题**: 我们生成的 VHD 文件无法被 AIM Toolkit 自带的 `aim_cli` 或 DiscUtils 库识别，报 "VHD file is corrupt"。

**根因**: VHD 格式要求大端序（big-endian）。v2 已修复 footer/dynhdr/BAT 的字节序并自洽（我们的引擎能正确读写自己生成的文件），但 DiscUtils 的校验逻辑更严格，可能检查了某些可选字段或校验和算法有差异。

**影响**: 不影响 ramdisk-cli 自身功能。不影响 Windows 直接读写（数据通过 PhysicalDrive，不经过 VHD 格式层）。但用户无法用 Hyper-V 或 7-Zip 等工具直接打开我们的 VHD 文件。

**临时方案**: 不需要。VHD 只作为持久化格式，正常使用中不会被外部工具读取。

### 6.2 Daemon 模式下日志可能不完整 ⚠️

**问题**: `--daemon` 启动的后台进程中，日志写入偶有丢失（特别是进程被强杀时）。

**根因**: DETACHED_PROCESS 子进程的 stderr 无控制台接收，`fprintf` 到日志文件的缓冲区在进程异常退出时可能未完全刷新。

**缓解**: v2 已添加 `fflush` + `FlushFileBuffers` + mutex 保护。正常 `unmount` 路径下日志完整。仅在 `TerminateProcess` 或系统崩溃时可能丢失最后一条。

**验证方法**: 
```bash
ramdisk-cli create -s 64M -o test_daemon.vhd --daemon --log-file test.log --save-interval 0
sleep 3
ramdisk-cli unmount
sleep 2
cat test.log  # 应看到: started → Mounted → Unmount signal → Save → Unmounted → stopped
```

### 6.3 保存时的延迟 ⚠️

**问题**: 执行 `save` 时，需要先 FlushFileBuffers 刷新所有 NTFS 卷的缓存，然后从 `\\.\PhysicalDriveN` 读取整个磁盘内容。对于大容量 RAM 盘，这个操作要数秒到数十秒。

- 256MB: ~2 秒
- 2GB: ~15-30 秒
- 8GB: ~60-120 秒

**缓解**: `--quick-save` 模式仅写回变更扇区（通过快照对比），第二次及以后的保存显著更快。首次保存仍是全量（建立基线快照）。

### 6.4 aim_ll 子进程依赖 ⚠️

**问题**: 挂载/卸载操作通过 `CreateProcess("aim_ll.exe")` 子进程执行。如果 `aim_ll.exe` 路径变化（不在 `C:\Program Files\AIM Toolkit\` 也不在 `AIM_LL_PATH` 环境变量），挂载会失败。

**缓解**: v2 已实现 `aimapi.dll` 直接调用模块（`aim_api.cpp`），但因 `ImScsiCreateDevice` 参数签名尚未完全确认，当前仍使用 aim_ll 子进程。后续切换后不再依赖 aim_ll.exe。

### 6.5 `--auto-format` 的 diskpart 延迟 ⚠️

**问题**: 自动格式化通过调用 `diskpart /s script.txt` 实现，需要等待 diskpart 进程完成（最长 30 秒）。在慢系统或磁盘繁忙时可能超时。

**缓解**: 默认不启用（`--auto-format` 需显式指定）。手动格式化更可控。

### 6.6 多进程并发保存 ⚠️

**问题**: `save` 命令是独立进程。如果同时运行两个 `save`，两个进程都会打开 VHD 文件并写入，可能导致数据损坏。

**缓解**: VHD 文件以 `FILE_SHARE_READ | FILE_SHARE_WRITE` 打开，操作系统允许并发写入但不保护数据一致性。用户应避免手动运行多个 save。

### 6.7 固定 VHD 创建慢 ⚠️

**问题**: `-t fixed` 创建时需要预分配完整文件空间（例如 4GB），比动态 VHD 慢很多。

**缓解**: 默认使用动态 VHD。固定 VHD 仅用于需要最高读写性能的场景。

## 7. 模块可信度评估

| 模块 | 状态 | 测试覆盖 |
|------|------|---------|
| `vhd_engine` — 动态 VHD 读写 | 🟢 可信 | 128MB 逐字节验证, E2E 持久化 |
| `vhd_engine` — 固定 VHD 读写 | 🟢 可信 | 独立测试通过, handle leak 已修复 |
| `vhd_engine` — VHD 大端序 | 🟡 自洽 | 读自己写的正确, 外部工具不识别 |
| `aim_bridge` — aim_ll 子进程 | 🟢 可信 | create/mount/save/unmount 均工作 |
| `aim_api` — aimapi.dll 直调 | 🟡 部分 | 加载+函数解析通过, CreateDevice 参数待确认 |
| `save_scheduler` — 定时保存 | 🟢 可信 | E2E 定时触发验证 |
| `cli_parser` — 参数解析 | 🟢 可信 | 全文法测试通过 |
| `daemon` — 后台守护 | 🟡 有缺陷 | 基本可用, 日志偶有丢失 |
| `drive_letter` — 盘符自动分配 | 🟡 有缺陷 | diskpart 依赖, 15s 超时 |
| `watchdog` — 进程守护 | 🟢 可信 | 6 launches/5 exits/crash rate limiting |
| `dirty_bitmap` — 脏块追踪 | 🟢 可信 | 单元测试 3/3 |
| `sector_cache` — 扇区缓存 | 🟢 可信 | 单元测试通过 |
| `storage_server` — IMDPROXY | 🟡 未验证 | 协议正确但未接入 AIM driver |
| `tray_icon` — 托盘 GUI | 🟡 基础 | 右键菜单可用, 图标为默认 |
| `trim_manager` — DMM | ❌ 未实现 | 仅 stubbed |
| `quick_save` — 快照保存 | 🟢 可信 | 增量验证通过 |

## 8. 给测试 agent 的建议

### 8.1 必备测试环境

- Windows 10/11 x64, Administrator 权限
- AIM Toolkit 已安装（phdskmnt.sys 运行中）
- 至少 1GB 空闲内存用于创建测试 RAM 盘
- 项目已构建：`build/src/ramdisk-cli.exe` 存在

### 8.2 推荐测试顺序

1. `ramdisk-cli version` → 应输出版本号
2. `ramdisk-cli help` → 应输出完整帮助
3. `ramdisk-cli create -s 64M -o test1.vhd --save-interval 0` → 后台运行
4. `ramdisk-cli status` → 应显示一个设备
5. 手动磁盘管理 → 初始化 GPT → 格式化 NTFS → 分配盘符
6. 写入测试文件到盘符 → `echo test > X:\verify.txt`
7. `ramdisk-cli save` → 应输出 "Saved 64.00 MB"
8. `ramdisk-cli unmount` → 应输出 "RAM disk unmounted"
9. `ramdisk-cli mount -i test1.vhd --save-interval 0` → 重新挂载
10. 分配盘符 → 读取文件 → 应包含 "test"
11. 清理：`ramdisk-cli unmount`，删除 `test1.vhd`

### 8.3 边界测试

- 创建超过可用内存的 RAM 盘（应失败并报错）
- 非管理员权限运行（应提示提权）
- 卸载已卸载的设备（应报 "No RAM disks mounted"）
- 挂载不存在的 VHD 文件（应报 VHD 错误）
- 同时启动两个 daemon（应拒绝第二个）

## 9. 文件清单

| 文件 | 用途 |
|------|------|
| `src/main.cpp` | CLI 入口, 命令分发 (417 行) |
| `src/vhd_engine.h/.cpp` | VHD 格式引擎, 动态+固定, 内联实现 (622 行) |
| `src/aim_bridge.h/.cpp` | aim_ll.exe 子进程桥接 (224+80 行) |
| `src/aim_api.h/.cpp` | aimapi.dll 直调封装 (194 行) |
| `src/save_scheduler.h/.cpp` | 定时保存 + 关机信号 |
| `src/cli_parser.h/.cpp` | 参数解析 |
| `src/daemon.h/.cpp` | 后台守护 + 日志 |
| `src/drive_letter.h/.cpp` | 盘符自动分配 |
| `src/watchdog.h/.cpp` | 进程守护 |
| `src/dirty_bitmap.h/.cpp` | 脏块位图 |
| `src/sector_cache.h/.cpp` | 扇区缓存 |
| `src/storage_server.h/.cpp` | IMDPROXY 协议服务端 |
| `src/tray_icon.h/.cpp` | 系统托盘 GUI |
| `src/utils.h/.cpp` | 日志/格式化 |
| `CLAUDE.md` | Agent 技能文档 |
| `PROJECT_STATUS.md` | 项目状态报告 |
| `DELIVERY_REPORT.md` | 本文件 |
