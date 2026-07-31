# RamDisk CLI v2 — Primo 级体验 SPEC

> 目标：从 CLI MVP 升级为「个人使用感受上接近 Primo Ramdisk」的产品
>
> 核心策略：用 `aimapi.dll` 直接 API 调用替换 `aim_ll.exe` 子进程，解锁精细控制
>
> 日期: 2026-07-31 | 关联: `PROJECT_STATUS.md`, `2026-07-31-ramdisk-cli-design.md`

---

## 1. 关键发现：aimapi.dll 替代 aim_ll 子进程

### 1.1 当前架构的问题

```
ramdisk-cli → aim_ll.exe 子进程 → stdout 解析 → phdskmnt.sys
              ↑
         每次操作都要 CreateProcess + 解析文本输出
         慢、脆弱、信息受限、无回调机制
```

### 1.2 目标架构

```
ramdisk-cli → aimapi.dll (LoadLibrary) → phdskmnt.sys
              ↑
         直接函数调用、返回值精确、无进程开销
         同 RamDyn.exe / RamDiskUI.exe 的技术路线
```

### 1.3 aimapi.dll 关键 API（已从 DLL 导出表提取）

| 函数 | 用途 |
|------|------|
| `ImScsiCreateDevice` / `ImScsiCreateDeviceEx` | 创建虚拟磁盘（支持 vm/file/proxy 类型） |
| `ImScsiRemoveDevice` | 移除设备 |
| `ImScsiOpenDiskByDeviceNumber` | 打开设备 |
| `ImScsiGetDeviceList` | 枚举设备 |
| `ImScsiFindFreeDriveLetter` | 查找可用盘符 |
| `ImScsiChangeFlags` | 修改设备标志（读写/只读切换等） |
| `ImScsiExtendDevice` | 动态扩展磁盘大小 |
| `ImScsiDeviceIoControl` | 直接发 IOCTL 到驱动 |
| `ImScsiGetVersion` / `ImScsiCheckDriverVersion` | 版本检查 |
| `ImScsiGetDeviceNumberForDisk` | 磁盘号→设备号映射 |

### 1.4 RamDyn.exe 参考

AIM Toolkit 自带的 RamDyn.exe 内存盘工具命令行：
```
RamDyn.exe MountPoint Size|ImageFile CleanRatio BlockSize FormatParam Label SectorSize
```

**关键特征**:
- `CleanRatio` — 动态内存清理比例（等同于 Primo 的 DMM），-1 表示用 TRIM 指令
- `MemoryType` — 内存分配方式
- `BlockSize` — 内存块大小（12=4KB 到 30=1GB）
- `ImageFile` — 可直接从镜像文件加载（等同于我们的 VHD 挂载）

**RamDyn 不使用 proxy 模式**。它用 `ImScsiCreateDevice` + vm 类型 + TRIM 清理实现动态内存。

---

## 2. Phase 1: 架构升级（aimapi.dll 直调）

### 2.1 新增模块：`AimApi`（替换 `AimBridge`）

```
AimApi (新)
├── aimapi.dll 动态加载
├── 封装所有关键 API 为 C++ 方法
├── 无子进程、无 stdout 解析
└── 直接获取设备信息、盘符、大小等
```

**接口**:
```cpp
class AimApi {
    bool Load();                              // LoadLibrary("aimapi.dll")
    bool CreateVmDisk(uint64_t size, const std::string& imageFile,
                      std::string& outMountPoint, int& outDeviceNumber);
    bool RemoveDevice(int deviceNumber);
    std::vector<DeviceInfo> ListDevices();
    bool ExtendDevice(int deviceNumber, uint64_t newSize);
    std::string FindFreeDriveLetter();
    bool FlushDevice(int deviceNumber);        // 强制写回
    bool ChangeFlags(int deviceNumber, bool writable);
    uint64_t ReadDeviceData(int deviceNumber, void* buffer, uint64_t size);
    static std::string GetApiVersion();
};
```

### 2.2 改动范围

| 文件 | 改动 |
|------|------|
| `src/aim_api.h/.cpp` | **新建** — 替换 aim_bridge |
| `src/aim_bridge.h/.cpp` | 标记 deprecated，保留兼容 |
| `src/main.cpp` | `AimBridge` → `AimApi` 切换 |
| `src/CMakeLists.txt` | 添加 aim_api.cpp |

### 2.3 收益

- 不再依赖 `aim_ll.exe` 路径查找和环境变量
- 挂载/卸载操作从 ~500ms（进程创建）降到 ~1ms（函数调用）
- 可获得更丰富的设备信息（SCSI 地址、驱动版本等）
- `ImScsiCreateDevice` 支持比 aim_ll CLI 更多的参数

---

## 3. Phase 2: 接近 Primo 的用户体验

### 3.1 系统托盘 GUI (`ramdisk-tray.exe`)

对标 Primo Ramdisk 的系统托盘体验。

```
托盘图标 ─→ 左键双击: 打开 RAM 盘
          └─ 右键菜单:
               ├── 打开 RAM 盘 (R:)
               ├── 状态: 2.00 GB, 已用 45%
               ├── ──────────────
               ├── 手动保存
               ├── 立即清理 (TRIM)
               ├── ──────────────
               ├── 管理磁盘... (打开配置窗口)
               ├── 日志...
               └── 退出
```

**技术方案**: 
- 纯 Win32 API 实现托盘图标（`Shell_NotifyIcon`），无第三方 GUI 库依赖
- `CreateWindowEx` 创建隐藏消息窗口接收托盘消息
- 通过命名 pipe 或共享内存与 daemon 进程通信
- `ramdisk-tray.exe` 作为独立轻量进程

**新增文件**: `src/tray_icon.h/.cpp`

### 3.2 自动格式化和挂载

对标 Primo：创建 RAM 盘后自动格式化并分配盘符，一步到位。

**当前**: 用户需手动 diskpart 初始化 + 格式化 + 分配盘符
**目标**: 自动调用 `IVdsService` 或 diskpart 脚本完成初始化→格式化→分配

```
ramdisk-cli create -s 4G -o D:\disk.vhd --auto-format --fs NTFS --label WorkDrive
→ 创建 VHD → 挂载 → GPT 初始化 → NTFS 快速格式化 → 分配盘符 R: → 就绪
```

**技术方案**: 
- 使用 diskpart 脚本作为回退方案
- 优先使用 `IVdsService` COM API 做磁盘管理
- `ImScsiFindFreeDriveLetter()` 自动选择盘符

### 3.3 动态内存管理 (DMM)

对标 Primo 的核心差异化功能。

**原理**: RamDyn.exe 的 CleanRatio 机制：
- 创建 RAM 盘时不预分配全部内存
- 使用稀疏内存分配
- 后台线程定期扫描文件系统中已删除的区域
- 通过 TRIM/UNMAP 通知驱动释放对应内存块

**我们的实现**:
```cpp
// 利用 aimapi.dll 的能力
ImScsiCreateDeviceEx(/* sparse allocation, TRIM support */);
// 后台清理线程
void TrimCleanupThread() {
    while (running) {
        Sleep(cleanInterval);
        // 扫描 NTFS $Bitmap 找到空闲簇
        // 发送 TRIM 命令到驱动释放空闲块
        ImScsiDeviceIoControl(device, IOCTL_TRIM, ...);
    }
}
```

**CLI 接口**:
```
--dmm             启用动态内存管理（默认启用）
--clean-interval  清理间隔秒数（默认 60）
--clean-ratio     清理比例（默认 10/1000，即每轮清理 1% 的磁盘空间）
```

### 3.4 Daemon 守护进程完善

修复已知问题，实现可靠的后台服务。

- [ ] **日志写入修复**: 每次 `WriteLog` 后强制 `fflush` + 文件锁
- [ ] **进程唯一性**: `CreateMutex` 确保只有一个 daemon 实例
- [ ] **开机自启**: 注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- [ ] **崩溃恢复**: Watchdog 集成，子进程崩溃自动重启
- [ ] **优雅退出**: `unmount` 命令通过命名 Event 通知 daemon → 保存 → 卸载 → 退出

### 3.5 多盘管理

```bash
ramdisk-cli create -s 2G -o D:\work.vhd --label WorkDrive    # 设备 0
ramdisk-cli create -s 4G -o D:\temp.vhd --label TempDrive    # 设备 1
ramdisk-cli status
# [0] D:\work.vhd -> R: (2.00 GB) | save: 300s | state: mounted
# [1] D:\temp.vhd -> S: (4.00 GB) | save: 600s | state: mounted
ramdisk-cli unmount --device 1
```

所有命令中 `--device` 参数支持指定设备号。

### 3.6 VHD 大端序修复

使 VHD 文件兼容 DiscUtils / aim_cli 等外部工具，以及 Windows 原生 VHD 挂载。

- 所有多字节 VHD 字段（footer、dynhdr、BAT）读写时做 big-endian 转换
- 使用已验证的逐字节 swap（`Swap32`/`Swap64`）
- 修复 Close() 中 end footer 的写入路径
- 修复 Open() 中 dynhdr 字段的反转换
- 修复 BAT 读写的字节序

### 3.7 固定 VHD 支持

固定 VHD 读写性能优于动态 VHD（无 BAT 查表开销），适合高频读写场景。

```cpp
bool Create(const std::string& path, uint64_t sizeBytes, bool dynamic = true);
// dynamic=false → 创建固定 VHD (type=2)
```

固定 VHD = 原始磁盘数据 + 512 字节 Footer。创建时需全量写入（比动态慢），但后续读写性能更好。

### 3.8 快速增量保存

利用 DirtyBitmap 实现增量写回，但不需要 proxy 模式。

**方案**: 在 vm 模式下，通过比较保存前后的 PhysicalDrive 快照来检测脏块：
- 每次 `save` 时：读取 PhysicalDrive 全部扇区 → 与上次保存的快照比较 → 只写回变更扇区
- 内存开销：保存一个扇区快照（与 RAM 盘同等大小）
- 可选启用：`--quick-save` 标志

---

## 4. 实施优先级

| 优先级 | 功能 | 影响 | 复杂度 |
|--------|------|------|--------|
| **P0** | aimapi.dll 直调替换 aim_ll | 架构基础，所有后续功能依赖 | 中 |
| **P1** | VHD 大端序修复 | 兼容性，外部工具可读 | 中 |
| **P1** | Daemon 守护完善 | 后台运行可靠性 | 低 |
| **P2** | 自动格式化 + 盘符 | 一步到位体验 | 低 |
| **P2** | 多盘管理 | CLI 扩展 | 低 |
| **P3** | 系统托盘 GUI | 接近 Primo 体验 | 中 |
| **P3** | 固定 VHD 支持 | 性能优化 | 低 |
| **P4** | 动态内存管理 (DMM) | 核心差异化 | 高 |
| **P4** | 快速增量保存 | 性能优化 | 高 |

---

## 5. 与 AIM proxy 的关系

### 当前状态

AIM Free Mode 内核驱动不接受第三方 pipe 连接。IMDPROXY 协议已完整提取，StorageServer 已实现但并不需要——**我们用 aimapi.dll 直接调用的 vm 模式就足够了**。

### proxy 模式的未来

如果将来 AIM Professional Mode 可用，StorageServer 可直接接入 pipe/TCP 获得：
- 真正的延迟加载（内核级按需读取，不用预加载整个 .raw 文件）
- 零拷贝增量保存（内核级脏块追踪，无需用户态快照对比）

在此之前，vm 模式 + DirtyBitmap 快照对比可以实现等效的增量保存。

---

## 6. 待确认项

- [ ] `ImScsiCreateDeviceEx` 的完整参数签名（需从 AIM 源码 aimapi.h 确认）
- [ ] TRIM/UNMAP 在 Free Mode 下的支持情况
- [ ] `IVdsService` vs diskpart 方案的最终选择
- [ ] 系统托盘是否需要独立进程，或集成到 daemon
