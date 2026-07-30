# RamDisk CLI — Design Spec

> 基于 Arsenal Image Mounter 内核驱动 (phdskmnt.sys) 的个人 RAM 磁盘工具
>
> 版本: v1.0-MVP | 日期: 2026-07-31 | 语言: C++11 | 平台: Windows 10/11 x64

---

## 1. 项目定位

个人使用工具，CLI 界面，MVP 优先。将 VHD 虚拟磁盘文件加载到内存中，支持定时写回磁盘。

**参考对标**: Primo Ramdisk 核心功能 + Arsenal Image Mounter 内核驱动

**分阶段策略**:

| 阶段 | 模式 | 功能 |
|------|------|------|
| Phase 1 (本 MVP) | `vm` 模式 — 预加载全量 | 创建/加载 VHD、定时保存、关机保存、手动保存 |
| Phase 2 (后续) | `proxy` 模式 — 按需加载 | 快速保存（增量）、延迟加载（按需读 VHD） |

---

## 2. 架构总览

```
ramdisk-cli.exe
├── CLI Parser      — 命令行参数解析与命令分发
├── VhdEngine       — VHD 文件创建/打开/扇区读写（基于 MiniVHD）
├── SaveScheduler   — 定时器 + 关机信号 + 手动触发写回
└── AimBridge       — 调用 aim_ll.exe 子进程管理 RAM 盘挂载/卸载
         │
         ▼
   aim_ll.exe ──► phdskmnt.sys (WHCP 签名内核驱动)
```

### 数据流

```
[创建] VhdEngine.Create → VHD 文件 → AimBridge.MountVm → RAM 盘 ∴ Windows 可读写
[保存] SaveScheduler 触发 → AimBridge.ReadDeviceData → VhdEngine.WriteAll → VHD 文件
[卸载] SaveScheduler 先保存 → AimBridge.Unmount → VHD 文件完整
```

### 技术选型

| 项目 | 选择 | 说明 |
|------|------|------|
| 语言 | C++11 | GCC 13.1.0 / MinGW-w64 (CLion 自带) |
| 构建 | CMake 3.16+ | 跨 IDE |
| VHD 库 | MiniVHD (C) | shermp/MiniVHD, `extern "C"` 集成 |
| 驱动交互 | aim_ll.exe 子进程 | 命令行 + stdout 解析 |
| 驱动文件 | phdskmnt.sys | 已安装于 Windows\system32\drivers, WHCP 签名 |
| 测试环境 | HVCI=ON, SecureBoot=OFF | 已验证驱动正常加载 |

---

## 3. CLI 接口

```
ramdisk-cli.exe <command> [options]
```

### 3.1 create — 创建新 VHD 并挂载为 RAM 盘

```bash
ramdisk-cli create -s <size> -o <vhd-path> [options]
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `-s, --size` | ✅ | 磁盘大小，如 `512M`, `4G`, `1T` |
| `-o, --output` | ✅ | VHD 文件输出路径 |
| `-t, --type` | ❌ | VHD 类型：`fixed` / `dynamic`，默认 `dynamic` |
| `--save-interval` | ❌ | 定时保存间隔（秒），默认 `300`（5分钟），`0`=仅手动保存 |
| `--label` | ❌ | 卷标，默认 `RAMDISK` |
| `--mount-point` | ❌ | 指定盘符如 `X:`，默认自动分配 |

**示例**:
```bash
ramdisk-cli create -s 4G -o D:\ramdisk.vhd --save-interval 600 --label WorkDrive
```

### 3.2 mount — 加载已有 VHD 为 RAM 盘

```bash
ramdisk-cli mount -i <vhd-path> [options]
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `-i, --input` | ✅ | VHD 文件路径 |
| `--save-interval` | ❌ | 定时保存间隔（秒），默认读取上次保存的配置 |
| `--readonly` | ❌ | 只读模式 |
| `--label` | ❌ | 卷标 |

**示例**:
```bash
ramdisk-cli mount -i D:\ramdisk.vhd --save-interval 120
```

### 3.3 save — 手动触发保存

```bash
ramdisk-cli save [options]
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--device` | ❌ | 设备号，0=第一个设备，默认 0 |

### 3.4 unmount — 保存并卸载

```bash
ramdisk-cli unmount [options]
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--device` | ❌ | 设备号，默认 0 |
| `--no-save` | ❌ | 跳过保存直接卸载 |

### 3.5 status — 查看状态

```bash
ramdisk-cli status
```

**输出格式**:
```
[0] D:\ramdisk.vhd → R: (4.00 GB, dynamic) | save: 600s | last: 2min ago | state: mounted
```

### 全局选项

| 参数 | 说明 |
|------|------|
| `--verbose, -v` | 详细输出 |
| `--help, -h` | 帮助信息 |
| `--version` | 版本号 |

### 退出码

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | VHD 文件错误（打不开/损坏/空间不足） |
| 3 | AIM 驱动错误（驱动未安装/权限不足/挂载失败） |
| 4 | 系统错误 |

---

## 4. VhdEngine — VHD 文件引擎

### 4.1 接口

```cpp
class VhdEngine {
public:
    bool Create(const std::string& path, uint64_t sizeBytes, bool dynamic = true);
    bool Open(const std::string& path, bool writable = true);
    void Close();

    bool ReadSectors(uint64_t sectorOffset, uint32_t sectorCount, void* buffer);
    bool WriteSectors(uint64_t sectorOffset, uint32_t sectorCount, const void* buffer);

    uint64_t GetTotalSectors() const;
    uint64_t GetSizeBytes() const;
    bool IsDynamic() const;
    bool IsOpen() const;

    // Phase 1 使用的全量读写
    bool ReadAll(void* buffer);
    bool WriteAll(const void* buffer);

    // 错误信息
    std::string GetLastError() const;
};
```

### 4.2 MiniVHD 集成

| MiniVHD C API | VhdEngine 方法 |
|--------------|---------------|
| `mvhd_open()` | `Open()` |
| `mvhd_create()` | `Create()` |
| `mvhd_read_sectors()` | `ReadSectors()` |
| `mvhd_write_sectors()` | `WriteSectors()` |
| `mvhd_close()` | `Close()` |

- MiniVHD 编译为静态库 `libminivhd.a`，通过 CMake `add_subdirectory` 集成
- `extern "C"` 声明头文件，C++ 侧不暴露 C 类型
- 扇区大小固定 512 字节（VHD 规范）
- 内部持有一个互斥锁，保护并发读写（未来 proxy 模式需要）

### 4.3 支持的文件类型

| VHD 类型 | Create | Open | 说明 |
|----------|--------|------|------|
| Fixed (type=2) | ✅ | ✅ | 文件大小 = 磁盘大小，创建慢但性能好 |
| Dynamic (type=3) | ✅ | ✅ | 文件按需增长，创建快，MVP 默认选项 |
| Differencing (type=4) | ❌ | ❌ | Phase 2 考虑 |

---

## 5. SaveScheduler — 保存调度器

### 5.1 接口

```cpp
class SaveScheduler {
public:
    using SaveCallback = std::function<bool()>;

    bool Start(int intervalSeconds);
    void Stop();
    bool SaveNow();
    void RegisterShutdownHandler();

    time_t GetLastSaveTime() const;
    bool IsRunning() const;
    int  GetInterval() const;

    void SetSaveCallback(SaveCallback callback);
};
```

### 5.2 定时器

- 独立后台线程 + `std::condition_variable` 实现可中断等待
- `intervalSeconds=0` 时不启动定时线程，仅响应手动和关机触发
- 默认间隔 300 秒（5 分钟）

### 5.3 关机保存

| 机制 | 场景 | 实现 |
|------|------|------|
| `SetConsoleCtrlHandler` | Ctrl+C / 关闭控制台窗口 / 系统关机 | PHANDLER_ROUTINE 回调 |
| 对象析构 | `main()` 正常 return / 栈展开 | `~SaveScheduler()` 调用 `Stop()` + `SaveNow()` |

### 5.4 线程安全

- `SaveNow()` 入口 `std::lock_guard<std::mutex>`，保证同一时刻只有一个保存任务执行
- 定时器触发时如果上一次保存仍在执行，跳过本次（不排队）

### 5.5 保存流程

```
SaveNow()
  ├── 1. AimBridge::ReadDeviceData(devNum, size, buffer)  // 从 RAM 盘读全量
  ├── 2. VhdEngine::WriteAll(buffer)                      // 写回 VHD
  ├── 3. lastSaveTime = time(nullptr)
  └── 4. 控制台输出: "Saved D:\ramdisk.vhd (4.00 GB) at 14:30:22"
```

---

## 6. AimBridge — 驱动桥接层

### 6.1 接口

```cpp
class AimBridge {
public:
    static bool IsAvailable();

    bool MountVm(const std::string& vhdPath, uint64_t sizeBytes,
                 std::string& outMountPoint);
    bool MountVmFromFile(const std::string& vhdPath, std::string& outMountPoint);
    bool Unmount(int deviceNumber, bool force = false);

    struct DeviceInfo {
        int number;
        std::string mountPoint;     // 盘符如 "R:"
        uint64_t sizeBytes;
        std::string backingFile;
    };
    std::vector<DeviceInfo> ListDevices();

    // Phase 1: 读 RAM 盘全量数据用于写回
    bool ReadDeviceData(int deviceNumber, uint64_t sizeBytes, void* buffer);

    static std::string FindAimLl();
};
```

### 6.2 aim_ll 命令映射

| Bridge 方法 | aim_ll 命令 |
|------------|------------|
| `MountVm` | `aim_ll -a -t vm -f <vhd> -s <size>` |
| `MountVmFromFile` | `aim_ll -a -t vm -f <vhd>` |
| `Unmount` | `aim_ll -d -u <devnum>` |
| `ListDevices` | `aim_ll -l`，解析文本输出 |
| `ReadDeviceData` | `CreateFile("\\\\.\\PhysicalDriveN")` + `ReadFile` |

### 6.3 子进程调用

- `CreateProcess` 启动 `aim_ll.exe`，捕获 stdout/stderr
- 超时 30 秒（创建大固定 VHD 可能慢）
- 解析 aim_ll 输出获取设备号和错误信息
- 路径查找顺序:
  1. 环境变量 `AIM_LL_PATH`
  2. `C:\Program Files\AIM Toolkit\aim_ll.exe`
  3. 当前目录 `.\aim_ll.exe`
  4. PATH 搜索

### 6.4 ReadDeviceData 实现

挂载后 AIM 驱动创建 `\\.\PhysicalDriveN` 块设备句柄：

```cpp
bool AimBridge::ReadDeviceData(int devNum, uint64_t sizeBytes, void* buffer) {
    // 找到设备路径
    auto devices = ListDevices();
    std::string physPath = "\\\\.\\PhysicalDrive" + std::to_string(devNum);

    HANDLE h = CreateFileA(physPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD bytesRead;
    BOOL ok = ReadFile(h, buffer, static_cast<DWORD>(sizeBytes), &bytesRead, nullptr);
    CloseHandle(h);
    return ok && bytesRead == sizeBytes;
}
```

### 6.5 错误处理

| 错误场景 | 检测方式 | 用户提示 |
|----------|---------|---------|
| aim_ll 未找到 | `FindAimLl()` 返回空 | "aim_ll.exe not found. Install AIM Toolkit or set AIM_LL_PATH." |
| 权限不足 | aim_ll 输出 "Access denied" 或进程返回非 0 | "Administrator privileges required." |
| 设备占用 | `Unmount` 失败 | "Device is in use. Close all applications using the disk first." |
| VHD 文件无效 | `MountVmFromFile` 失败 | "Invalid or corrupted VHD file: <path>" |

---

## 7. 项目结构

```
build_my_ramdisk/
├── CMakeLists.txt
├── CLAUDE.md                         # Agent 技能文档
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp                      # CLI 入口
│   ├── cli_parser.h / .cpp           # 参数解析
│   ├── vhd_engine.h / .cpp           # VHD 引擎
│   ├── save_scheduler.h / .cpp       # 保存调度
│   ├── aim_bridge.h / .cpp           # 驱动桥接
│   └── utils.h / .cpp                # 日志/工具
├── vendor/
│   └── MiniVHD/                      # git submodule
├── docs/
│   └── superpowers/
│       └── specs/
│           └── 2026-07-31-ramdisk-cli-design.md   # 本文件
└── .claude/
    └── settings.json
```

---

## 8. 依赖

| 依赖 | 版本 | 用途 | 安装方式 |
|------|------|------|---------|
| GCC (MinGW-w64) | 13.1.0+ | C++11 编译 | CLion 自带 `C:\Program Files\JetBrains\CLion 2026.1\bin\mingw\bin\` |
| CMake | 3.16+ | 构建系统 | 系统已安装 |
| MiniVHD | latest | VHD 读写 | git submodule `vendor/MiniVHD` |
| AIM Toolkit | 已安装 | aim_ll.exe + phdskmnt.sys | `C:\Program Files\AIM Toolkit\` |

---

## 9. 构建与运行

```bash
# 构建
cmake -B build -G "MinGW Makefiles"
cmake --build build

# 运行（需要管理员权限）
./build/ramdisk-cli.exe create -s 4G -o D:\test.vhd

# 查看状态
./build/ramdisk-cli.exe status
```

---

## 10. 错误码

| 码 | 常量 | 含义 |
|----|------|------|
| 0 | `OK` | 成功 |
| 1 | `ERR_ARGS` | 参数错误 |
| 2 | `ERR_VHD` | VHD 文件错误 |
| 3 | `ERR_AIM` | AIM 驱动错误 |
| 4 | `ERR_SYSTEM` | 系统错误（内存不足、文件权限等） |

---

## 11. Phase 2 预留

Phase 2 切换到 `aim_ll -t proxy` 模式后新增:

- **增量保存 (Quick Save)**: 维护脏块位图，仅写回变更扇区
- **延迟加载 (Delayed Load)**: 实现 AIM proxy 协议存储服务端，按需从 VHD 读取扇区
- **多设备支持**: 同时管理多个 RAM 盘
- **内存模式**: 纯内存盘（不关联 VHD 文件，用于临时数据）

---

## 12. 待确认项

- [x] 编程语言 C++11
- [x] 编译器 GCC 13.1.0 MinGW
- [x] CLI 接口设计
- [x] VHD 格式兼容（MiniVHD）
- [x] 全功能保存模式（定时 + 关机 + 手动）
- [x] Phase 1 vm 模式 / Phase 2 proxy 模式
- [x] MiniVHD 许可证 — MIT，完全兼容
- [x] AIM 再分发 — 本项目不捆绑 AIM 组件，仅调用已安装的 aim_ll.exe，符合 Free Mode 使用条款
