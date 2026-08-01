# M1/M2 Proxy 产品化与动态块存储设计规格

日期：2026-08-01

状态：设计已确认，等待用户复核

目标平台：Windows x64、MSVC、C++20

前置里程碑：M0 Driver-only 验证最终 GO

## 1. 文档目的

本规格定义 M1 和 M2 的产品边界、组件接口、状态机、并发模型、错误语义、资源限制和验收标准。

- M1 将 M0 的 Shared Memory Proxy PoC 产品化。
- M2 实现真正按访问量提交物理内存的动态块存储。
- M1/M2 不实现 VHD、journal、checkpoint 或定时写回。
- M1/M2 为 M3 提供稳定的按需读取和版本化 Dirty 快照接口。

本文是一份共同架构规格，但执行时必须拆成独立的 M1 实施计划和 M2 实施计划。M1-C 未通过前，不得进入 M2 的真实驱动工作。

本规格批准的是设计，不代表当前真实驱动链路已经通过。本文编写时，仓库已通过 M0 V3 只读查询和 Task 24 harness，但真实 CREATE 生命周期仍在 Task 25 排查内核命名空间解析问题。M1/M2 的真实驱动实施和验收必须等待 M0 最终 GO。

规范词：

- “必须”：实现和验收不可省略。
- “不得”：违反即为 NO-GO。
- “应”：除非有记录完整的技术理由，否则必须实现。
- “可以”：可选实现，不构成第一版验收条件。

## 2. 已批准的设计决策

1. M1/M2 采用易失性动态 RAM Disk。
2. 未写入的新盘区域读取为全零。
3. M1/M2 只提供抽象 backing 接口；VHD 和 journal 留到 M3。
4. 内存限制采用软目标加硬安全上限。
5. Dirty 数据不得因内存压力被静默丢弃。
6. 动态块固定为 64 KiB。
7. SHM Proxy 每盘保持单请求循环。
8. `DynamicBlockStore` 从第一版开始支持线程安全访问。
9. 逻辑容量使用 `VirtualAlloc(MEM_RESERVE)` 预留连续虚拟地址。
10. 实际访问时才以 64 KiB 为单位 `MEM_COMMIT`。
11. 同时实现并公布 UNMAP 和 ZERO。
12. 完整块 UNMAP/ZERO 必须释放对应 committed memory。
13. 采用分层块引擎，不采用单体 Provider 或完整异步分页系统。

## 3. 范围

### 3.1 M1 范围

M1 负责：

- Shared Memory、Request Event、Response Event 和 Cancel Event 生命周期。
- INFO、READ、WRITE、UNMAP、ZERO、CLOSE 的编解码。
- 请求长度、对齐、边界和整数溢出校验。
- 请求取消、超时、关闭和 fatal protocol error。
- 将协议请求转换为 `IBlockDevice` 调用。
- 将内部错误映射为稳定 Proxy `errorno`。
- Provider 状态、统计和结构化诊断。
- Fake block device 和本地 SHM 集成测试。
- M0 GO 后的真实驱动生命周期测试。

M1 不负责：

- 直接管理块内存。
- 选择缓存或淘汰算法。
- 解析 VHD。
- 保存 Dirty 数据。
- Windows Service、CLI、profile 或开机恢复。

### 3.2 M2 范围

M2 负责：

- 连续虚拟地址预留。
- 64 KiB 块的按需 commit 和 decommit。
- 稀疏元数据目录。
- 块状态机和版本号。
- backing 按需读取。
- Dirty、Clean 和 ZeroOverride 语义。
- Clean eviction。
- 软、硬内存限制。
- 并发访问、取消和超时。
- 版本化 Dirty 数据与零覆盖快照。
- Fake/Zero backing、模型测试和故障注入。
- M0 GO 后的真实 NTFS 动态内存盘验收。

M2 不负责：

- 永久保存 Dirty 数据。
- 临时交换文件。
- VHD、VHDX、journal 或 checkpoint。
- 压缩、去重、加密或网络 backing。

### 3.3 第一版明确不做

- 自研内核驱动。
- 替换 `phdskmnt.sys`。
- 多队列 SHM 协议。
- 无锁块存储。
- 精确 LRU。
- 页面文件、休眠文件或系统启动盘。
- GUI。

## 4. 前置条件与硬门槛

### 4.1 M0 前置条件

真实驱动 M1/M2 工作开始前，M0 必须提供并通过：

- 可重复的 CREATE/QUERY/REMOVE 生命周期。
- 可由驱动 System 上下文解析的 SHM 和 Event 命名规则。
- 精确设备归属和删除规则。
- 原始扇区读写。
- 清理后设备、服务、句柄和命名对象基线恢复。
- 不加载 AIM Toolkit 用户态组件的证据。

如果 M0 最终为 BLOCKED 或 NO-GO：

- 可以完成纯单元和本地 SHM 实现。
- 不得运行真实驱动 M1/M2 gate。
- 不得宣布 M1 或 M2 GO。

### 4.2 支持环境

- 64 位 Windows 和 64 位进程。
- MSVC C++20。
- 逻辑扇区大小只允许 512 或 4096 字节。
- 逻辑容量必须大于零并为扇区大小的整数倍。
- 第一版参考环境必须能创建 64 GiB 动态盘。
- 参考机器必须验证 1 TiB 虚拟地址预留；失败必须原子返回，不得部分创建。

## 5. 总体架构

```text
Windows 文件系统
        |
        v
phdskmnt.sys
        |
        | AIM Shared Memory Proxy
        v
ShmProxySession
        |
        v
RequestProcessor
        |
        | IBlockDevice
        v
DynamicBlockStore
        |
        | IBackingStore
        v
ZeroBackingStore / FakeBackingStore

M3 将增加：CompositeVhdJournalBackingStore + CheckpointEngine
```

依赖必须单向：

```text
proxy -> IBlockDevice <- storage
storage -> IBackingStore
driver lifecycle -> proxy session
```

- `proxy` 不得包含 `VirtualAlloc`、LRU 或块状态知识。
- `storage` 不得包含 AIM request code、Event 或 Shared Memory 知识。
- M3 可以依赖 M2 快照接口，M2 不得依赖 M3。

## 6. 组件定义

### 6.1 `ShmProxySession`

职责：

- 创建并拥有命名对象。
- 建立共享内存布局。
- 启动、取消和 join Provider 线程。
- 执行会话状态机。
- 等待 Request Event。
- 在响应完整写入后触发 Response Event。
- 发布会话统计和最后错误。

不得处理块缓存或设备创建/删除策略。

### 6.2 `RequestProcessor`

职责：

- 从共享内存复制和验证请求头。
- 执行 checked arithmetic。
- 规范化 UNMAP/ZERO ranges。
- 调用 `IBlockDevice`。
- 编码成功或失败响应。
- 决定错误是 request-level 还是 session-fatal。

### 6.3 `DynamicBlockStore`

职责：

- 实现 `IBlockDevice`。
- 管理预留地址、块 commit 和 decommit。
- 管理状态、版本、pin 和统计。
- 调用 `IBackingStore` 加载未驻留块。
- 执行 Clean eviction。
- 提供 Dirty 快照和条件式 `mark_persisted`。

### 6.4 `VirtualMemoryArena`

- 窄封装 `VirtualAlloc`、`VirtualFree` 和 `VirtualQuery`。
- 执行地址、大小和对齐验证。
- 支持测试替身和 commit/decommit 故障注入。
- 维护 committed bytes，并允许测试与 `VirtualQuery` 交叉验证。

### 6.5 `BlockDirectory`

- 提供两级稀疏块元数据目录。
- 惰性创建叶节点。
- 管理固定数量 lock stripes 和 condition variables。
- 提供驻留块索引，不为每个逻辑块预创建 mutex。

### 6.6 `EvictionManager`

- 超过软上限时执行近似 CLOCK/second-chance。
- 只选择未 pin 的 `ResidentClean`。
- 回收到 low watermark。
- 新 commit 接近硬上限时提供同步回收尝试。

### 6.7 `IBackingStore`

M1/M2 中 backing 是只读块来源，不是保存目标。

```cpp
enum class BlockLoadKind {
    Data,
    AllZero
};

struct BlockLoadResult {
    BlockLoadKind kind;
};

class IBackingStore {
public:
    virtual ~IBackingStore() = default;

    virtual Result<BlockLoadResult> load_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) = 0;
};
```

约束：

- `destination` 大小固定为 64 KiB。
- 失败时调用方不得发布 `destination` 内容。
- `AllZero` 允许调用方不 commit RAM。
- `ZeroBackingStore` 对所有块返回 `AllZero`。
- `FakeBackingStore` 支持确定性内容、延迟、取消、超时和指定块失败。
- M3 的 VHD/journal 分层读取实现此接口。
- 一个 Session 看到的 backing 视图必须保持不可变；M3 只有在新的持久化 generation 已原子发布后才能更新读取视图。
- `AllZero` 成功返回后，Store 可以把该块发布为 `ZeroClean`，避免重复读取 backing。

## 7. 公共块设备接口

```cpp
struct DataRange {
    std::uint64_t offset;
    std::uint64_t length;
};

struct DeviceFeatures {
    bool read_only;
    bool supports_unmap;
    bool supports_zero;
};

class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;

    virtual std::uint64_t size_bytes() const noexcept = 0;
    virtual std::uint32_t sector_size() const noexcept = 0;
    virtual DeviceFeatures features() const noexcept = 0;

    virtual Result<void> read(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<std::byte> destination) = 0;

    virtual Result<void> write(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<const std::byte> source) = 0;

    virtual Result<void> unmap(
        const OperationContext& context,
        std::span<const DataRange> ranges) = 0;

    virtual Result<void> zero(
        const OperationContext& context,
        std::span<const DataRange> ranges) = 0;
};
```

通用约束：

- 零长度 READ/WRITE 无效。
- offset 和 length 必须按逻辑扇区对齐。
- `offset + length` 使用 checked arithmetic。
- 范围不得超过逻辑容量。
- 跨块请求拆分为 64 KiB 块片段。
- 同一块内读写在块锁保护下不可观察到中间 memcpy 状态。
- 不承诺跨多个块的设备级原子性。

## 8. Proxy 协议

### 8.1 支持请求

| 请求 | Code | 第一版 |
|---|---:|---|
| INFO | 1 | 必须 |
| READ | 2 | 必须 |
| WRITE | 3 | 必须 |
| CLOSE | 5 | 必须 |
| UNMAP | 6 | 必须 |
| ZERO | 7 | 必须 |

第一版不公布 CONNECT、SCSI 和 SHARED 扩展能力。

### 8.2 INFO

```text
file_size     = 精确逻辑容量
req_alignment = 512 或 4096
flags         = SUPPORTS_UNMAP | SUPPORTS_ZERO
```

只读设备另外设置 `IMDPROXY_FLAG_RO`。

### 8.3 READ/WRITE 验证顺序

1. 请求头完整。
2. request code 正确。
3. length 大于零。
4. length 不超过 SHM data area。
5. offset 和 length 按扇区对齐。
6. 加法无溢出。
7. 范围不超过设备容量。

READ 成功必须返回完整长度；失败返回非零 `errorno` 和长度 0。

WRITE：

- 完整块覆盖不得预读 backing。
- 部分覆盖 `Unresident` 必须加载旧块。
- 部分覆盖零状态必须先初始化整块为零。
- 成功修改后递增 version 并成为 Dirty。
- 中途失败可以留下已完成前缀，但不得伪造完整成功。

### 8.4 UNMAP/ZERO

数据区是 `DEVICE_DATA_SET_RANGE[]`。处理前必须：

- 验证数组字节长度。
- 对每个 range 执行 checked arithmetic。
- 验证扇区对齐和容量边界。
- 忽略 length 为零的 range。
- 按 offset 排序并合并重叠或相邻 range。

M1/M2 中两者的可见结果相同：后续读取必须返回零。

- 完整覆盖 64 KiB 块时 decommit RAM。
- 部分覆盖时保留块并清零目标字节。
- 内部快照保留操作类型，供 M3 选择 ZERO 或 DISCARD 记录。

### 8.5 CLOSE 与顺序

- CLOSE 使会话进入 `Closing` 并停止接收新请求。
- CLOSE 不负责猜测和删除驱动设备。
- 每盘只允许一个活动 SHM 请求。
- Provider 必须先完整写入响应，再触发 Response Event。
- 不得轮询未同步的共享内存字段作为完成信号。

## 9. 块大小与地址模型

```text
BlockSize = 64 KiB
```

创建时：

1. 验证容量和扇区大小。
2. checked round-up 到 64 KiB。
3. `VirtualAlloc(nullptr, reserve_size, MEM_RESERVE, PAGE_NOACCESS)`。
4. 预留失败则整个创建失败。

块地址：

```text
block_address = base + block_index * 64 KiB
```

- Commit：`MEM_COMMIT/PAGE_READWRITE`。
- Decommit：`MEM_DECOMMIT`。
- 析构：整个区域 `MEM_RELEASE`。
- `pin_count != 0` 时不得 decommit。
- Commit 成功前不得发布 Resident 状态。

## 10. 稀疏元数据

- 每个叶节点管理 1024 个块，即 64 MiB 逻辑区域。
- 顶层只保存叶节点指针。
- 叶节点首次访问或状态偏离默认值时创建。
- 空盘不得因逻辑容量线性提交 metadata。
- lock stripes 和 condition variables 数量固定。

Store 默认块来源：

- `BackingUnresident`：默认从 backing 读取。
- `CleanZero`：默认读取为零，不需要 backing 或 RAM。

`ZeroBackingStore` 使用 `CleanZero`；未来 M3 已有镜像使用 `BackingUnresident`。

每个已实例化块至少记录：

```text
state
version
pin_count
last_access_epoch
dirty_epoch
reference_bit
last_zero_operation_kind
```

`version` 为 64 位。发生回绕时 Store 进入不可恢复内部错误。

## 11. 块状态机

| 状态 | RAM | 数据来源 | Dirty | 可淘汰 |
|---|---:|---|---:|---:|
| `Unresident` | 否 | backing | 否 | 不适用 |
| `ResidentClean` | 是 | RAM，与 backing 一致 | 否 | 是 |
| `ResidentDirty` | 是 | RAM | 是 | 否 |
| `ZeroClean` | 否 | 已提交零覆盖或新盘默认零 | 否 | 不适用 |
| `ZeroDirty` | 否 | 尚未提交的零覆盖 | 是 | 不适用 |

内部允许短暂使用 `Loading`、`Committing` 和 `Evicting`，但必须有 owner、取消路径和唤醒行为。

主要转换：

```text
Unresident --读取并缓存--> ResidentClean
Unresident --完整写入--> ResidentDirty
Unresident --部分写入--> load -> ResidentDirty
Unresident --完整 UNMAP/ZERO--> ZeroDirty

ResidentClean --写入--> ResidentDirty
ResidentClean --eviction--> Unresident
ResidentClean --完整 UNMAP/ZERO--> ZeroDirty

ResidentDirty --同版本保存成功--> ResidentClean
ResidentDirty --完整 UNMAP/ZERO--> ZeroDirty

ZeroClean/ZeroDirty --读取--> 直接返回零
ZeroClean/ZeroDirty --完整写入--> ResidentDirty
ZeroClean/ZeroDirty --部分写入--> commit zero block -> ResidentDirty
ZeroDirty --同版本零记录保存成功--> ZeroClean
```

`ZeroClean` 和 `ZeroDirty` 都覆盖 backing。任何读取路径都不得让旧数据复活。

## 12. READ 数据流

每个块片段先检查取消和 deadline，再获取 stripe lock。

Resident：

- 增加 pin。
- 更新 access epoch/reference bit。
- 复制到目标。
- 减少 pin。

Zero：

- 直接写零。
- 不 commit。
- 不调用 backing。

Unresident 正常缓存路径：

1. 标记 `Loading`。
2. 释放锁。
3. backing 读取到临时 64 KiB scratch buffer。
4. 重新加锁并验证 owner、取消和 deadline。
5. `AllZero` 时不 commit，直接返回零。
6. `Data` 时 commit、复制并发布 `ResidentClean`。
7. 唤醒等待者。

硬上限且无法回收时，READ 可以使用 uncached read-through：读取到固定 64 KiB scratch buffer，服务本次请求但不 commit、不改变稳定状态。

## 13. WRITE 数据流

完整块覆盖：

- 不调用 backing。
- 如未 committed，先做预算检查和 commit。
- 锁内复制完整块。
- 递增 version/dirty epoch。
- 发布 `ResidentDirty`。

部分覆盖：

- `Unresident` 先加载旧内容。
- Zero 状态 commit 后先清零整块。
- Resident 状态锁内修改。
- 修改后递增 version，成为 `ResidentDirty`。

预计超过 hard limit 时，先同步 Clean eviction；仍无法满足则返回 `MemoryHardLimit`，不得修改块状态。覆盖已有 Resident 块且不新增 commit 时可以继续。

## 14. UNMAP/ZERO 数据流

完整块：

- 等待瞬态操作完成。
- 获取块锁并等待 pin 为零。
- 如 committed，执行 decommit。
- 递增 version。
- 设置 `ZeroDirty` 并记录 UNMAP 或 ZERO 来源。

如果 decommit 失败，必须保留原状态、原 version 和原数据并返回 `DecommitFailed`，不得发布已经释放或已经清零的假状态。

部分块：

- 当前已为 Zero 时是 no-op。
- `Unresident` 先加载 backing，以保留未覆盖字节。
- Resident 状态锁内清零目标范围。
- 设置 `ResidentDirty` 并递增 version。

大范围操作最多保留起始和结尾两个部分块；中间完整块逐块 decommit，不得预先 commit。取消后已完成前缀可以保持生效，响应必须失败。

## 15. 并发模型

- SHM 每盘单请求。
- `DynamicBlockStore` 允许多线程直接调用。
- 块状态由固定 stripe lock 保护。
- 跨块操作按 block index 递增处理。
- 不在 backing I/O 期间持有全局 metadata lock。
- 不在等待 Event/deadline 时持有块锁。
- 多块请求默认一次只持有一个块锁。

同块首次加载只有一个 Loading owner；其他线程在 stripe condition variable 上等待并用 predicate 重查。成功、失败、取消和超时都必须唤醒等待者。

以下操作必须 pin：

- 从 Resident 块 READ。
- 向 Resident 块 WRITE。
- 复制 Dirty snapshot。
- 对 committed 块执行部分 UNMAP/ZERO。

Eviction 和完整块 decommit 只允许在 pin 为零时执行。

## 16. 内存预算与回收

```cpp
struct DynamicBlockStoreOptions {
    std::uint64_t soft_limit_bytes;
    std::uint64_t hard_limit_bytes;
    std::uint64_t eviction_low_watermark_bytes;
    std::uint64_t max_snapshot_batch_bytes;
    std::size_t max_snapshot_batch_records;
    std::size_t lock_stripe_count;
};
```

配置约束：

```text
0 < low_watermark <= soft_limit < hard_limit
snapshot batch >= 64 KiB
snapshot batch 是 64 KiB 的整数倍
snapshot record 上限大于零
lock stripe count 是 2 的幂且至少为 64
```

M1/M2 库要求调用方显式传入配置。M4 再定义产品默认值。测试工具可以使用物理 RAM 的 25%/50% 作为软、硬限制。

超过 soft limit：

- 唤醒后台 eviction worker。
- 使用近似 CLOCK/second-chance。
- 第一次扫描清除 reference bit。
- 再次扫描未被访问、未 pin 的 `ResidentClean`。
- 回收到 low watermark 后停止。

不得淘汰 `ResidentDirty`、瞬态块或 pin 非零块。Zero 状态不占 committed RAM。

hard limit 是安全边界：

- Dirty 可以超过 soft limit。
- 新 commit 不得让 committed bytes 超过 hard limit。
- 无法回收时，新增写入返回资源不足。
- 不得静默丢弃 Dirty。
- 不得创建临时交换文件。

`VirtualAlloc` 失败与策略拒绝分开记录：

- 策略拒绝：`MemoryHardLimit`。
- Win32 commit 失败：`CommitFailed`，保留 `GetLastError()`。

## 17. Dirty 快照接口

M2 不保存文件，但必须让 M3 稳定读取 Dirty 版本，包括无 RAM 的 ZeroOverride。

```cpp
enum class DirtyRecordKind {
    Data,
    Zero,
    Discard
};

struct DirtyBlockSnapshot {
    std::uint64_t block_index;
    std::uint64_t version;
    DirtyRecordKind kind;
    std::vector<std::byte> data;
};

struct DirtyScanCursor {
    std::uint64_t next_block_index;
};

Result<std::vector<DirtyBlockSnapshot>> copy_dirty_batch(
    const OperationContext& context,
    DirtyScanCursor& cursor,
    std::size_t max_bytes);

Result<void> mark_persisted(
    std::uint64_t block_index,
    std::uint64_t expected_version,
    DirtyRecordKind expected_kind);
```

数据快照规则：

- `ResidentDirty` 生成 `Data`，data 必须恰好 64 KiB。
- 在块锁内复制到调用方拥有的稳定缓冲区。
- 复制完成立即释放块锁。
- batch 总 data bytes 不得超过配置上限。

零覆盖快照规则：

- `ZeroDirty` 生成 `Zero` 或 `Discard`。
- 不分配 64 KiB data buffer。
- kind 由产生当前 version 的完整块操作决定。
- Data 和零覆盖记录的总数都受 `max_snapshot_batch_records` 限制，零记录不得绕过 byte limit 形成无界 batch。

`mark_persisted` 只有在当前块仍然 Dirty、version 相同、kind 相同时成功：

- `ResidentDirty -> ResidentClean`
- `ZeroDirty -> ZeroClean`

保存期间发生新写入或 UNMAP/ZERO 时返回 `VersionChanged`，当前块继续 Dirty。

## 18. 会话生命周期

```text
Constructed
  -> ObjectsCreated
  -> Ready
  -> Serving
  -> Closing
  -> Closed

任意阶段不可恢复错误 -> Failed
```

`Failed` 只能清理，不能恢复到 `Serving`。

启动顺序：

1. 生成随机 Session ID。
2. 根据已验证 namespace policy 生成对象名。
3. 创建 Mapping、Request Event、Response Event、Cancel Event。
4. 应用显式 DACL。
5. 映射并清零共享内存。
6. 启动 Provider 线程。
7. Provider 发布 Ready barrier。
8. Ready 后才允许外部 lifecycle 调用 CREATE_DEVICE。

驱动 CREATE 失败时必须取消 Provider 并释放所有对象。

```cpp
class IProxyNamespacePolicy {
public:
    virtual ~IProxyNamespacePolicy() = default;
    virtual Result<ProxyObjectNames> build_names(
        std::wstring_view session_id) const = 0;
};
```

- 本地测试使用进程可见测试 policy。
- 真实驱动使用 M0 证据确认的 kernel-visible policy。
- 不得尝试多个名称直到“碰巧成功”。
- 不得静默回退到 AIM 用户态组件。

真实产品正常关闭顺序：

1. 禁止新的控制操作。
2. 精确删除当前 Session 设备。
3. 等待 CLOSE 或设备消失。
4. 触发 Cancel Event。
5. join Provider。
6. 释放 Mapping/Event/Handle/Store。
7. 验证命名对象无法再次打开。

设备删除编排属于 lifecycle 层；`ShmProxySession` 不自行删除设备。

## 19. 取消与超时

```cpp
struct OperationContext {
    std::stop_token stop_token;
    std::chrono::steady_clock::time_point deadline;
};
```

- 等待 Request Event 必须同时等待 Cancel Event。
- backing load 必须检查 stop token 和 deadline。
- 晚到 backing 结果不得在取消后发布。
- snapshot 每处理一个块都必须检查取消。
- 响应布局已知时，取消返回错误并触发 Response Event。
- 请求头损坏且无法安全编码响应时进入 `Failed`。

## 20. 错误模型

内部错误集合：

```text
ProtocolMalformed
UnknownRequest
InvalidRange
Misaligned
ReadOnly
BackingReadFailed
MemoryHardLimit
CommitFailed
DecommitFailed
Cancelled
TimedOut
SessionClosed
VersionChanged
InternalInvariant
```

Request-level 错误：

- InvalidRange
- Misaligned
- ReadOnly
- BackingReadFailed
- MemoryHardLimit
- CommitFailed
- Cancelled
- TimedOut

Session-fatal 错误：

- 截断且无法确定响应布局。
- 未知 request code。
- Mapping 或 Event 身份失效。
- Response Event 触发失败。
- 非法状态转换。
- metadata、pin 或 committed counter 不变量损坏。
- version 回绕。

固定 Proxy `errorno` 映射：

| 内部错误 | Proxy errorno |
|---|---:|
| InvalidRange / Misaligned | `ERROR_INVALID_PARAMETER` |
| ReadOnly | `ERROR_WRITE_PROTECT` |
| BackingReadFailed | `ERROR_READ_FAULT` |
| MemoryHardLimit | `ERROR_NOT_ENOUGH_MEMORY` |
| CommitFailed / DecommitFailed | 原始 Win32 error；无值时 `ERROR_NOT_ENOUGH_MEMORY` |
| Cancelled | `ERROR_OPERATION_ABORTED` |
| TimedOut | `ERROR_TIMEOUT` |
| SessionClosed | `ERROR_BROKEN_PIPE` |
| InternalInvariant | `ERROR_INTERNAL_ERROR` |

日志必须同时保留错误域、内部 code、Win32 error、请求类型、offset 和 length。

## 21. 安全要求

- Session ID 使用系统加密安全随机源。
- 命名对象不得使用固定公共名称。
- Mapping 和 Event 使用显式 DACL。
- 默认只允许 LocalSystem、服务 SID 和本地管理员。
- 普通用户不得发出伪造 Request Event。
- 不记录块数据内容。
- 诊断报告中的路径支持脱敏。
- 所有外部长度和 offset 经过 checked arithmetic。
- 不使用 “remove all AIM devices” 作为清理方法。
- 只清理能通过 Session identity、DeviceId 和 object path 证明归属的资源。

## 22. 可观测性

会话统计：

```text
session_state
request_count_by_type
request_error_count_by_type
fatal_protocol_error_count
cancel_count
timeout_count
last_error
```

存储统计：

```text
logical_size_bytes
reserved_bytes
committed_bytes
resident_clean_blocks
resident_dirty_blocks
zero_clean_blocks
zero_dirty_blocks
loading_blocks
evicted_blocks_total
backing_reads_total
backing_read_bytes_total
uncached_reads_total
unmap_bytes_total
zero_bytes_total
hard_limit_rejections
commit_failures
decommit_failures
snapshot_records_total
snapshot_version_conflicts
```

当 Store 使用 `CleanZero` 作为默认状态时，`zero_clean_blocks` 包含尚未实例化 metadata 的隐式零块，必须通过总块数减去偏离默认状态的块数计算，不得为了统计创建叶节点。

默认不记录每次正常 I/O。错误日志记录请求类型、offset、length、block index 和错误码；诊断模式可以采样延迟，但不得 dump 数据区。

## 23. 单元测试

### 23.1 M1 协议测试

必须覆盖：

- INFO/READ/WRITE/UNMAP/ZERO/CLOSE 精确结构。
- request code 和 flag 常量。
- 截断和未知请求。
- SHM data area 最大边界。
- offset/length 加法和乘法溢出。
- 扇区对齐和容量边界。
- UNMAP/ZERO range 数组长度和规范化。
- 错误响应精确编码。
- CLOSE、重复 CLOSE、取消和超时。
- 响应写入与 Event 顺序。

M1 fuzz 至少执行 10,000 个随机合法/非法请求；不得越界、崩溃或调用错误 block operation。固定随机种子并记录失败输入。

### 23.2 M2 状态机测试

覆盖：

- Unresident/Resident/Zero 全部合法和非法转换。
- 完整、部分和跨块读写。
- 完整块写不调用 backing。
- Dirty 不淘汰。
- Zero 不从 backing 复活。
- stale snapshot 不清除新 Dirty。
- ZeroDirty 快照不分配 data buffer。
- commit/decommit 失败不产生幽灵状态。

### 23.3 模型测试

使用小型参考 byte array 与 Store 执行相同操作：

```text
read
write
unmap
zero
evict
copy_dirty_batch
mark_persisted
```

- 至少 100,000 个随机操作。
- 每一步抽样比较可见数据。
- 随机种子必须可重现。

### 23.4 并发测试

- 多线程读取同一 Unresident 块，只允许一次 backing load。
- 读/写同块竞争。
- 写/UNMAP 同块竞争。
- eviction/READ 竞争。
- snapshot/WRITE 竞争。
- 取消发生在 load、commit 和 publish 前后。
- 长时间压力测试不得死锁或破坏计数器。

## 24. 集成测试

### 24.1 本地 SHM

- INFO、READ、WRITE、UNMAP、ZERO、CLOSE 完整流程。
- 请求线程与取消线程竞争。
- Provider 初始化各阶段故障注入。
- fatal error 后不继续处理请求。
- Mapping/Event/Thread/Handle 全部释放。

### 24.2 VirtualAlloc

- 64 GiB 空盘不按容量 commit。
- 1 TiB reserve 成功或原子返回明确容量错误。
- 首次写一个块只增加约 64 KiB committed memory。
- 完整块 UNMAP 后 `VirtualQuery` 显示未提交。
- Store 析构后整个 reserve region 释放。
- commit 失败后 state、pin 和 counter 一致。

### 24.3 Fake backing

- 确定性内容和 `AllZero`。
- 单块延迟和指定块失败。
- 取消和 deadline。
- 同块并发只执行一次 load。
- hard limit 下 uncached read-through。

## 25. 真实驱动 E2E

本章节只能在 M0 最终 GO 后执行。

### 25.1 M1 gate

- 只使用本项目程序和 `phdskmnt.sys`。
- 不加载 `aimapi.dll`、`aim_ll`、AIM GUI 或 RamDyn。
- 创建唯一测试设备。
- INFO 公布 UNMAP 和 ZERO。
- 完成原始 READ/WRITE 和真实 UNMAP/ZERO。
- 精确删除本项目设备。
- 前后 AIM device 和 PhysicalDrive 清单一致。

### 25.2 M2 NTFS gate

创建 64 GiB 易失性动态盘：

1. 初始化 GPT。
2. 创建 NTFS。
3. 分配盘符或挂载目录。
4. 创建、修改、重命名和删除文件。
5. 写入并校验至少 4 GiB 数据。
6. 确认 Windows DeleteNotify/ReTrim 可用。
7. 执行 ReTrim。
8. 证明 committed memory 下降。
9. 卸载并精确删除测试设备。
10. 验证未影响其他磁盘或 AIM 设备。

M1/M2 没有持久化，卸载后数据丢失符合预期。

## 26. 性能与资源验收

固定参考机器必须记录 Windows、CPU、RAM、驱动和可执行文件版本。

- 64 GiB 空盘 Ready 后 Provider committed memory 不超过 128 MiB。
- 写入 1 GiB 后新增 committed memory 不超过 1.10 GiB 加固定 metadata 开销。
- 对 1 GiB 完整块区域 UNMAP 后，10 秒内释放至少 90% 对应 committed memory。
- 顺序吞吐达到 M0 固定内存设备基线的至少 70%。
- 4 KiB 随机 IOPS 达到 M0 基线的至少 60%。
- 正常请求路径不得产生无界逐请求堆分配。
- 20 次创建、读写、UNMAP、删除循环无设备、句柄、线程、Mapping 或虚拟地址泄漏。

性能优化不得牺牲边界检查、Dirty 安全、精确设备归属、Event 顺序和错误可诊断性。

## 27. 分阶段门槛

### M1-A：协议库

- 六种请求完整支持。
- ABI、fuzz、错误映射通过。
- Fake `IBlockDevice` 通过。

### M1-B：本地 SHM 会话

- 生命周期、取消、超时、fatal behavior 和资源释放通过。

### M1-C：真实驱动

- 仅在 M0 GO 后运行。
- 创建、协议调用和精确删除通过。
- 无 AIM 用户态依赖。

### M2-A：动态块内核

- VirtualAlloc、状态机、稀疏 metadata 和模型测试通过。

### M2-B：并发与资源压力

- 并发、eviction、hard limit、snapshot 和故障注入通过。

### M2-C：真实 NTFS 动态盘

- 文件系统正常使用。
- UNMAP/ZERO 实际释放 RAM。
- 达到资源和性能最低指标。

前一门槛未通过时不得宣布后一门槛完成。

## 28. GO/NO-GO

M1 GO 必须同时满足：

- M1-A、M1-B、M1-C 全部通过。
- 真实驱动链路无 AIM 用户态依赖。
- 无设备、命名对象、线程和句柄残留。
- 所有协议失败均可诊断。

M2 GO 必须同时满足：

- M2-A、M2-B、M2-C 全部通过。
- 状态机、随机模型和并发测试通过。
- NTFS 动态盘可正常使用。
- UNMAP/ZERO 确实释放 committed memory。
- Dirty 不因压力或竞态丢失。
- 达到最低性能指标。

停止条件：

- M0 namespace contract 未解决：BLOCKED，不运行真实 driver gate。
- 协议或状态机出现可重复数据错误：NO-GO。
- 无法精确清理自己的设备：NO-GO。
- Dirty 在压力下丢失：NO-GO。
- UNMAP 后 backing 旧数据复活：NO-GO。
- 仅性能未达标但正确性完整：记录偏差并进入性能根因分析，不得宣称 M2 GO。

不得以“M3 会解决”为理由忽略 M1/M2 的协议、内存和数据正确性失败。

## 29. M3 接入契约

M3 可以新增：

- `CompositeVhdJournalBackingStore`，实现 `IBackingStore`。
- `CheckpointEngine`，消费 `copy_dirty_batch()`。
- journal durability barrier。
- 成功 barrier 后调用 `mark_persisted()`。

M3 不应要求修改：

- Proxy request decoder。
- `IBlockDevice` READ/WRITE/UNMAP/ZERO 语义。
- 64 KiB 块大小。
- ZeroOverride 防旧数据复活规则。
- version 条件式 Dirty 清理。
- 内存软、硬限制语义。

如果 M3 无法在这些边界内实现，应先修订设计，不得在实现中绕过 version 和 Dirty 安全规则。

## 30. 参考资料

- 项目总体设计：`docs/superpowers/specs/2026-07-31-aim-driver-only-clean-slate-design.md`
- M0 操作规格：`docs/superpowers/specs/2026-08-01-m0-driver-only-operational-spec.md`
- AIM M0 协议合同：`docs/protocol/aim-m0-contract.md`
- AIM `imdproxy.h`：<https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/d379caa027d21902e45fce8c9daf02fd116c2dda/Unmanaged%20Source/phdskmnt/inc/imdproxy.h>
- AIM `proxy.cpp`：<https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/d379caa027d21902e45fce8c9daf02fd116c2dda/Unmanaged%20Source/phdskmnt/proxy.cpp>
- Windows `VirtualAlloc`：<https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc>
- Windows `VirtualFree`：<https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree>
- Windows `VirtualQuery`：<https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery>
- Windows `DEVICE_DATA_SET_RANGE`：<https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddstor/ns-ntddstor-_device_data_set_range>
