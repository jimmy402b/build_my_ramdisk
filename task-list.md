# RamDisk CLI — Task Checklist

> 从 [2026-07-31-ramdisk-cli-plan.md](2026-07-31-ramdisk-cli-plan.md) 提取，纯 checklist，实施时逐一打勾。

---

## Task 1: 项目脚手架

- [ ] 1.1 创建 `CMakeLists.txt`（根，project ramdisk-cli, C11/CXX11, add_subdirectory vendor+src）
- [ ] 1.2 创建 `src/CMakeLists.txt`（executable + link minivhd）
- [ ] 1.3 创建 `vendor/CMakeLists.txt`（placeholder static library minivhd）
- [ ] 1.4 创建 `src/main.cpp`（最小 stub：printf 版本号，return 0）
- [ ] 1.5 创建 `.gitignore`（build/, *.o, *.exe, *.a）
- [ ] 1.6 **验证**: `cmake -B build -G "MinGW Makefiles" && cmake --build build` 成功
- [ ] 1.7 **验证**: `./build/ramdisk-cli.exe` 输出 `ramdisk-cli v1.0.0`
- [ ] 1.8 提交：`git init && git add CMakeLists.txt src/ vendor/ .gitignore && git commit -m "feat: project scaffolding with CMake + MinGW"`

---

## Task 2: MiniVHD 集成

- [ ] 2.1 克隆 submodule: `git submodule add https://github.com/shermp/MiniVHD.git vendor/MiniVHD`
- [ ] 2.2 检查 MiniVHD 源文件: `ls vendor/MiniVHD/*.c vendor/MiniVHD/*.h`
- [ ] 2.3 更新 `vendor/CMakeLists.txt`（`file(GLOB MINIVHD_SOURCES .../*.c)` → `add_library(minivhd STATIC ...)`, C99, -w）
- [ ] 2.4 创建 `src/vhd_engine.h`（前向声明，空壳 class）
- [ ] 2.5 **验证**: `cmake -B build -G "MinGW Makefiles" && cmake --build build` 成功
- [ ] 2.6 **验证**: `ls -la build/vendor/libminivhd.a` 文件存在，非零大小
- [ ] 2.7 提交：`git add vendor/CMakeLists.txt vendor/MiniVHD src/vhd_engine.h .gitmodules && git commit -m "feat: add MiniVHD submodule and static library target"`

---

## Task 3: Utils 模块

- [ ] 3.1 创建 `src/utils.h`（LogInfo/LogError, GetLastWinErrorMessage, FormatSize, FormatTime, FormatTimeAgo, Trim, StartsWith）
- [ ] 3.2 创建 `src/utils.cpp`（完整实现：stderr 输出, FormatMessageA, snprintf, localtime/strftime, string find/rfind）
- [ ] 3.3 **验证**: `cmake --build build` 编译成功
- [ ] 3.4 **验证**: 临时改 main.cpp 调用所有函数，确认输出格式正确
- [ ] 3.5 还原 main.cpp 为 stub
- [ ] 3.6 提交：`git add src/utils.h src/utils.cpp && git commit -m "feat: add utils module (logging, formatting, string helpers)"`

---

## Task 4: VhdEngine

- [ ] 4.1 更新 `src/vhd_engine.h`（完整接口：Create/Open/Close, ReadSectors/WriteSectors, ReadAll/WriteAll, GetSizeBytes/GetTotalSectors/IsDynamic/IsOpen/GetPath/GetLastError, pImpl 模式，禁止拷贝）
- [ ] 4.2 创建 `src/vhd_engine.cpp`（extern "C" miniVHD, MVHDHandle 封装, 所有方法实现）
- [ ] 4.3 **验证**: `grep -E "MVHDHandle|mvhd_" vendor/MiniVHD/minivhd.h` 核对 API 名称，如有差异修正
- [ ] 4.4 **验证**: `cmake --build build` 编译成功
- [ ] 4.5 **验证**: 临时改 main.cpp → 创建 10MB 动态 VHD → 写 0xAB 到扇区0 → 读回 → 比对 → 成功 → 删除测试文件
- [ ] 4.6 还原 main.cpp 为 stub
- [ ] 4.7 提交：`git add src/vhd_engine.h src/vhd_engine.cpp && git commit -m "feat: add VhdEngine module wrapping MiniVHD"`

---

## Task 5: AimBridge

- [ ] 5.1 创建 `src/aim_bridge.h`（DeviceInfo struct, IsAvailable, MountVm, MountVmFromFile, Unmount, ListDevices, ReadDeviceData, FindAimLl, RunAimLl, FindPhysicalDrivePath）
- [ ] 5.2 创建 `src/aim_bridge.cpp`（FindAimLl: ENV→默认路径→当前目录; RunAimLl: CreateProcess+管道, 30s超时; MountVm/MountVmFromFile: aim_ll -a -t vm; Unmount: aim_ll -d/-D; ListDevices: 解析 -l 输出; ReadDeviceData: CreateFile \\.\PhysicalDriveN + ReadFile 分块64MB）
- [ ] 5.3 **验证**: `cmake --build build` 编译成功
- [ ] 5.4 **验证**: 临时改 main.cpp → FindAimLl 非空 → IsAvailable true
- [ ] 5.5 还原 main.cpp 为 stub
- [ ] 5.6 提交：`git add src/aim_bridge.h src/aim_bridge.cpp && git commit -m "feat: add AimBridge module (aim_ll.exe subprocess bridge)"`

---

## Task 6: SaveScheduler

- [ ] 6.1 创建 `src/save_scheduler.h`（SaveCallback, Start/Stop/SaveNow/RegisterShutdownHandler, GetLastSaveTime/IsRunning/GetInterval/SetSaveCallback, pImpl）
- [ ] 6.2 创建 `src/save_scheduler.cpp`（定时线程 + condition_variable, 全局 static g_activeScheduler + ConsoleCtrlHandler, mutex 保护 SaveNow, Stop 时 join 线程 + 反注册 handler）
- [ ] 6.3 **验证**: `cmake --build build` 编译成功
- [ ] 6.4 **验证**: 临时改 main.cpp → 设置回调 → 手动 SaveNow → Start(2s) → sleep 5s → 至少看到 2 次 auto-save 日志 → Stop → 验证 lastSaveTime
- [ ] 6.5 还原 main.cpp 为 stub
- [ ] 6.6 提交：`git add src/save_scheduler.h src/save_scheduler.cpp && git commit -m "feat: add SaveScheduler module (timer + shutdown handler)"`

---

## Task 7: CLI Parser

- [ ] 7.1 创建 `src/cli_parser.h`（Command enum, CreateOptions/MountOptions/SaveOptions/UnmountOptions/ParseResult structs, ParseArgs/PrintHelp/PrintVersion 声明）
- [ ] 7.2 创建 `src/cli_parser.cpp`（ParseSize: B/K/M/G/T 后缀解析; ParseArgs: argv 遍历+switch 分发; PrintHelp: 完整帮助文本; PrintVersion: 版本号）
- [ ] 7.3 **验证**: `cmake --build build` 编译成功
- [ ] 7.4 **验证**: 临时改 main.cpp → 测试所有命令组合：
  - `create -s 4G -o D:\test.vhd` → 解析出 Create, size=4294967296
  - `create -s 512M -o test.vhd -t fixed --save-interval 0` → dynamic=0, interval=0
  - `mount -i D:\test.vhd --readonly` → 解析出 Mount, readonly
  - `status` → 解析出 Status
  - `help` → 打印帮助
  - `bogus` → PARSE ERROR
  - `create -o x.vhd` → PARSE ERROR（缺 --size）
- [ ] 7.5 还原 main.cpp 为 stub
- [ ] 7.6 提交：`git add src/cli_parser.h src/cli_parser.cpp && git commit -m "feat: add CLI parser module"`

---

## Task 8: main.cpp 集成

- [ ] 8.1 重写 `src/main.cpp`（全局 VhdEngine/AimBridge/SaveScheduler, DoSave 回调, CmdCreate/CmdMount/CmdSave/CmdUnmount/CmdStatus 函数, main 入口分发）
- [ ] 8.2 **验证**: `cmake --build build` 编译成功
- [ ] 8.3 **验证**: `ls -la build/ramdisk-cli.exe` 文件 > 100KB
- [ ] 8.4 **验证**: `./build/ramdisk-cli.exe help` 打印完整帮助
- [ ] 8.5 **验证**: `./build/ramdisk-cli.exe version` 输出 `ramdisk-cli v1.0.0`
- [ ] 8.6 提交：`git add src/main.cpp && git commit -m "feat: wire main.cpp with full command dispatch"`

---

## Task 9: CLAUDE.md

- [ ] 9.1 创建 `CLAUDE.md`（概述、前置依赖、命令参考含表格、退出码、架构图 ASCII、构建命令、故障排除、文件结构）
- [ ] 9.2 提交：`git add CLAUDE.md && git commit -m "docs: add CLAUDE.md agent skill documentation"`

---

## Task 10: 端到端测试（需管理员权限）

- [ ] 10.1 以管理员身份打开终端
- [ ] 10.2 从 clean 构建: `cmake -B build -G "MinGW Makefiles" && cmake --build build`
- [ ] 10.3 **create 流程**: `ramdisk-cli create -s 100M -o test_e2e.vhd --save-interval 0`
  - 预期: Creating VHD → Mounting → RAM disk ready: ? (test_e2e.vhd)
  - 按 Ctrl+C → 预期: 保存完成，正常退出
- [ ] 10.4 **验证 VHD 文件**: `ls -la test_e2e.vhd` → 文件存在，非零
- [ ] 10.5 **mount 流程**: `ramdisk-cli mount -i test_e2e.vhd --save-interval 0`
  - 预期: Opening VHD → Mounting → RAM disk ready
  - 按 Ctrl+C → 保存完成，退出
- [ ] 10.6 **status**: `ramdisk-cli status` → `No RAM disks mounted.`
- [ ] 10.7 清理: `rm test_e2e.vhd`
- [ ] 10.8 提交（如有修复）：`git add -u && git commit -m "fix: end-to-end test fixes"`

---

## 完成标准

- [ ] `build/ramdisk-cli.exe` 可执行，`help`/`version` 正常
- [ ] `create -s 100M -o test.vhd` 创建 VHD + 挂载 RAM 盘 + Ctrl+C 保存
- [ ] `mount -i test.vhd` 加载已有 VHD + Ctrl+C 保存
- [ ] `status` 显示无挂载设备
- [ ] `save` 手动触发保存（运行中会话内）
- [ ] `unmount` 保存并卸载
- [ ] 退出码正确（0=成功, 1=参数, 2=VHD, 3=AIM）
