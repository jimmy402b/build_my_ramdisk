# M3 VHDX 镜像、增量 Checkpoint 与一致性恢复 SPEC

**状态：** 设计已确认，等待实现计划
**日期：** 2026-08-04
**基线：** `a37ce86`（M2 GO）
**目标工具链：** MSVC、C++20、64 位 Windows
**依赖边界：** 继续使用已验证的 AIM 内核驱动；不依赖 AIM Toolkit 用户态程序、DLL 或服务

## 1. 结论

M3 采用以下确定方案：

- 用标准 dynamic、non-differencing VHDX 保存经过压缩的基础磁盘状态；
- 用项目自有 append-only journal 保存后续增量 checkpoint；
- 用双槽 manifest 决定唯一可恢复的已提交状态；
- 用 M2 的 `IBackingStore` 在第一次访问块时从 journal 或 VHDX 按需加载；
- 在 M2 管理面增加一致性快照/COW epoch，使一个 checkpoint 中的所有块来自同一个逻辑时间点；
- 普通写入在进入 RAM 后即可完成，只有成功发布的 checkpoint 才承诺断电恢复；
- M3 提供手动 checkpoint、恢复、校验和压缩能力，定时调度、服务化和 GUI 留给 M4。

M3 完成后，产品应能创建一个镜像 bundle，将 Windows 已分区和格式化的磁盘状态写入该 bundle，异常退出后重新创建同容量 RAM Disk，在不重新格式化的情况下恢复 NTFS，并且仅把实际访问的数据块加载到物理内存。

## 2. 当前基线与必须补齐的缺口

M2 已提供：

- 稀疏虚拟地址保留和按块物理内存提交；
- `IBackingStore::load_block()` 按需加载；
- 干净块回收和后续重新加载；
- `copy_dirty_batch()` 的逐块版本化脏数据复制；
- 严格更新的 `BackingView` 和 `BackingGeneration`；
- `mark_persisted()` 的版本、类型和代数校验；
- DATA、ZERO、DISCARD 三种脏记录语义；
- 取消、deadline、关闭、内存压力和并发测试。

M2 当前的 `copy_dirty_batch()` 每次只锁定一个块：它读取该块的版本和类型，复制该块，然后继续扫描。它能防止“较新的写入被较旧 checkpoint 错误清为 Clean”，但不能保证跨多个 batch 得到的整盘内容属于同一个时间点。NTFS 元数据和数据可能在扫描期间继续变化。

因此，M2 GO 仍代表逐块状态机和 generation-safe backing handoff 已完成；M3 不能把它误写成“已经具有整盘一致性快照”。M3 的第一个前置交付必须是 M2.1 一致性快照扩展。

## 3. 目标

M3 必须实现以下能力：

1. 创建新的空白持久化镜像 bundle，并记录不可变磁盘几何信息。
2. 将一个一致性时间点上的所有 M2 脏块增量写入 journal。
3. 只有在 journal 和 manifest 都达到持久化边界后才报告 checkpoint 已提交。
4. 进程终止、机器重启或文件尾部损坏后，只恢复到旧的完整 checkpoint 或新的完整 checkpoint。
5. 从 manifest、journal 和基础 VHDX 构造不可变 `BackingView`。
6. 启动时不预加载整个镜像；块只在 Windows 真正读取时进入 RAM。
7. 支持 DATA、ZERO 和 DISCARD 的恢复，禁止旧数据复活。
8. 将过大的 journal 压缩进新的标准 VHDX，同时允许磁盘继续正常读写。
9. 生成可审计、机器可读的操作和验收证据。
10. 在真实 AIM 内核代理和 NTFS 上完成“格式化一次、多次 checkpoint、多次异常恢复”的端到端验收。

## 4. 非目标

以下内容不属于 M3：

- 定时器、Windows 服务、开机自动恢复和关机策略；
- GUI、托盘程序和多磁盘管理界面；
- 每次普通写入都同步落盘的 write-through durability；
- VSS provider、应用一致性数据库快照或分布式事务；
- 加密、压缩、去重、远程镜像和网络复制；
- VHDX differencing disk、父盘链和任意第三方 VHDX 导入兼容；
- 修改 AIM 内核驱动、IMDPROXY wire protocol 或 M1 `IBlockDevice` 正常读写接口；
- 宣称性能已达到 Primo Ramdisk 的最终水平。

M4 将使用 M3 的稳定接口实现定时 checkpoint、Windows 卷 flush 协调、自动压缩、启动/停止编排、策略配置和用户界面。

## 5. 术语与持久化承诺

### 5.1 术语

- **普通写入完成：** 数据已被 M2 接收并保存在 RAM 中，不代表已经持久化。
- **一致性切点：** M2.1 短暂阻止新 mutation、排空已经进入的 mutation，并记录的整盘逻辑时间点。
- **checkpoint：** 切点时需要持久化的脏状态以及对应的不可变索引根。
- **提交点：** 新 manifest 槽成功 `FlushFileBuffers()` 的时刻。
- **基础代数 `base_generation`：** 已合并进基础 VHDX 的完整逻辑状态代数。
- **checkpoint 代数 `checkpoint_generation`：** manifest 当前发布的完整 composite 状态代数。
- **backing 代数：** M2 `BackingView::generation`，必须与 `checkpoint_generation` 相同。
- **journal epoch：** journal 轮换标识；只在创建新 journal 或压缩轮换时增加。
- **durable view：** 一个不可变的 VHDX 文件、journal committed length、索引根和 generation 组合。

### 5.2 用户可依赖的语义

M3 只承诺恢复到最后一次成功 checkpoint：

- checkpoint 成功之前异常断电，允许丢失上一次成功 checkpoint 之后的全部写入；
- checkpoint 成功之后异常断电，必须恢复该 checkpoint 的完整状态；
- checkpoint 之后尚未再次提交的写入允许丢失；
- 不允许恢复出由两个 checkpoint 混合组成的磁盘；
- 不允许把损坏、未知或无法读取的块静默替换为零；
- 不允许 ZERO/DISCARD 后的旧数据从旧 VHDX 或旧 journal 中重新出现。

如果 manifest slot 写入已经开始，而最终 flush 明确返回失败，操作结果必须标记为 `CommitIndeterminate`：当前进程不能证明该 slot 是否已经到达稳定存储。此状态下禁止继续修改 bundle；下一次恢复允许选择旧完整 generation 或新完整 generation，但仍不允许混合 generation。只有成功 checkpoint 才向用户承诺新 generation 已持久化。

### 5.3 Windows 缓存边界

checkpoint 只覆盖一致性切点之前已经到达 M2 的写入。尚在应用或 Windows 文件缓存中、尚未送到虚拟磁盘的内容不属于 M3 能观察的状态。

现有 IMDPROXY 协议没有独立的用户态 FLUSH request，M1 `IBlockDevice` 也没有 `flush()`。M3 不增加虚假的代理命令。产品级 M4 在调用 checkpoint 前必须对目标 Windows 卷执行 `FlushFileBuffers()` 并等待成功；M3-V6 真实 NTFS 验收也必须执行同样步骤。该卷 flush 不负责冻结后续写入，后续一致性由 M2.1 切点和 COW 保证。

## 6. 总体架构

```text
Windows 文件系统
    ↓
AIM 内核驱动
    ↓ IMDPROXY
M1 ShmProxyServer / RequestProcessor
    ↓ IBlockDevice
M2 DynamicBlockStore
    ├─ resident dirty/clean blocks
    ├─ M2.1 consistent snapshot + COW epoch
    └─ immutable BackingView
           ↓ IBackingStore
M3 CompositeImageBackingStore
    ├─ JournalBackingStore + persistent sparse index
    └─ VhdxBackingStore

M3 management plane
    ├─ ImageRepository
    ├─ CheckpointEngine
    ├─ RecoveryEngine
    ├─ CompactionEngine
    ├─ ImageVerifier
    └─ EvidencePublisher
```

依赖方向必须保持单向：

- M1 只依赖 `IBlockDevice`；
- M2 数据面只依赖 `IBackingStore`；
- M2.1 向管理面增加一致性快照接口，但不依赖具体文件格式；
- M3 依赖 M2 管理接口和抽象 backing contract；
- VHDX、journal 和 manifest 代码不能反向依赖 AIM 生命周期或 CLI；
- M4 可以依赖 M3，M3 不依赖 M4。

### 6.1 组件职责

| 组件 | 唯一职责 | 主要依赖 |
|---|---|---|
| `M2SnapshotCoordinator` | 建立切点、保存首次覆盖前镜像、按切点输出脏块 | `DynamicBlockStore` 内部状态 |
| `ManifestCodec` | 读写、校验和选择双 manifest 槽 | Windows 文件 I/O、CRC32C |
| `JournalWriter` | 追加记录、构建 COW 索引、写 commit、flush、readback | 文件 I/O、索引 codec |
| `JournalReader` | 按 committed root 查询块、验证记录 | 文件 I/O、索引 codec |
| `VhdxBackingStore` | 严格解析产品 VHDX 子集并按块读取 | VHDX codec、文件 I/O |
| `CompositeImageBackingStore` | journal 优先、VHDX 回退、ZERO/DISCARD 归零 | 两种 backing reader |
| `CheckpointEngine` | 执行唯一提交顺序并与 M2 generation 对齐 | M2.1、manifest、journal |
| `RecoveryEngine` | 选择可恢复 generation 并构造 immutable view | manifest、journal、VHDX |
| `CompactionEngine` | 把旧 durable view 合成新 VHDX 并轮换 journal | composite reader、writer |
| `ImageVerifier` | 离线检查格式、引用关系、哈希和几何 | 所有只读 codec |
| `EvidencePublisher` | 原子发布机器可读操作证据 | JSON、文件 I/O |

每个 codec、engine 和 backing store 必须可以通过内存文件或 fake I/O 单独测试。CLI 只做参数解析和编排，不包含格式或提交逻辑。

## 7. M2.1 一致性快照/COW epoch

### 7.1 管理接口

M2.1 增加以下语义等价的窄接口。最终头文件可以调整命名和类型拆分，但行为不得改变：

```cpp
struct ConsistentSnapshotToken {
    std::uint64_t snapshot_id;
    BackingGeneration source_backing_generation;
    std::uint64_t cutoff_dirty_epoch;
};

Result<ConsistentSnapshotToken> begin_consistent_snapshot(
    const OperationContext& context);

Result<std::vector<DirtyBlockSnapshot>>
copy_consistent_snapshot_batch(
    const OperationContext& context,
    const ConsistentSnapshotToken& token,
    DirtyScanCursor& cursor,
    std::size_t max_bytes);

Result<void> finish_consistent_snapshot(
    const ConsistentSnapshotToken& token);

Result<void> abort_consistent_snapshot(
    const ConsistentSnapshotToken& token);
```

M3 v1 每个 `DynamicBlockStore` 最多有一个活动 snapshot token。token 绑定 store 实例、snapshot id、source generation 和 cutoff epoch；跨实例、过期、重复 finish 或错误 generation 的 token 必须失败。

原有 `copy_dirty_batch()` 可以保留给诊断或兼容测试，但 M3 checkpoint 不得使用它组装整盘恢复点。

### 7.2 切点建立

`begin_consistent_snapshot()` 必须：

1. 获得 snapshot coordinator 独占权；
2. 阻止新的 `write()`、`zero()` 和 `unmap()` mutation 进入；
3. 等待已经进入的 mutation 完成或按照 `OperationContext` 取消/超时；
4. 记录当前 `BackingGeneration` 和全局 `dirty_epoch`；
5. 发布活动 snapshot token；
6. 立即重新允许 mutation。

普通 read 可以继续执行。切点期间不得进行 `install_backing_view()`、`mark_persisted()`、第二次 snapshot 或 store close 状态转换。close 可以请求取消并等待 snapshot 协调器退出。

如果切点建立失败，不得留下活动 token、mutation gate 或 COW 资源。

### 7.3 每块 dirty epoch

每个可变 block entry 必须记录最后一次改变其 dirty logical value 的 `dirty_epoch`。全局 epoch 单调增加且不得回绕；达到最大值时，store 必须进入明确的 fatal invariant 状态，不能复用旧 epoch。

对于切点 `E`：

- 在切点时为 dirty 且 `dirty_epoch <= E` 的块属于该 snapshot；
- 在切点时为 clean 的块不属于该 snapshot；
- 仅在切点后首次变 dirty、`dirty_epoch > E` 的块不属于该 snapshot；
- 在切点时 dirty、切点后又被改写的块，snapshot 必须返回改写前的版本、类型和内容。

### 7.4 首次覆盖前 COW

活动 snapshot 存在时，mutation 在改写属于该 snapshot 且尚未被消费的块之前，必须保存一次 pre-image：

- DATA 保存完整 M2 block payload、原版本、原 dirty kind、原 base generation 和原 dirty epoch；
- ZERO/DISCARD 只保存类型和元数据，不分配 64 KiB payload；
- 同一 snapshot、同一块只保存第一次 pre-image；
- mutation 真正发布前，pre-image 必须已经可由 snapshot reader 获得；
- snapshot reader 返回 batch 后，payload 由返回对象拥有，内部 COW 项可以释放；
- 对已经被 snapshot reader 消费的块，后续 mutation 不再需要为该 token 创建 COW。

`copy_consistent_snapshot_batch()` 对每个候选块先查询 COW 表，再查询当前 block entry。它返回的所有记录必须严格属于 token 的切点，并保留供 `mark_persisted()` 使用的 expected version、kind 和 source generation。

### 7.5 COW 资源与写入背压

COW 内存必须计入独立的 `snapshot_cow_max_bytes`，最小配置必须至少容纳一个 DATA block。禁止无界增长。

如果 mutation 需要保存 DATA pre-image 而 COW budget 已满：

1. mutation 可以短暂等待 snapshot reader 消费已有项；
2. coordinator 应优先提示 checkpoint worker 扫描该阻塞块；
3. 等待达到 `snapshot_writer_stall_limit` 时，必须原子 abort 整个 snapshot；
4. snapshot abort 后，正常 mutation 继续执行；checkpoint 返回 `SnapshotResourceExhausted` 或 `SnapshotAborted`；
5. 不得仅因后台 checkpoint 的 COW 内存不足而静默丢写或覆盖 pre-image。

默认 `snapshot_writer_stall_limit` 为 250 ms，可配置但必须有上限。原始写请求自身的取消、deadline 和 close 语义仍然优先。

### 7.6 必须保持的不变量

- 一个 token 返回的记录集合等于切点时全部 dirty logical values；
- 同一个 block 在一个 token 中最多出现一次；
- 返回 DATA payload 后发生的写入不能改变已返回 payload；
- abort 后所有 COW 项、pin、waiter 和 gate 最终归零；
- finish 只能用于已经完整扫描的 token；M2 只验证扫描和 token 状态，不感知文件提交，M3 在提交或 reconciliation 不再需要 COW 后调用 finish；
- `mark_persisted()` 的现有版本冲突是正常并发结果：说明切点后有新写入，该块继续保持 dirty；
- generation 冲突、token 归属错误或状态机不可能路径属于 fatal invariant，必须阻止下一次持久化操作。

## 8. 镜像 bundle 与磁盘格式

### 8.1 文件布局

一个逻辑镜像由同目录下的多个文件组成：

```text
MyRamDisk.rdm
MyRamDisk.base-000001.vhdx
MyRamDisk.journal-000001.rdj
MyRamDisk.compact-<operation-id>.tmp
```

`.rdm` 是用户选择和打开的主入口；VHDX 与 journal 是其引用的实现文件。临时文件从不代表已提交状态。

manifest 中的 `file identity` 必须对应项目文件自己的 immutable identity：VHDX 使用其 validated VHDX file/data-write identity，journal 使用 journal header UUID；如果平台还能提供打开时的 file ID，也必须一并核对，但不能只依赖可变的文件名或大小。

manifest 只能保存最长 255 bytes 的 UTF-8 相对文件名，不允许绝对路径、驱动器前缀、UNC、`..`、路径分隔符逃逸、alternate data stream 或重解析点逃逸。解析后每个目标的规范绝对路径必须仍位于 `.rdm` 所在目录。

运行时以 Windows handle sharing policy 独占 bundle 的修改权。第二个 writer 必须返回 `ImageInUse`。只读 inspect 可以共享读取，但不能绕过 committed length。

### 8.2 通用编码规则

- 所有项目自有格式使用 little-endian；
- 所有结构包含 magic、major/minor version、结构长度和 reserved 字段；
- reserved 字段写零，reader 对当前 major version 要求其为零；
- 所有 offset、length 和乘加运算先做 checked arithmetic；
- 所有持久化结构和 payload 使用 CRC32C Castagnoli；
- checkpoint commit 额外保存 SHA-256 chain digest；
- 校验是意外损坏检测，不构成加密认证或恶意篡改防护；
- 未知 major version 拒绝，已知 major 下未知但明确标记 optional 的 minor 字段可以忽略；
- writer 在 M3 期间只生成 format major 1。

### 8.3 双槽 manifest：`.rdm`

`.rdm` 在创建时预分配为固定 8192 bytes，由两个 4096-byte slot 组成：slot A 位于 offset 0，slot B 位于 offset 4096。不得通过 truncate-rewrite 更新整个文件。

每个 slot 至少包含：

- magic 和 manifest format version；
- slot length 和 slot CRC32C；
- image UUID；
- manifest/checkpoint generation；
- virtual disk byte size；
- logical sector size、physical sector size 和 M2 block size；
- base VHDX 相对文件名、file identity、expected length 和 `base_generation`；
- journal 相对文件名、file identity、`journal_epoch`；
- journal committed length；
- checkpoint commit record offset、length、CRC32C 和 SHA-256 digest；
- persistent index root offset、length、CRC32C；
- feature flags；
- 创建时间和最后提交时间，仅用于诊断，不参与 generation 选择。

恢复时分别验证 A/B：

1. slot 自身格式和 CRC；
2. UUID、几何和 generation 关系；
3. 路径安全和文件 identity；
4. VHDX 严格 profile；
5. journal header、committed length、commit record 和 root；
6. commit digest chain。

选择“完整验证通过的最高 generation”。最高 slot 失败时允许回退到较低的完整 slot。两个都失败时返回明确损坏错误并拒绝创建 RAM Disk。

更新时总是覆盖当前较旧或无效的 slot，写满 4096 bytes，随后 `FlushFileBuffers(manifest)`。slot CRC 使 torn write 无法被误选。

### 8.4 Journal：`.rdj`

journal 是 append-only 文件。4096-byte immutable header 包含 image UUID、journal epoch、格式版本、磁盘几何、前一基础 checkpoint generation、前一 checkpoint chain digest 和 header CRC。这样 journal 轮换后仍能验证跨 epoch 的 commit 链；generation 0 的初始 journal 使用明确的 genesis 标记。

记录从 4096-byte 边界开始，所有记录 4096-byte 对齐，类型包括：

- `DATA`：block index、snapshot version、source generation、payload length、payload CRC 和一个完整 M2 block payload；
- `ZERO`：block index、snapshot version 和 generation，无 payload；
- `DISCARD`：与 ZERO 一样返回逻辑零，但保留原始语义供统计和未来策略使用；
- `INDEX_NODE`：固定 4096-byte copy-on-write sparse index node；
- `CHECKPOINT_COMMIT`：发布该 journal 内一个不可变索引根和 generation。

最后一个物理 block 的 DATA payload 仍按完整 M2 block 编码，超出逻辑磁盘末尾的字节必须为零，reader 不得把这些字节暴露给调用者。

每个 record header 包含 magic、type、format version、record length、record CRC、checkpoint generation 和 operation id。reader 必须拒绝未知 mandatory type、重叠 offset、越过 committed length、错位、重复索引键和 CRC 不匹配。

`CHECKPOINT_COMMIT` 至少包含：

- image UUID 和 journal epoch；
- previous checkpoint generation 和新 generation；
- previous commit digest 和新 chain digest；
- 本 checkpoint DATA/ZERO/DISCARD 计数；
- index root descriptor；
- journal committed end offset；
- geometry digest；
- commit record CRC32C。

checkpoint chain digest 的 canonical 输入固定为：ASCII domain `MYRD-CHECKPOINT-V1`、previous digest、image UUID、journal epoch、previous/new generation、geometry digest、本 checkpoint 按 append 顺序写入的全部 DATA/ZERO/DISCARD 和 INDEX_NODE 完整 record bytes（padding 必须为零）、以及 digest/CRC 字段置零后的 commit metadata。所有整数沿用 little-endian wire bytes。writer 在 append 时流式计算 SHA-256，readback 必须从文件 bytes 独立重算。genesis digest 使用 domain `MYRD-GENESIS-V1`、image UUID、geometry digest 和 generation 0 base identity 计算。

manifest 是发布 authority。即使 journal 尾部存在 CRC 正确的新 commit，只要没有被有效 manifest 引用，也必须忽略。

### 8.5 Persistent sparse index

journal index 是 immutable copy-on-write B+tree，key 为 M2 block index，value 为最新已提交 record descriptor：record type、record offset、record length、payload CRC 和 generation。

要求：

- node 固定 4096 bytes，并有 node CRC；
- 修改 checkpoint 只重写从受影响叶子到 root 的路径；
- root descriptor 由 commit record 和 manifest 双重引用；
- lookup 不得扫描整个 journal；
- startup 只读取 root 和必要元数据，其他 node 按需读取；
- tree depth 最大为 8，超过时按损坏拒绝；
- node cache 有可配置硬上限并支持 clean eviction；
- ZERO/DISCARD value 阻止继续回退旧 journal 或基础 VHDX；
- 索引中不存在的 block 才允许回退基础 VHDX。

### 8.6 基础 VHDX profile

M3 v1 只读写以下严格 profile：

- VHDX；
- dynamic；
- non-differencing；
- 无 parent locator；
- clean/empty VHDX log；
- VHDX data block size 固定 2 MiB；
- logical sector size 为 512 或 4096，并与 manifest/M2 完全一致；
- physical sector size 为 4096；
- virtual disk size 与 manifest 完全一致；
- 必需 identifier、双 header、双 region table、metadata region 和 BAT 均通过 checksum 与边界验证。

运行时 `VhdxBackingStore` 直接解析文件并读取 BAT/data block，不把 VHDX 作为第二块磁盘附加到 Windows，从而避免重复磁盘签名和自动挂载冲突。

M3 v1 只承诺读取本产品生成且符合该 profile 的 VHDX。其他 VHDX 返回 `UnsupportedVhdx`，不能尝试猜测。项目自有 writer 的输出必须通过 Windows 原生 Virtual Disk API 独立打开和只读挂载测试。

## 9. 创建、恢复和按需读取

### 9.1 创建新镜像

`create_image()` 必须：

1. 验证容量、sector size、目录、名称和磁盘空间；
2. 获得 bundle writer lock；
3. 生成 image UUID 和 VHDX file identity；
4. 创建 generation 0 的稀疏全零 dynamic VHDX；
5. 创建 epoch 1 journal header 和 generation 0 空 commit/root；
6. flush 并 readback VHDX、journal；
7. 创建预分配双槽 `.rdm`；
8. 写入并 flush generation 0 manifest slot；
9. 用 `ImageVerifier` 重新打开 bundle；
10. 返回 geometry 和 generation 0 `BackingView`。

任何失败都不得发布看似有效的 manifest。未被有效 slot 引用的创建残留可以在持有 writer lock 时清理。

### 9.2 打开与恢复

`open_image()` 的顺序固定为：

1. 打开并锁定 `.rdm`；
2. 独立读取和验证两个 slot；
3. 按 generation 从高到低验证完整引用链；
4. 选择最高完整 candidate；
5. 以不可变 committed length 打开 VHDX、journal 和 index root；
6. 构造 `CompositeImageBackingStore`；
7. 返回 geometry、generation、恢复诊断和 `BackingView`；
8. 上层使用该 view 创建 `DynamicBlockStore`；
9. 只有 backing 安装完成后，才允许 M1 将磁盘暴露给 Windows。

恢复不得读取 manifest 之外的 journal tail 来推断更“新”的状态。可安全 truncate 未提交 tail 时可以在获得 writer lock 后执行；truncate 失败时可以保留 tail，并从新的 journal epoch 继续，不能覆盖 committed bytes。

### 9.3 按需加载

`CompositeImageBackingStore::load_block()` 的解析顺序固定为：

1. 验证 operation context、block index 和 destination size；
2. 在当前 immutable journal root 中查找 block；
3. 命中 DATA 时从记录 offset 读取、验证 CRC 并返回 `BlockLoadKind::Data`；
4. 命中 ZERO/DISCARD 时返回 `BlockLoadKind::AllZero`；
5. 未命中时从 VHDX BAT 对应 logical range 读取；
6. VHDX block 未分配时返回 `AllZero`；
7. 任意被引用数据损坏时返回 `BackingDataCorrupt`，不得继续回退旧数据。

M2 负责把第一次读取的 DATA block 提交进 RAM、缓存和后续回收。M3 reader 不维护第二份无界数据 cache；只允许有上限的 VHDX metadata、index node 和 I/O buffer cache。

旧 `BackingView` 使用 shared ownership 持有文件 handles 和 reader。安装新 view 后，已经开始的 load 继续使用旧 view；只有最后一个引用释放后旧 handle 才能关闭。

## 10. Checkpoint 算法与唯一提交顺序

### 10.1 前置条件

- session 状态为 `Ready`；
- 当前 M2 backing generation 等于 manifest checkpoint generation；
- 没有活动 checkpoint、compaction 或 reconciliation；
- bundle writer lock 仍有效；
- journal 文件 identity、epoch 和 committed length 与 manifest 相同；
- generation 加一不会回绕；
- 生产编排已成功执行 Windows 卷 flush；M3 核心 raw 测试可以显式声明无 volume barrier。

### 10.2 固定顺序

checkpoint generation 必须严格为当前 generation `G + 1`：

1. 调用 M2.1 `begin_consistent_snapshot()`，取得 source generation `G` 的 token；
2. 分 batch 调用 `copy_consistent_snapshot_batch()`；
3. 按 block index 的确定顺序 append DATA/ZERO/DISCARD records；
4. append 新的 copy-on-write index nodes；
5. append `CHECKPOINT_COMMIT(G + 1)`；
6. 对 journal 调用 `FlushFileBuffers()`；
7. 从磁盘重新读取本次新增的全部 record、index node 和 commit，重新计算 CRC/digest，验证 committed end；
8. 进入不可取消的 manifest publish attempt，将引用该 commit 的完整 4096-byte manifest 写入较旧/无效 slot；
9. 对 manifest 调用 `FlushFileBuffers()`；此时达到唯一提交点；
10. 构造 generation `G + 1` 的 immutable composite view；
11. 调用 `install_backing_view()`；
12. 对 snapshot 返回的每个记录调用 `mark_persisted(block, expected_version, expected_kind, G + 1)`；
13. 正常 version conflict 表示切点后有新写入，该块保持 dirty；
14. 调用 `finish_consistent_snapshot()`；
15. 发布成功结果和证据。

ack 所需的 block/version/kind 元数据必须从本次 checkpoint 的有界 batch 或已提交 journal records 流式回放，不能为了步骤 12 在 RAM 中保留与全部 dirty payload 等量的第二份数据。

在步骤 8 的 manifest slot 写入开始之前发生的失败必须 abort token，返回 `NotCommitted`，保留旧 view 和全部当前 dirty state。journal 尾部作为 orphan tail 忽略。

步骤 8 开始后不再接受取消。进程在 slot write/flush 期间被终止时没有 API 返回值；下一次恢复允许选择 G 或完整的 G+1。若 slot write 或 manifest flush 返回错误，当前进程必须返回 `CommitIndeterminate` 并进入 commit resolution 状态，不能声称 `NotCommitted`。步骤 9 成功之后不得再返回 `Cancelled`、`DeadlineExceeded` 或普通失败，因为新 generation 已知持久化；后续错误进入 reconciliation。

### 10.3 提交线性化和返回值

`checkpoint()` 返回结构化 outcome：

```cpp
enum class CommitDisposition {
    NotCommitted,
    CommitIndeterminate,
    Committed,
    CommittedNeedsReconciliation
};

struct CheckpointOutcome {
    CommitDisposition disposition;
    BackingGeneration generation;
    bool manifest_candidate_verified;
    bool backing_view_installed;
    std::uint64_t acknowledged_blocks;
    std::uint64_t still_dirty_blocks;
    std::optional<Error> error;
};
```

- 提交点前错误使用 `NotCommitted` 并在 `error` 中携带具体原因；
- manifest publish attempt 已开始但 durable 结果无法证明时使用 `CommitIndeterminate`；
- 提交点后取消请求只停止非必要清理，不能改变 `Committed` 事实；
- `mark_persisted()` 的 expected version conflict 不降低 committed 状态；
- 无法安装 view、generation 冲突或无法完成必要 ack 协调时返回 `CommittedNeedsReconciliation`，并在 `error` 中记录 post-commit 原因；
- post-commit 状态不能通过普通 `Result` error 隐藏已提交事实。

### 10.4 Commit resolution

`CommitIndeterminate` 时，session 进入 `ResolvingCommit`：

- 阻止新的 checkpoint、compaction、truncate、rotation 和 cleanup；
- 保留新旧 slot 引用的全部文件；
- 允许普通 RAM Disk I/O 继续，但仍把 snapshot blocks 视为 dirty；
- 尝试重新打开、验证 candidate 并重新执行 manifest flush；
- 后续 flush 成功且新 candidate 完整时转为 `CommittedNeedsReconciliation`；
- 能证明 slot 无效且旧 generation 仍是唯一完整 candidate 时，才可以安全转为 `NotCommitted` 并 abort snapshot；
- 如果当前进程无法证明任一结果，停止 persistence management，并将最终选择交给下一次正常 recovery。

这不是 mixed-state 豁免。无论 resolution 结果如何，恢复内容仍只能是完整 G 或完整 G+1。

### 10.5 Reconciliation

manifest 已提交但运行时尚未对齐时，session 进入 `Reconciling`：

- 阻止新的 checkpoint 和 compaction；
- 允许现有 RAM Disk 普通 I/O 继续；
- 重试构造/安装已提交 view 和安全 ack；
- 未 ack 的块继续 dirty，不能被当作 clean 回收；
- 如果当前进程无法恢复，明确报告 committed generation 后停止 persistence management；
- 进程重启后 `RecoveryEngine` 直接从新 manifest 恢复，不需要旧内存状态。

## 11. Compaction 与文件生命周期

### 11.1 触发

M3 提供显式 `compact()` 和 `should_compact()`，不提供定时器。建议阈值由配置表达，例如 journal physical bytes、journal/base 比例、checkpoint 数和索引深度。M4 决定何时调用。

### 11.2 算法

对于当前 durable generation `G`：

1. 持有 generation `G` 的 immutable view；
2. 创建 operation-id 唯一的临时 VHDX；
3. 枚举旧 VHDX BAT 与 journal index 中的非零 allocated extents；
4. 从 immutable view 读取这些 extents 并生成新的稀疏 VHDX；
5. flush 临时 VHDX，完整 readback format metadata 和已写 extents；
6. 使用项目 reader 和 Windows Virtual Disk API 对临时 VHDX 做不附加到当前 Windows 的离线格式验证；不能在 RAM Disk 仍在线时把具有相同磁盘身份的临时 VHDX 作为第二块磁盘 attach；
7. 将临时文件以 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 发布为新的 final VHDX 名称；
8. 创建并 flush 新 journal epoch；
9. 建立 M2.1 切点，将压缩期间相对 generation `G` 的当前 dirty state 写入新 journal；
10. 写入新 commit，并按正常 checkpoint 顺序发布 generation `G + 1` manifest；
11. 安装新 composite view 并执行 ack；
12. 进入正常文件保留与清理。

即使切点时没有 dirty block，compaction 也发布一个 generation `G + 1` 的空增量 commit，使 M2 backing generation 保持严格单调。

checkpoint 和 compaction 在 M3 v1 中全局串行；普通 read/write/zero/unmap 可以继续。压缩 source 必须是 immutable durable view，不能直接遍历不断变化的 live RAM。

### 11.3 旧文件保留

文件只有同时满足以下条件才可删除：

- 没有任一有效 manifest slot 引用；
- 没有旧 `BackingView` 或在途 load 持有；
- 新 bundle 已通过 recovery reopen 验证；
- 不是当前 reconciliation 所需文件。

压缩刚提交后，旧 slot 仍可能引用旧 VHDX/journal，因此必须保留旧文件。只有后续成功更新另一个 slot，使两个有效 slot 都不再引用旧文件，并且所有 view 引用释放后，才可删除。删除失败是可报告的 cleanup 问题，不能撤销已提交 checkpoint。

无 manifest 引用的 `.tmp`、orphan journal epoch 和未完成 final 文件可在获得 writer lock 后清理；清理逻辑必须再次做规范路径和 file identity 检查。

## 12. Session 状态机与并发规则

```text
Closed
  ↓ open/create
Opening
  ├─ failure → Closed
  └─ success → Ready
                  ├─ Checkpointing ── pre-commit failure → Ready
                  │        ├─ committed → Ready
                  │        ├─ publish unknown → ResolvingCommit
                  │        └─ post-commit issue → Reconciling
                  ├─ Compacting ───── 同上
                  ├─ Verifying(read-only, concurrent by policy)
                  └─ close → Closing → Closed

Reconciling
  ├─ success → Ready
  └─ unrecoverable runtime invariant → PersistenceFaulted

ResolvingCommit
  ├─ proved old → Ready
  ├─ proved new → Reconciling
  └─ unresolved → PersistenceFaulted
```

规则：

- 单个 image session 同时最多有一个修改型 operation；
- 第二个 checkpoint/compaction 返回 `OperationBusy`，M3 不隐式排队；
- read-only inspect 可读取 manifest 已发布的 immutable state；
- close 在提交点前可以请求 operation abort，并等待 token/gate 释放；
- close 在 manifest publish attempt 期间不能把未知结果改写成取消；
- close 在提交点后必须先记录 committed disposition，再释放 session；
- close 不自动发起 checkpoint；关机策略属于 M4；
- `PersistenceFaulted` 阻止新的持久化修改，但不得把 RAM 中尚可服务的数据静默丢弃；
- 所有 wait 都必须响应 stop token、deadline 和 store close。

## 13. 管理 API 形状

M3 必须提供可被 M4 和测试 harness 调用的 C++ 核心 API。以下是语义 contract，不要求所有函数位于同一类：

```cpp
Result<CreatedImage> create_image(
    const OperationContext&, const ImageCreateOptions&);

Result<RecoveredImage> open_image(
    const OperationContext&, const std::filesystem::path&,
    const ImageOpenOptions&);

CheckpointOutcome checkpoint(
    const OperationContext&, PersistentImageSession&,
    DynamicBlockStore&);

CheckpointOutcome compact(
    const OperationContext&, PersistentImageSession&,
    DynamicBlockStore&);

Result<ImageInspection> inspect_image(
    const OperationContext&, const std::filesystem::path&);

Result<VerificationReport> verify_image(
    const OperationContext&, const std::filesystem::path&,
    VerificationDepth);

Result<void> close_image(
    const OperationContext&, PersistentImageSession&);
```

`RecoveredImage` 至少包含 geometry、image UUID、checkpoint generation、base generation、journal epoch、recovery candidate/slot、immutable `BackingView` 和诊断信息。它不负责创建 AIM device；上层必须先用该 view 创建 M2 store，再暴露 M1/AIM 磁盘。

M3 提供薄测试 CLI，至少支持：

- `create`
- `inspect`
- `verify`
- `checkpoint`
- `compact`
- `recover-test`
- `fault-matrix`

CLI 输出 stable JSON，并使用非零 exit code 表示未满足命令目标。CLI 不替代核心库测试。

## 14. 错误模型

M3 至少稳定区分：

| 错误 | 含义 | 是否允许自动重试 |
|---|---|---|
| `InvalidFormat` | 自有格式字段、长度或边界非法 | 否 |
| `UnsupportedVhdx` | 不属于 M3 v1 VHDX profile | 否 |
| `ManifestCorrupt` | 无可用 manifest candidate | 否 |
| `JournalCorrupt` | header、record、index 或 commit 损坏 | 否 |
| `BackingDataCorrupt` | 已引用 DATA/VHDX 内容校验失败 | 否 |
| `ImageInUse` | 另一个 writer 持有 bundle | 稍后可重试 |
| `OperationBusy` | 当前 session 已有修改型 operation | 稍后可重试 |
| `GenerationConflict` | runtime 和 durable generation 不一致 | 进入诊断/reconciliation |
| `SnapshotBusy` | 已有活动 snapshot token | 稍后可重试 |
| `SnapshotAborted` | token 被 close、资源策略或协调器终止 | 是 |
| `SnapshotResourceExhausted` | COW budget 无法在 stall limit 内取得进展 | 调整负载/预算后重试 |
| `CommitIndeterminate` | manifest publish 已开始，但 durable 结果无法证明 | 禁止盲目重试，先 resolution/recovery |
| `DiskFull` | append、VHDX 或 manifest 无可用空间 | 释放空间后重试 |
| `AccessDenied` | ACL、共享模式或管理员权限不满足 | 修复权限后重试 |
| `IoError` | Windows I/O 失败或 short I/O | 视 Win32 code |
| `Cancelled` | 提交点前取消 | 是 |
| `DeadlineExceeded` | 提交点前超时 | 是 |
| `VerificationFailed` | 独立 verifier 未通过 | 否 |
| `FatalInternalInvariant` | 不可能状态、epoch 回绕或所有权破坏 | 否，停止 persistence management |

所有错误都携带 operation、phase、Win32 code（如适用）、image UUID、operation id、generation 和 commit disposition。日志消息不能成为程序判断错误类型的依据。

## 15. 安全性与资源上限

M3 必须防御损坏文件和意外的大输入：

- manifest 文件必须恰好符合已知固定布局，超出或不足均按格式策略处理；
- filenames、record count、index depth、cache entries、batch size 和 payload length 都有硬上限；
- image size、block count、BAT size、offset 加法和对齐使用 checked arithmetic；
- 不信任 file length、sparse range 或 VHDX metadata 中的任何 offset；
- 禁止内存映射整个用户指定文件；
- 不跟随逃出 bundle 的 symlink/junction/reparse point；
- 删除、truncate、rename 前重新确认规范路径、file identity 和 writer lock；
- journal read 不得越过 manifest committed length；
- index lookup 设置最大深度并检测循环和重复 node；
- 所有 I/O buffer 和 cache 有硬预算并计入统计；
- post-commit ack descriptor 使用有界 batch 流式回放，不能形成无界内存列表；
- 不在普通日志或 JSON evidence 中记录用户块内容；
- fault injection 只能由测试构建或显式测试开关启用，生产默认关闭；
- M3 不宣称提供加密或恶意篡改认证。

### 15.1 本地文件系统威胁边界

M3 防御损坏/替换文件、错误路径、已存在的 hard link、reparse escape、并发的本产品
writer，以及在每个受控 mutation 边界能够观察到的身份变化。产品在 in-place
write/truncate/delete/rename 前后都要复核 opened-handle identity、containment、link
count 和 writer authority；事后发现变化时必须 poison capability，不能继续操作。

M3 不承诺抵御拥有同一 Windows 账户/等价文件权限的恶意进程在“最后一次复核”与
in-place I/O 之间抢占创建 hard link 或主动篡改 namespace。单 writer lock 只协调
本产品 session，不是针对同权限 OS 对手的安全边界。真实 NTFS 验证已证明普通共享
模式和 read/write/handle oplock 不能禁止 `CreateHardLinkW`，因此重复一次有限 recheck
不能把该 check/use 窗口变成原子操作。

如果未来把 hostile same-principal namespace actor 纳入目标，必须另立设计里程碑：
要么把每次内容 mutation 改成 immutable copy-on-write file + verified atomic publish，
要么使用经过安全审查的 filesystem minifilter/内核 enforcement。不得在当前用户态
实现上声称已经提供这种保证。

## 16. 可观测性与证据

每个 operation 生成唯一 `operation_id`，日志和最终 JSON 至少包含：

- schema version；
- build commit、build type 和 binary identity；
- image UUID 和规范路径的脱敏表示；
- source/target checkpoint generation；
- base generation 和 journal epoch；
- manifest selected slot 及 fallback 原因；
- operation phase 和 commit disposition；
- snapshot dirty block 数、DATA/ZERO/DISCARD 数；
- COW peak bytes、writer stalls、snapshot aborts；
- journal appended/readback bytes；
- flush 次数和耗时；
- view install、ack success、version conflicts 和 still-dirty 数；
- backing reads、VHDX reads、journal reads、zero hits 和 cache peak；
- compaction input/output physical bytes；
- error domain/code/phase；
- 开始、提交点、结束时间戳和总耗时。

evidence 使用临时文件写入、`FlushFileBuffers()`、原子 rename/replace 发布。报告发布失败不能改变镜像 commit disposition，但验收必须判定证据不完整。

### 16.1 确定性故障注入点

测试构建至少在以下边界支持 nth-occurrence 故障、short write、disk full、取消、deadline 和 `TerminateProcess`：

- snapshot cut 建立前后；
- COW pre-image 分配和发布前后；
- 每个 DATA/ZERO/DISCARD append 前后；
- index node append 前后；
- commit append 前后；
- journal flush 前后；
- readback 每个阶段；
- manifest slot write 的分段位置；
- manifest flush 前后；
- backing view install 前后；
- 每个 ack 前后；
- compaction VHDX create/write/flush/validate/rename；
- 新 journal create/flush；
- 旧文件 cleanup。

故障注入 API 不允许测试直接篡改 production state machine 来制造“通过”。

## 17. 验收体系

M3 采用 M3-V1 至 M3-V7 七层门槛。任何一层失败都不能宣布 M3 GO。

### 17.1 M3-V1：格式与 codec

覆盖 manifest、journal、index 和 VHDX strict profile：

- 每种正常结构 round-trip；
- golden bytes 固定 wire layout；
- endian、alignment、reserved、version 和 checksum；
- 截断到每个 byte boundary；
- header、payload、offset、generation 和 filename 位翻转；
- 超大 count、整数溢出、循环索引、重叠区域和 path escape；
- fuzz 输入不 crash、不越界、不死循环、不超预算；
- unsupported VHDX 明确拒绝。

### 17.2 M3-V2：checkpoint/recovery 崩溃矩阵

测试建立一个不复用 production codec 的简单 reference model。对每个 durability boundary 和每个 kill point：

- manifest slot write 开始前的 kill 只能得到 generation G；
- slot write/flush 进行中的 kill 允许得到 G 或完整 G+1；
- manifest 提交点后只能得到完整 generation G+1；
- 注入的 slot write/flush 返回错误必须产生 `CommitIndeterminate`，并阻止后续 bundle mutation，直到 resolution 或重启 recovery；
- 不存在每块分别选择 G/G+1 的混合结果；
- orphan tail、完整但未发布的 commit 和临时文件被忽略；
- 最高 slot 损坏时正确回退前一完整 slot；
- 两个 slot 均无效时拒绝恢复；
- DATA、ZERO、DISCARD 和最后一个短 logical block 均覆盖；
- 不发生旧数据复活。

### 17.3 M3-V3：M2.1 generation 与并发

使用 deterministic gates 和随机模型测试：

- 多块相关元数据在同一切点；
- 扫描前、扫描中、扫描后的覆盖写；
- DATA→DATA、DATA→ZERO、ZERO→DATA、DISCARD→DATA；
- 同一块高频覆盖只保存一次必要 pre-image；
- copied batch 与 writer 并发时 payload 不变化；
- ack 与新写入竞争，新写入保持 dirty；
- COW budget 满、writer stall、snapshot abort 和恢复写入；
- close、cancel、deadline、allocation failure；
- token 误用、重复 finish、generation conflict；
- 结束后 pin、waiter、gate、COW bytes 和活动 token 全部归零。

### 17.4 M3-V4：compaction

- 压缩前后完整 logical disk hash 和逐 allocated block reference model 一致；
- sparse zero ranges 不被物理写满；
- 压缩期间持续 read/write/zero/unmap；
- temp VHDX 任意阶段失败不影响旧 bundle；
- rename、新 journal、commit 和 manifest 边界 kill matrix；
- 旧 manifest slot、旧 view 和在途 load 的文件保留；
- 两个 slot 都迁移后才允许清理旧文件；
- cleanup 失败不改变已提交结果。

### 17.5 M3-V5：Windows VHDX 兼容性

在 RAM Disk 已关闭、避免磁盘签名冲突的独立阶段，使用 Windows Virtual Disk API：

- `OpenVirtualDisk()` 成功；
- read-only、no-drive-letter attach 成功；
- geometry、sector size 和 virtual size 正确；
- 基础代数包含分区/NTFS 时，Windows 能识别并只读检查；
- 基础代数文件哈希与 reference dataset 一致；
- Windows 不报告 VHDX header、region、metadata、BAT 或 log 错误。

普通增量 checkpoint 的最新内容位于 journal，基础 VHDX 单独只代表 `base_generation`。Windows 独立兼容性验证比较的也必须是该 base generation，而不是错误地要求 VHDX 包含更新的 journal generation。

### 17.6 M3-V6：真实 AIM + NTFS 异常恢复

必须在管理员权限、真实 `phdskmnt.sys`、真实 M1/M2/M3 provider 和真实 NTFS 上执行：

1. 创建新 bundle 和同容量 RAM Disk；
2. 仅第一次初始化分区并格式化 NTFS；
3. 写入 deterministic 小文件、大文件、稀疏文件和随机文件；
4. 对卷执行 `FlushFileBuffers()`；
5. 执行 checkpoint 并保存 dataset manifest/hash；
6. 继续执行未 checkpoint 写入、rename、delete、TRIM 和 zero；
7. 在不同故障点 `TerminateProcess` provider，必要时执行受控重启场景；
8. 用同一 `.rdm` 恢复并重新创建 RAM Disk；
9. 禁止重新分区或格式化；
10. NTFS 必须正常挂载并完成 filesystem check；
11. 已 checkpoint dataset hash 必须完全一致；
12. checkpoint 后数据允许丢失，但不得导致混合代数或旧数据复活；
13. 证明 startup 没有预加载全部数据；
14. 对恢复后首次随机读取证明对应块才被加载；
15. 重复多轮 checkpoint、异常终止、恢复和 compaction。

mock、local fake、预构造 JSON 或人工说明不能替代 M3-V6。

### 17.7 M3-V7：资源、soak 与性能约束

正确性门槛：

- 长时间随机 read/write/zero/unmap/checkpoint/recovery 循环无数据模型偏差；
- 无 handle、thread、COW、cache、pin、waiter、temporary file 和旧 view 泄漏；
- 无永久 Loading/Committing block；
- journal/index/cache 均保持在配置硬上限内；
- close 和取消在规定 deadline 内收敛。

架构性能门槛：

- 打开镜像不得读取全部 virtual disk data；
- 未访问数据不得提交对应 M2 RAM；
- incremental checkpoint 的数据工作量与 dirty block 数相关，不与 logical capacity 线性相关；
- journal lookup 不得线性扫描 journal；
- compaction 与 allocated/nonzero extents 相关，不得无条件读取或写满全部 logical capacity；
- 100 GiB logical、1 GiB allocated 的验收镜像在 Windows 首次访问前，M3 启动读取不超过 64 MiB，M3 额外 committed memory 不超过 128 MiB；
- 256 MiB DATA dirty checkpoint 的 journal 增量物理写入不超过 payload 加 20% 格式/索引开销；
- 标准并发写验收负载下，一致性切点导致的 mutation admission pause 最大不超过 250 ms；COW 资源耗尽触发的单次 writer stall 不超过配置的 250 ms 默认上限。

验收必须记录吞吐量、P50/P95/P99 latency、checkpoint duration 和 compaction duration，但 M3 不设置“达到 Primo”或固定 NVMe 带宽的营销门槛。若相对门槛失败，必须定位算法或同步根因，不能只提高 timeout。

## 18. M3 交付分段

本 SPEC 后续 implementation plan 应按依赖顺序拆分，至少包含：

1. **M3.0 / M2.1：** consistent snapshot/COW epoch 及 M3-V3；
2. **M3.1：** checked I/O、CRC/SHA、manifest/journal/index codecs 及 M3-V1；
3. **M3.2：** strict VHDX reader/writer、Windows compatibility 及 M3-V5 基础部分；
4. **M3.3：** recovery、composite backing、lazy load 及损坏处理；
5. **M3.4：** checkpoint engine、reconciliation 和 fault matrix；
6. **M3.5：** compaction、rotation 和文件生命周期；
7. **M3.6：** CLI、evidence、真实 AIM+NTFS E2E、soak 和最终验收。

每一分段都必须先写失败测试或 reference model，再实现，再运行局部和完整回归。后续 plan 可以进一步拆任务，但不得改变提交点、generation、COW 和恢复不变量。

## 19. M3 GO 完成定义

只有同时满足以下条件才能标记 **M3 GO**：

- MSVC C++20 Debug 和 Release 构建成功；
- M0、M1、M2 现有回归全部通过；
- M3-V1 至 M3-V7 全部通过；
- 真实 AIM+NTFS 场景无需二次格式化即可多轮恢复；
- crash matrix 证明只恢复旧完整代或新完整代；
- VHDX 基础代通过 Windows 原生兼容性验证；
- 大镜像启动和首次读取证明按需加载；
- 所有测试产生 schema-valid、与当前 commit/binary 绑定的机器可读 evidence；
- format version、恢复规则、错误码和操作手册与实现一致；
- 禁止加载 AIM Toolkit 用户态模块的依赖审计继续通过；
- 代码和第三方依赖许可证记录完整。

真实管理员环境不可用时，状态只能是 `BLOCKED` 或明确列出未运行门槛的 `CONDITIONAL`，不能通过 mock 或文档豁免宣布 M3 GO。

## 20. 向 M4 的稳定交接面

M3 GO 后，M4 可以依赖以下稳定事实：

- `create/open/checkpoint/compact/inspect/verify/close` 有结构化 API；
- checkpoint 返回无歧义的 commit disposition；
- M4 可以在 checkpoint 前执行 Windows volume flush；
- 定时任务可以根据 dirty bytes、上次成功时间和 journal 大小做策略；
- `should_compact()` 不自行调度；
- immutable `BackingView` 支持正常 I/O 和后台持久化并发；
- 最后成功 checkpoint 可在进程或机器异常后恢复；
- M4 不需要解析 manifest、journal 或 VHDX；
- M4 不需要操作 AIM Toolkit 用户态组件。

M4 不得绕过 M3 直接改写 bundle 文件，也不得把 `CommitIndeterminate` 或 `CommittedNeedsReconciliation` 当作普通失败后立即启动另一个 checkpoint。

## 21. 已确定且不得在 implementation plan 中重新悬置的决策

1. 使用 VHDX + append-only journal + 双槽 manifest，不改回自定义单体镜像。
2. 普通写入只确认 RAM；成功 checkpoint 才承诺断电恢复。
3. manifest flush 成功是唯一“已知提交”点；publish attempt 中断或 flush 失败必须报告 `CommitIndeterminate`。
4. checkpoint 必须使用 M2.1 一致性切点，不能用现有逐块 `copy_dirty_batch()` 冒充整盘快照。
5. M2.1 采用首次覆盖前 COW，M3 v1 每盘一个活动 snapshot。
6. runtime lazy load 直接解析 VHDX，不把基础 VHDX 同时附加成第二块 Windows 磁盘。
7. M3 v1 仅支持产品生成的 strict VHDX profile。
8. journal index 使用 immutable COW sparse tree，startup 和 lookup 不扫描整个 journal。
9. checkpoint/compaction 串行，普通 I/O 继续。
10. compaction 从 immutable durable view 构建基础盘，并通过新的 checkpoint generation 发布。
11. 仍被任一有效 manifest slot 或 BackingView 引用的旧文件不得删除。
12. 损坏块不得静默变零或回退旧数据。
13. post-commit 问题进入 reconciliation，不能返回模糊的普通失败。
14. 定时写回、服务、GUI 和自动启动属于 M4。
15. M3 GO 必须包含真实 AIM+NTFS 异常恢复证据。
