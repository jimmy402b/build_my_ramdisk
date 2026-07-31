# RamDisk CLI

基于 Arsenal Image Mounter 内核驱动 (phdskmnt.sys) 的 RAM 磁盘 CLI 工具。
将 VHD 虚拟磁盘文件加载到系统内存中，支持定时写回磁盘。

**对标**: Primo Ramdisk 核心功能  
**技术栈**: C++11, GCC MinGW-w64, CMake, VHD 内联实现  
**状态**: Phase 1 MVP (vm 模式, 全量写回)

## 前置依赖

- **Arsenal Image Mounter** / AIM Toolkit 已安装 (`phdskmnt.sys` + `aim_ll.exe`)
- `aim_ll.exe` 位于 `C:\Program Files\AIM Toolkit\` 或 `AIM_LL_PATH` 环境变量
- **必须以管理员权限运行**

## 命令参考

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
| `--log-file` | 日志文件路径 (默认 ramdisk-cli.log) |
| `--mode` | vm (默认) / proxy |

### mount — 加载已有 VHD

```bash
ramdisk-cli mount -i <vhd-path> [options]
```

| 参数 | 说明 |
|------|------|
| `-i, --input` | VHD 文件路径 |
| `--save-interval` | 定时保存间隔秒数 |
| `--readonly` | 只读模式 |
| `--label` | 卷标 |

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

## 退出码

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | VHD 文件错误 |
| 3 | AIM 驱动错误 |
| 4 | 系统错误 |

## 架构

```
main.cpp (CLI 入口 + 命令分发)
├── cli_parser    — 参数解析 (ParseArgs → ParseResult)
├── vhd_engine    — VHD 格式引擎 (内联实现, 动态 VHD 创建/读写/持久化)
├── aim_bridge    — aim_ll.exe 子进程桥接 (CreateProcess + stdout)
├── save_scheduler — 定时保存 + Ctrl+C 关机信号
├── storage_server — AIM proxy pipe 存储后端 (增量保存 + 延迟加载)
└── watchdog       — 进程守护 (崩溃重启 + 速率限制)
```

### 数据流

- **创建**: `VhdEngine::Create` → VHD 文件 → `AimBridge::MountVm` → RAM 盘
- **加载**: `VhdEngine::Open` → `AimBridge::MountVmFromFile` → RAM 盘
- **保存**: 定时/手动/关机 → `AimBridge::ReadDeviceData` (\\\\.\\PhysicalDriveN) → `VhdEngine::WriteAll` → VHD
- **卸载**: 先保存 → `AimBridge::Unmount` → `VhdEngine::Close`

## 构建

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
# 产物: build/src/ramdisk-cli.exe
```

## 故障排除

### "aim_ll.exe not found"
安装 AIM Toolkit 或设置 `AIM_LL_PATH` 环境变量

### "Administrator privileges required"
以管理员身份运行终端

### "Failed to mount RAM disk"
- `sc query phdskmnt` 确认驱动运行
- 确认 VHD 文件未被其他设备占用

### "Failed to write VHD"
- 检查磁盘空间
- 确认文件未被锁定

## 项目结构

```
build_my_ramdisk/
├── CMakeLists.txt
├── CLAUDE.md
├── task-list.md
├── src/
│   ├── main.cpp              # 入口 + 命令分发
│   ├── cli_parser.h/.cpp     # CLI 参数解析
│   ├── vhd_engine.h/.cpp     # VHD 引擎 (内联格式实现)
│   ├── aim_bridge.h/.cpp     # AIM 驱动桥接
│   ├── save_scheduler.h/.cpp # 保存调度器
│   └── utils.h/.cpp          # 日志/格式化
├── vendor/
├── docs/
│   └── superpowers/
│       ├── specs/2026-07-31-ramdisk-cli-design.md
│       └── plans/2026-07-31-ramdisk-cli-plan.md
└── .gitignore
```
