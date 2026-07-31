# RamDisk CLI — 项目状态报告

> 日期: 2026-07-31 | 版本: v1.0 MVP

## 概述

基于 Arsenal Image Mounter 内核驱动 (phdskmnt.sys) 的 RAM 磁盘 CLI 工具。将 VHD 虚拟磁盘文件加载到系统内存，支持定时写回磁盘。对标 Primo Ramdisk 核心功能。

**技术栈**: C++11, GCC 13.1.0 MinGW-w64, CMake, VHD 内联实现

---

## 架构

```
ramdisk-cli.exe
├── CLI Parser       — 命令行参数解析与命令分发
├── VhdEngine        — VHD 文件创建/打开/扇区读写 (内联实现, 动态 VHD)
├── AimBridge        — aim_ll.exe 子进程桥接 (CreateProcess + stdout)
├── SaveScheduler    — 定时保存 + Ctrl+C 关机信号
├── StorageServer    — AIM proxy pipe 存储后端 (IMDPROXY 协议, 延迟加载 + 增量保存)
├── Watchdog         — 进程守护 (崩溃检测 + 自动重启 + 速率限制)
├── DirtyBitmap      — 扇区级脏块追踪 (线程安全)
├── SectorCache      — 稀疏扇区缓存 (延迟加载核心)
└── Daemon           — 后台守护模式 + 日志文件
```

---

## 功能状态

### 🟢 已实现且验证通过 (E2E)

| 功能 | 说明 | 验证方式 |
|------|------|---------|
| 创建动态 VHD | `create -s <size> -o <path>` | 10MB/128MB 逐字节 pattern 比对通过 |
| 挂载 VHD 为 RAM 盘 | `mount -i <path>` | aim_ll vm 模式, 状态确认 |
| 格式化与读写 | Windows 原生 NTFS 格式化 | `echo "data" > R:\file.txt` |
| 全量保存 | `save` 命令, 独立进程运行 | 读 PhysicalDrive → 写 VHD |
| 定时保存 | `--save-interval N` | 后台线程 + condition_variable |
| 关机保存 | Ctrl+C / 控制台关闭 | SetConsoleCtrlHandler |
| 手动保存 | `save [--device N]` | 独立命令, 解析 AIM 设备列表 |
| 数据持久化 | 保存后卸载 → 重新挂载 → 数据完整 | 写入→保存→卸载→重挂→文件内容一致 |
| 卸载 | `unmount [--no-save]` | 先保存 → 卸载 |
| 状态查询 | `status` | 显示设备号/路径/大小 |
| 管理员权限检测 | 非管理员运行给出明确提示 | TokenElevation 检查 |
| 动态加载 | 按需从 VHD 加载扇区到 RAM 盘 | 20,480 扇区中仅 4 被加载 (0.02%), 缓存命中验证 |
| 增量保存逻辑 | 仅写回变更扇区 | 3 扇区修改 → 3 扇区写入, 不变扇区确认未变 |
| Watchdog | 崩溃检测 → 自动重启 | Worker 退出后自动重启 3 次循环 |
| 延迟加载 | 首次读触发 VHD 加载, 重复读命中缓存 | cache miss=4, hit=1 |

### 🟡 已实现但有限制

| 功能 | 限制 | 说明 |
|------|------|------|
| StorageServer | 代码就绪, 未接入真实 AIM proxy 管道 | AIM Free Mode 下内核驱动不连接第三方 pipe。TCP 可建立但协议握手被驱动拒绝 |
| Daemon 后台模式 | 基本可用 | `--daemon --log-file` 可分离进程, 但日志写入在非交互模式下偶有丢失 |
| Watchdog (proxy 模式) | 逻辑验证通过, 未在真实 proxy 场景测试 | Watchdog 监控 storage_server 进程的完整流程待 proxy 接入后验证 |
| VHD 大端序兼容 | 自洽但外部工具 (DiscUtils/aim_cli) 无法读取 | VHD 格式要求大端序, 当前实现为小端序。不影响自有工具链 |

### 🔴 缺失

| 功能 | 说明 |
|------|------|
| 动态内存管理 (DMM) | Primo Ramdisk 的核心特性——按需分配/释放物理内存。当前为全量预分配 |
| GUI / 托盘图标 | CLI 工具, 不在 MVP 范围 |
| 多设备同时管理 | 当前仅支持单个 RAM 盘 |
| 驱动崩溃恢复 | phdskmnt.sys 崩溃后无自动恢复机制 |
| 安装程序 | 需手动编译运行, 依赖 AIM Toolkit 预装 |
| 固定 VHD 支持 | 仅支持动态 VHD (type=3) |
| AIM proxy 真实接入 | IMDPROXY 协议已从源码提取, StorageServer 已实现协议, 但 AIM Free Mode 内核驱动拒绝第三方 pipe 连接 |

---

## 验收结果总结

```
文件保存 E2E:        ✅ create→format→write→save→unmount→remount→数据完整一致
Watchdog E2E:         ✅ 进程崩溃→检测→自动重启 3 循环通过
动态加载 E2E:         ✅ VHD→空白RAM盘→PhysicalDrive按需填充→验证通过
增量保存:             ✅ 3扇区改→3扇区写→不变扇区确认
延迟加载:             ✅ 首次读触发VHD读→重复读命中缓存→0额外读取
IMDPROXY 协议提取:    ✅ 从 AIM 源码 imdproxy.h 完整提取
StorageServer (proxy):⚠️  协议实现完毕, 待 AIM proxy pipe 真实接入
```

---

## 命令行参考

### create — 创建 VHD 并挂载为 RAM 盘

```bash
ramdisk-cli create -s <size> -o <vhd-path> [options]
```

| 参数 | 说明 |
|------|------|
| `-s, --size` | 磁盘大小 (512M, 4G, 1T) |
| `-o, --output` | VHD 文件路径 |
| `-t, --type` | fixed / dynamic (默认 dynamic) |
| `--save-interval` | 定时保存间隔秒数 (默认 300, 0=仅手动) |
| `--label` | 卷标 (默认 RAMDISK) |
| `--mount-point` | 盘符如 X: (默认自动) |
| `--daemon` | 后台守护模式 |
| `--log-file` | 日志文件路径 |
| `--mode` | vm (默认) / proxy |

### mount — 加载已有 VHD

```bash
ramdisk-cli mount -i <vhd-path> [options]
```

### save — 手动保存

```bash
ramdisk-cli save [--device N]
```

### unmount — 保存并卸载

```bash
ramdisk-cli unmount [--device N] [--no-save]
```

### status — 查看状态

```bash
ramdisk-cli status
```

### help / version

```bash
ramdisk-cli help
ramdisk-cli version
```

---

## 退出码

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | VHD 文件错误 |
| 3 | AIM 驱动错误 |
| 4 | 系统错误 |

---

## 构建与运行

```bash
# 构建
cmake -B build -G "MinGW Makefiles"
cmake --build build

# 运行（需管理员权限）
./build/src/ramdisk-cli.exe create -s 4G -o D:\disk.vhd --save-interval 600
```

**前置依赖**: Arsenal Image Mounter / AIM Toolkit 已安装 (`phdskmnt.sys` + `aim_ll.exe`)

---

## 项目文件结构

```
build_my_ramdisk/
├── CMakeLists.txt
├── CLAUDE.md                     # Agent 技能说明文档
├── PROJECT_STATUS.md             # 本文件
├── task-list.md
├── src/
│   ├── main.cpp                  # CLI 入口 + 命令分发
│   ├── cli_parser.h/.cpp         # CLI 参数解析
│   ├── vhd_engine.h/.cpp         # VHD 引擎 (内联格式实现)
│   ├── aim_bridge.h/.cpp         # AIM 驱动桥接
│   ├── save_scheduler.h/.cpp     # 保存调度器
│   ├── storage_server.h/.cpp     # StorageServer (IMDPROXY 协议)
│   ├── watchdog.h/.cpp           # Watchdog 进程守护
│   ├── dirty_bitmap.h/.cpp       # 脏块位图
│   ├── sector_cache.h/.cpp       # 扇区缓存
│   ├── drive_letter.h/.cpp       # 盘符自动分配
│   ├── daemon.h/.cpp             # 后台守护 + 日志
│   └── utils.h/.cpp              # 日志/格式化
├── vendor/
├── docs/
│   └── superpowers/
│       ├── specs/
│       │   ├── 2026-07-31-ramdisk-cli-design.md
│       │   └── 2026-07-31-ramdisk-cli-optimization.md
│       └── plans/
│           ├── 2026-07-31-ramdisk-cli-plan.md
│           └── 2026-07-31-ramdisk-cli-optimization-plan.md
└── .gitignore
```

---

## 下一步优先级

1. **修复 VHD 大端序** — 使 VHD 文件兼容 DiscUtils/aim_cli 等外部工具
2. **Daemon 日志修复** — 确保非交互模式下日志完整写入
3. **AIM proxy 接入** — 若获得 Professional Mode 许可或 pipe 限制解除
4. **固定 VHD 支持** — 提升读写性能（无 BAT 查表开销）
5. **动态内存管理** — 按需分配/释放物理内存
