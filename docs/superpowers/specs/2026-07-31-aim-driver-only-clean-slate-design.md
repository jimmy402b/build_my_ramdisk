# MyRamDisk vNext：AIM Driver-only Clean-slate 设计规格

> 日期：2026-07-31
>
> 状态：已批准
>
> 目标平台：Windows 10/11 x64
>
> 项目许可：AGPL-3.0-only
>
> 官方 AIM 源码基线：`d379caa027d21902e45fce8c9daf02fd116c2dda`

## 1. 决策摘要

本项目从空工作树重新实现，不复制旧项目源码。旧 Git 历史仅作为误删恢复手段，新分支不继承旧项目文件、构建系统或运行时架构。

vNext 采用以下技术路线：

```text
Windows 文件系统与磁盘栈
          │
          ▼
官方签名 phdskmnt.sys
          │ Shared Memory Proxy
          ▼
MyRamDisk Windows Service
          │
          ├── DirectAIM：直接 IOCTL 控制驱动
          ├── ProxyTransport：实现 IMDPROXY 共享内存协议
          ├── DynamicBlockStore：按需提交、释放内存页
          ├── LayeredImage：VHD + 已提交日志的统一读取视图
          └── CheckpointEngine：增量写回、崩溃恢复和镜像合并
```

产品运行时不得调用或加载：

- `aim_ll.exe`
- `aimapi.dll`
- `RamDyn.exe`
- AIM Toolkit GUI/CLI
- 旧项目的 raw 中转链路

第一阶段仍依赖 AIM 官方签名内核驱动。项目的准确定位是：

> Toolkit-free、AIM-driver-based 的开源动态 RAM Disk。

在 Driver-only 真实验收通过前，不开始完整产品实现。

## 2. 为什么选择从头实现

旧项目的核心数据路径建立在 `aim_ll + RamDyn + raw staging` 上，而新目标要求我们的程序成为实际块设备后端。继续重构将需要同时替换设备创建、I/O、内存管理、镜像加载、保存和故障恢复，保留旧主链路的收益很小，反而容易继承错误假设。

从头实现仍允许复用知识，但不复用旧代码：

- 允许参考 AIM 官方头文件、协议和开源实现。
- 允许使用以前得到的性能数据作为对比基线。
- 允许重新生成测试镜像和测试负载。
- 不复制旧项目的类、函数、CLI 结构或构建文件。
- 每项能力必须由新测试重新证明，不继承旧验收结论。

## 3. 方案比较与最终选择

### 3.1 方案 A：完整 AIM Toolkit + 自研上层

优点是接入快，缺点是继续依赖 `aim_ll`、AIM API 和 Toolkit 安装状态，无法形成独立产品。拒绝该方案。

### 3.2 方案 B：官方驱动 + 自研用户态后端

直接向 `phdskmnt.sys` 发送控制请求，由我们实现 Shared Memory Proxy、动态块存储和持久化。这条路线保留成熟签名驱动，且让我们完全控制数据策略。选择该方案。

### 3.3 方案 C：立即自研内核驱动

独立性最高，但需要承担 Storport、PnP、电源管理、取消请求、内核并发、驱动签名和蓝屏风险。该方案不属于 vNext 第一代范围。上层接口必须允许未来替换内核适配层。

## 4. 语言、工具链和工程约束

### 4.1 主语言

核心服务、驱动适配器、Proxy 和存储引擎统一使用 C++20。

选择理由：

- 与 AIM 官方 C/C++ 结构和 Windows Driver IOCTL ABI 最直接。
- 可生成无额外托管运行时依赖的原生 x64 程序。
- 可以精确控制共享内存、页提交、页释放和文件刷新。
- 避免第一版同时维护 C++/Rust/C# 多语言边界。

未来 GUI 可以独立选择其他技术，但不得进入核心 I/O 进程。

### 4.2 构建基线

- Visual Studio Build Tools + Windows SDK。
- CMake + Ninja。
- 仅支持 x64；ARM64 和 x86 不属于第一版范围。
- `/W4`、`/WX`、`/permissive-`。
- Debug 构建启用断言、RTC 或等价检查。
- CI 必须运行单元测试、协议布局测试和静态分析。
- 所有 Win32 HANDLE 使用 RAII 封装，禁止裸句柄跨越所有权边界。
- 协议边界返回显式 `Result/Error`，不得依赖未捕获异常表达普通错误。

### 4.3 仓库结构

```text
/
├── CMakeLists.txt
├── LICENSE
├── README.md
├── THIRD_PARTY.md
├── cmake/
├── docs/
│   ├── architecture/
│   ├── protocol/
│   └── superpowers/specs/
├── src/
│   ├── common/
│   ├── driver/
│   ├── proxy/
│   ├── storage/
│   ├── image/
│   ├── journal/
│   ├── service/
│   └── cli/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── e2e/
│   └── fixtures/
└── installer/
```

模块之间通过窄接口通信。`storage` 不得包含 AIM 常量，`driver` 不得解析 VHD，`image` 不得直接操作 Windows 虚拟磁盘设备。

## 5. Driver-only 可行性门槛

### 5.1 验证目标

在当前已安装 `phdskmnt.sys` 的机器上，使用我们自己编译的最小程序完成：

1. 找到 AIM SCSI Adapter。
2. 使用 `IOCTL_SCSI_MINIPORT` 查询驱动版本。
3. 创建 Shared Memory Proxy 对象。
4. 直接发送创建设备请求。
5. 让 Windows 枚举出一块真实 PhysicalDrive。
6. 对该 PhysicalDrive 写入并读回测试扇区。
7. 根据驱动返回的精确设备号删除设备。
8. 确认系统中没有测试设备残留。

### 5.2 禁止依赖

验证程序必须满足：

- 导入表中没有 `aimapi.dll`。
- 运行过程中不创建 `aim_ll.exe`、RamDyn 或 AIM GUI 进程。
- 测试期间停止 `deviosvc`，证明 Shared Memory 路径不依赖该服务。
- 测试必须记录 `deviosvc` 原始启动类型和运行状态，并在结束后恢复原状态。
- 不使用 PowerShell、DiskPart 或 WMI 创建设备；它们只允许用于验收观察。
- 不链接 AIM 用户态二进制文件。

### 5.3 直接驱动控制

`DirectAimAdapter` 负责：

- 通过 SetupAPI/Configuration Manager 找到由 `phdskmnt` 服务驱动的适配器。
- 打开适配器设备对象。
- 构造 `SRB_IO_CONTROL`，签名为 `PhDskMnt`。
- 支持 `SMP_IMSCSI_QUERY_VERSION`。
- 支持 `SMP_IMSCSI_QUERY_ADAPTER`。
- 支持 `SMP_IMSCSI_CREATE_DEVICE`。
- 支持 `SMP_IMSCSI_QUERY_DEVICE`。
- 支持 `SMP_IMSCSI_REMOVE_DEVICE`。
- 校验驱动返回的 NTSTATUS、数据长度和设备号。

所有来自驱动的数据均按不可信输入处理。每个 C/C++ 协议结构必须使用 `static_assert` 验证大小、对齐和关键字段偏移。

### 5.4 最小 Shared Memory Provider

PoC 创建以下全局内核对象：

```text
Global\<session-id>
Global\<session-id>_Request
Global\<session-id>_Response
Global\<session-id>_Server
```

驱动创建参数中的对象路径使用：

```text
\BaseNamedObjects\Global\<session-id>
```

共享内存大小固定为 `8 MiB + 4096 bytes`。前 4096 字节是协议头，其余部分是数据区。事件使用 AutoReset 语义。

PoC 仅宣告和实现：

- `IMDPROXY_REQ_INFO`
- `IMDPROXY_REQ_READ`
- `IMDPROXY_REQ_WRITE`
- `IMDPROXY_REQ_CLOSE`

INFO 响应不宣告 UNMAP、ZERO、SCSI 或 SHARED 能力，避免驱动发送尚未实现的请求。后续里程碑再逐项增加能力。

后端是一块 64 MiB 的零初始化测试内存。逻辑扇区大小为 512 字节。所有读写必须进行边界、对齐和长度检查。

### 5.5 验收数据

PoC 必须生成机器可读报告，至少包含：

- 驱动文件版本和签名状态。
- 适配器标识。
- 创建请求使用的 flags。
- AIM 设备号。
- PhysicalDrive 编号。
- 首次 INFO、READ、WRITE 和 CLOSE 次数。
- 写入偏移、长度和测试数据 SHA-256。
- 读回 SHA-256。
- 删除结果。
- 删除后设备列表。
- 当前进程加载模块列表。
- 相关 AIM 用户态进程列表。

### 5.6 Go/No-go 条件

只有以下条件全部满足才判定运行时 Driver-only 可行：

- 不调用 AIM 用户态组件即可创建 Proxy 设备。
- Windows 能对设备发出真实 READ/WRITE。
- 写入数据可以精确读回。
- 可以只删除本次创建的设备。
- 停止 `deviosvc` 不影响测试。
- Provider 正常关闭后没有设备、共享内存或事件残留。
- 重复运行 20 次没有随机失败或资源泄漏。

如该门槛失败，必须先提交失败证据和根因分析，不能直接进入完整产品开发。

## 6. 独立驱动安装门槛

运行时验证通过后，在一次性 Windows VM 中验证独立安装。

### 6.1 安装原则

- 不安装 AIM Toolkit GUI/CLI。
- 使用官方未经修改的签名驱动包。
- 不单独复制一个 `.sys` 文件。
- 不修改 INF 或 CAT。
- 安装前校验 SHA-256 和 Authenticode。
- 校验签名链和预期发布者。
- 安装器记录驱动包版本、来源和哈希。

### 6.2 安装场景

必须覆盖：

1. 全新 Windows VM，无 AIM 组件。
2. 安装官方驱动包后重启。
3. 运行 Driver-only PoC。
4. 卸载我们的程序但保留驱动。
5. 显式卸载驱动并确认 ROOT 设备和服务清理完成。
6. 离线安装包路径。
7. 从固定官方来源下载、哈希验证后安装的路径。

产品卸载默认不删除驱动。只有显式选择“移除驱动”，并确认没有其他 AIM 设备正在使用时，才执行驱动卸载。

## 7. 正式运行时架构

### 7.1 `DirectAimAdapter`

这是整个系统唯一了解 AIM 创建结构、IOCTL 和设备号的模块。

对上层暴露：

```cpp
class IKernelDiskAdapter {
public:
    virtual Result<DriverInfo> query_driver() = 0;
    virtual Result<CreatedDisk> create_proxy_disk(const ProxyDiskSpec&) = 0;
    virtual Result<DiskInfo> query_disk(DeviceId) = 0;
    virtual Result<void> remove_disk(DeviceId) = 0;
    virtual Result<std::vector<DiskInfo>> list_owned_disks() = 0;
};
```

上层只能持有强类型 `DeviceId`，禁止用磁盘容量、盘符或“最新出现的磁盘”猜测设备身份。

### 7.2 `ShmProxyTransport`

职责仅限：

- 创建和销毁共享内存及同步对象。
- 等待驱动请求。
- 解码协议头。
- 校验请求。
- 调用 `IBlockDevice`。
- 编码响应并唤醒驱动。

它不得实现缓存、VHD 或保存策略。

正式版本逐步支持：

- INFO
- READ
- WRITE
- CLOSE
- UNMAP
- ZERO

第一代不宣告 SCSI passthrough 和 shared reservation。

### 7.3 `IBlockDevice`

```cpp
class IBlockDevice {
public:
    virtual uint64_t size_bytes() const noexcept = 0;
    virtual uint32_t logical_sector_size() const noexcept = 0;
    virtual Result<size_t> read(uint64_t offset, std::span<std::byte>) = 0;
    virtual Result<size_t> write(uint64_t offset, std::span<const std::byte>) = 0;
    virtual Result<void> unmap(std::span<const BlockRange>) = 0;
    virtual Result<void> zero(std::span<const BlockRange>) = 0;
    virtual Result<CheckpointId> checkpoint() = 0;
};
```

Proxy 不允许接触具体镜像类型，便于使用内存后端、测试后端和 VHD 后端做一致性测试。

## 8. 动态内存模型

### 8.1 基本单位

- 对外逻辑扇区：512 字节，后续可支持 4096 字节。
- Provider 分配单位：64 KiB。
- v1 最大逻辑容量：256 GiB。
- x64 进程为整块磁盘预留虚拟地址空间，但不提交物理页。
- 首次读取或写入某个 64 KiB 块时才提交对应页面。
- 释放 Clean 或 TRIM 块时使用 `MEM_DECOMMIT`，保留地址空间映射稳定性。

预留虚拟地址空间不等于占用物理内存。物理内存消耗由已提交块决定。

### 8.2 块状态

每个块具有以下逻辑状态：

| 状态 | 是否占内存 | 读取来源 | 可直接回收 |
|---|---:|---|---:|
| `BackingOnly` | 否 | 已提交日志或基础 VHD | 是 |
| `ResidentClean` | 是 | 内存 | 是 |
| `ResidentDirty` | 是 | 内存 | 否 |
| `DiscardedZero` | 否 | 返回零 | 是 |
| `Saving` | 是 | 内存中的一致副本 | 保存完成后决定 |

状态元数据使用位图和紧凑数组，不为每个块分配堆对象。

### 8.3 读取

1. 校验范围和扇区对齐。
2. 将请求切分到 64 KiB 块。
3. `Resident*` 从内存读取。
4. `DiscardedZero` 返回零。
5. `BackingOnly` 先查询已提交日志索引，再查询基础 VHD。
6. 提交内存页并加载完整块。
7. 标记为 `ResidentClean`。
8. 返回请求范围。

启动和挂载不得扫描完整 VHD 数据区。只允许读取 VHD footer、header、BAT 等必要元数据，以及恢复日志索引。

### 8.4 写入

- 完整覆盖 64 KiB 块时，不读取旧数据，直接提交并写入。
- 部分覆盖时，先解析当前逻辑块内容，再修改指定范围。
- 每次写入递增块版本并标记 `ResidentDirty`。
- 写入响应只能在数据已经进入 Provider 内存并更新状态后返回。

### 8.5 TRIM 和 ZERO

完整块 UNMAP：

- 释放已提交页。
- 设置 `DiscardedZero`。
- 在当前保存代记录 discard。
- 后续读取必须返回零，不能从旧 VHD 恢复已经删除的数据。

部分块 UNMAP/ZERO：

- 加载当前块。
- 清零对应范围。
- 标记 Dirty。

Provider 只有在实现和通过相应测试后，才可在 INFO 中宣告 UNMAP/ZERO 能力。

### 8.6 内存压力

- Clean 块按照近似 LRU 回收。
- Dirty 块在 checkpoint 成功前不得回收。
- 达到软限制时回收 Clean 块。
- 达到高水位时触发 checkpoint。
- 达到硬限制且无法保存时，新写入返回明确的资源不足错误。
- 禁止因内存不足返回伪造的零数据。

默认内存上限为“逻辑磁盘容量”和“系统物理内存 50%”中的较小值，最低为 256 MiB。用户可以显式配置为物理内存的 10% 到 80%。软限制为上限的 80%，checkpoint 高水位为 90%，硬限制为 100%。系统低内存通知可以在尚未达到软限制时提前触发 Clean 回收。

## 9. 镜像和持久化模型

### 9.1 基础镜像

第一代产品格式为标准动态 VHD。会话期间基础 VHD 视为不可变，避免定时保存直接破坏当前唯一有效镜像。

VHDX、ISO、物理磁盘镜像、压缩和加密不属于第一代范围。

### 9.2 增量提交日志

频繁保存使用同目录 sidecar 和双副本清单：

```text
disk.vhd
disk.vhd.rdj
disk.vhd.rdm
disk.vhd.rdm.previous
```

`.rdj` 是本项目定义的追加写日志，包含：

- 文件头和格式版本。
- 基础 VHD 身份和大小。
- 单调递增 checkpoint id。
- DATA 记录。
- ZERO/DISCARD 记录。
- 每条记录的长度、块号和校验和。
- checkpoint commit 记录。
- checkpoint 索引记录，保存“逻辑块 → 最新记录偏移”的快照。
- 固定大小尾部定位器，指向最后一个已提交索引。
- 文件级尾部校验信息。

`.rdm` 是带格式版本、image UUID、基础镜像 generation、基础 checkpoint、活动 journal generation 和校验和的清单。更新清单时先写临时文件并 Flush，再使用原子重命名发布；上一份有效清单保存在 `.rdm.previous`。

只有完整写入、Flush 并存在有效 commit 记录的 checkpoint 才可见。正常启动从尾部定位器直接加载最后一个已提交索引，不扫描整个日志。尾部损坏时向前查找最近的有效 commit/index；恢复过程忽略最后一个有效 commit 之后的不完整尾部。

### 9.3 读取分层

`LayeredImage` 的读取优先级：

```text
当前 RAM 块
   ↓ 未驻留
最后一个已提交日志记录
   ↓ 未覆盖
基础 VHD
   ↓ 未分配
零
```

启动时从最后一个已提交的索引恢复块映射，不加载日志数据内容。读取命中某个日志块时才从 `.rdj` 读取数据。

### 9.4 Checkpoint

保存开始时交换 Dirty 代：

- 保存线程处理冻结代。
- 新写入进入下一代。
- 每个块带版本计数。
- 保存线程锁定块，复制一致内容，记录版本后立即释放锁。
- 保存完成时，只有版本未再次变化的块可以从 Dirty 转为 Clean。

Checkpoint 步骤：

1. 冻结当前 Dirty/Discard 集合。
2. 逐块追加 DATA 或 DISCARD 记录。
3. 写入 checkpoint commit。
4. Flush 文件数据和元数据。
5. 验证刚提交记录。
6. 发布新的 committed checkpoint id。
7. 将未再次修改的块变为 Clean。

应用 I/O 不因完整 checkpoint 长时间冻结。单块复制期间允许短暂等待。

### 9.5 合并和导出

定时 checkpoint 不复制整个 VHD。以下场景执行合并：

- 用户显式执行 `compact`。
- 日志大小达到 `max(4 GiB, 基础 VHD 已分配大小的 50%)`，但最迟不得超过 64 GiB。
- 日志 DATA/ZERO/DISCARD 记录达到 4,000,000 条。
- 用户选择卸载时生成独立 VHD。

合并必须允许应用继续写入。开始合并前先完成一个 checkpoint，并把当前 journal generation 冻结；随后创建新的活动 journal generation，后续 checkpoint 写入新 journal。合并流程：

```text
冻结 journal generation G，创建活动 generation G+1
          ↓
读取 base VHD + frozen journal G
          ↓
生成 disk.next.vhd
          ↓
结构校验 + 数据抽样校验 + Flush
          ↓
disk.vhd → disk.previous.vhd
disk.next.vhd → disk.vhd
          ↓
原子发布新 manifest：新 base 已包含至 checkpoint G
活动 journal G+1 继续作为新 base 的 overlay
          ↓
确认成功后清理 frozen journal G
```

Journal 记录按逻辑 image UUID 和 checkpoint id 关联，不依赖基础 VHD 的物理文件哈希，因此 G+1 可以在基础镜像切换后继续作为 overlay。任何时刻至少保留一个已验证可恢复的基础镜像和一份有效 manifest。合并中断时根据当前有效 manifest 选择旧 base 或新 base，不凭文件名猜测。

## 10. 服务和控制面

### 10.1 Windows Service

`myramdisk-service.exe` 是唯一有权控制驱动和持有 Provider 的进程。

第一代最多同时管理 8 块活动磁盘。超过上限的 mount 请求返回明确的资源限制错误，不进行部分创建。

职责：

- 读取 profile。
- 恢复日志。
- 启动 Provider。
- 创建设备。
- 管理盘符和卷状态。
- 执行定时 checkpoint。
- 响应关机和 preshutdown。
- 精确卸载设备。
- 发布健康状态和统计信息。

默认 checkpoint 间隔为 300 秒；配置为 0 表示仅手动和正常卸载时保存。Dirty 数据达到 1 GiB 或内存达到 checkpoint 高水位时，可以早于定时间隔触发保存。同一磁盘只允许一个 checkpoint 或 compact 操作运行。

### 10.2 CLI

`myramdisk.exe` 只通过本地命名管道调用服务，不直接访问驱动。

第一代命令：

```text
driver status
image create
disk mount
disk status
disk save
disk compact
disk unmount
diagnose
```

CLI 和服务使用版本化消息。未知字段必须可忽略，未知命令必须返回稳定错误码。

### 10.3 会话状态

每块活动磁盘有唯一 SessionId，并持久化：

- profile id。
- VHD 路径。
- journal 路径。
- AIM device id。
- PhysicalDrive path。
- 盘符。
- Provider 对象名。
- 最后提交的 checkpoint id。
- 正常或异常退出标记。

启动时不得盲目复用旧设备号。必须查询当前驱动状态并进行身份核对。

## 11. 安全和权限

- Service 默认运行于 LocalSystem。
- CLI 管道只允许本机管理员和配置中授权的用户访问。
- Shared Memory、Event 和 Mutex 使用显式 DACL。
- Session 对象名包含加密安全随机标识，禁止固定全局名称。
- 所有文件路径使用 Unicode Win32 API。
- 路径必须规范化并防止指向设备命名空间、管道或非预期重解析点。
- 驱动下载必须使用 HTTPS、固定来源、固定哈希和签名验证。
- 日志不得记录用户文件内容或内存数据。
- 诊断报告中的路径支持脱敏选项。

## 12. 故障处理

### 12.1 Provider 崩溃

- 驱动 I/O 进入明确失败状态。
- 不向 Windows 返回伪造成功。
- Watchdog 不尝试让新进程接管旧共享内存会话。
- 服务重启后精确清理旧设备，再从最后已提交 checkpoint 创建新会话。
- 最后一次 checkpoint 后的内存数据允许丢失，但已提交数据不得损坏。

### 12.2 驱动异常

- 驱动不存在、签名无效或版本不兼容时拒绝挂载。
- 创建设备超时后查询适配器，判断是创建失败还是响应丢失。
- 无法证明设备归属时禁止自动删除。

### 12.3 镜像或日志损坏

- 基础 VHD 无法通过结构校验时拒绝读写挂载。
- 日志尾部损坏时截断到最后有效 commit；原文件先备份为诊断副本。
- 已提交记录校验失败时进入恢复模式，不进行自动覆盖。
- `disk.previous.vhd` 只在用户确认或自动策略明确满足时用于回退。

## 13. 可观测性

服务输出结构化日志，关键字段包括：

- session id
- device id
- physical drive
- request type
- offset/length
- resident/dirty/discarded block count
- committed bytes
- checkpoint id 和耗时
- VHD/journal 读取字节数
- 内存 committed/reserved bytes
- Win32、NTSTATUS 和内部错误码

默认日志不记录每个正常 I/O 请求，避免严重影响性能。协议跟踪只能在诊断模式下启用。

## 14. 测试策略

### 14.1 单元测试

- AIM 和 IMDPROXY 结构大小、对齐和字段偏移。
- SRB 请求编码和响应解码。
- 块状态机的全部合法和非法转换。
- 部分块读写。
- Dirty 代切换和版本竞争。
- UNMAP 后旧数据不会复活。
- VHD footer/header/BAT 解析。
- journal 记录编码、校验、索引和恢复。
- 文件路径和整数溢出验证。

### 14.2 集成测试

- Fake transport + DynamicBlockStore。
- 内存后端 + 真实 Proxy 编解码。
- VHD + journal 的分层读取。
- checkpoint 期间并发写入。
- Clean 回收后从 journal 重新加载。
- 故障注入后的恢复。

### 14.3 真实 E2E

- 真实 `phdskmnt.sys` 创建/读写/删除。
- Windows 初始化、分区、格式化和盘符挂载。
- NTFS 文件创建、修改、删除和 TRIM。
- 保存后强制终止服务并重新挂载。
- 保存每个关键阶段的进程强杀。
- Windows 重启和 preshutdown。
- 安装、升级、卸载和驱动残留。

真实 E2E 测试必须串行执行，并在开始和结束时记录 AIM 设备清单。

## 15. 性能和资源验收

在固定参考机器上记录硬件、Windows 版本和驱动版本。第一代最低目标：

- 挂载 64 GiB 动态 VHD 时，Ready 前不得读取超过 64 MiB 镜像数据。
- 挂载路径不得包含与逻辑容量成正比的数据扫描。
- 空闲 64 GiB 磁盘的 Provider committed memory 不超过 128 MiB。
- 实际触碰 1 GiB 数据后，新增 committed memory 不超过 1.10 GiB 加固定元数据开销。
- TRIM 1 GiB 完整块区域后，30 秒内至少释放其中 90% 的 Provider committed memory。
- checkpoint 期间文件系统保持可读写。
- Shared Memory Provider 顺序吞吐达到官方 DevioShm 基线的 70% 以上。
- 4 KiB 随机 IOPS 达到官方 DevioShm 基线的 60% 以上。
- 连续 100 次挂载/卸载无设备、句柄或命名对象泄漏。

性能不以牺牲数据正确性、边界检查或 Flush 语义为代价。

## 16. 开发里程碑

### M0：Driver-only PoC

- Direct IOCTL。
- 共享内存 Provider。
- 64 MiB 测试盘。
- 原始扇区读写。
- 精确删除。
- 20 次重复测试。

通过后才能确认 Driver-only 可行。

### M1：Proxy 协议产品化

- 独立 `driver` 和 `proxy` 库。
- 完整校验、取消、关闭和超时。
- Fake adapter/transport 测试。
- Provider 崩溃行为测试。

### M2：动态块存储

- VirtualAlloc reserve/commit/decommit。
- 64 KiB 块状态。
- Dirty、Discard 和 Clean eviction。
- UNMAP/ZERO。
- 内存压力策略。

### M3：VHD、journal 和 checkpoint

- 动态 VHD 只读层。
- 追加式 `.rdj`。
- 崩溃恢复。
- 分层按需读取。
- checkpoint 和 compact。

### M4：Windows Service 和 CLI

- 版本化 IPC。
- profile/session。
- 定时保存。
- 开机恢复和 preshutdown。
- 多磁盘会话。

### M5：独立安装和 VM 验收

- 驱动包管理。
- 签名和哈希校验。
- 全新 VM 安装。
- 升级、卸载和共存策略。

### M6：性能与发布加固

- 资源压力。
- 长时间运行。
- 故障注入矩阵。
- 基准测试。
- 文档、诊断包和发布签名。

每个里程碑单独编写实施计划和验收报告。不得在前一里程碑未通过时，以“后续会修复”为理由宣布下一阶段完成。

## 17. 第一代明确不做的内容

- 自研 Windows 内核驱动。
- Linux/macOS 支持。
- VHDX。
- 镜像压缩、加密和去重。
- 网络块存储。
- 多主机共享写入。
- SCSI passthrough 和持久预留。
- 完整桌面 GUI。
- 休眠文件、页面文件和系统启动盘场景。

这些能力不会进入第一代接口承诺，避免妨碍核心 I/O 和持久化正确性。

## 18. 许可证和第三方来源

项目采用 AGPL-3.0-only，与 AIM 官方开源许可方向保持兼容。仓库必须包含：

- 项目许可证。
- AIM 来源、版本、提交号和许可证说明。
- 使用或转写的 AIM/ImDisk 协议定义来源。
- 驱动包下载来源、版本、SHA-256 和签名验证规则。
- 其他依赖的版本、许可证和固定哈希。

官方参考：

- <https://github.com/ArsenalRecon/Arsenal-Image-Mounter>
- <https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/master/LICENSE.md>
- <https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/master/Unmanaged%20Source/phdskmnt/inc/common.h>
- <https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/master/Unmanaged%20Source/phdskmnt/inc/imdproxy.h>
- <https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/master/Unmanaged%20Source/phdskmnt/proxy.cpp>
- <https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/master/Managed%20Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs>

## 19. 最终产品验收定义

只有同时满足以下条件，才可以称为“接近 Primo Ramdisk 使用体验的第一代产品”：

1. 不安装 AIM Toolkit 用户态工具也能安装和运行。
2. 可以创建标准动态 VHD。
3. 可以把已有 VHD 立即挂载为 Windows 普通磁盘。
4. 镜像数据只在实际访问时加载到内存。
5. 物理内存随实际访问量增长，而不是随逻辑容量预分配。
6. NTFS 删除和 TRIM 能释放相应内存。
7. 被删除数据不会从旧镜像复活。
8. 应用程序对它的使用方式与普通磁盘一致。
9. 定时 checkpoint 不长时间中断磁盘 I/O。
10. 任意时刻至少存在一个已提交、可校验和可恢复的数据状态。
11. 突然终止服务或断电模拟不会破坏最后一个已提交 checkpoint。
12. 可以精确卸载自己的设备，不影响其他 AIM 设备。
13. 安装、运行和卸载不需要 `aim_ll`、`aimapi.dll`、RamDyn 或 AIM GUI。
14. 通过资源、性能、故障注入和 100 次生命周期测试。

本规格批准后，下一步只为 M0 编写实施计划。完整产品代码必须等待 M0 Driver-only 可行性验收通过。
