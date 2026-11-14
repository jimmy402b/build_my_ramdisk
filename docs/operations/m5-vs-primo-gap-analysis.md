# MyRamDisk 与 Primo Ramdisk / AIM Toolkit 的能力对比

> 用途：作为外部评审（人或模型）的输入，用来判断"下一步该补什么"。
> 作者：基于对 `fancyrd.sys` / `FancyRd.exe` 的逆向结果（`Z:\nixiang\notes\fancyrd-analysis.md`）
> 与对 `build_my_ramdisk` 的只读代码勘察。
> 日期：2026-11-14

**两个对标物，性质不同，不要混用：**

| 对标物 | 关系 | 用来回答 |
|---|---|---|
| **Primo Ramdisk** | 商业闭源，自研内核驱动 `fancyrd.sys` | **功能标杆**：同类产品能做到多全 |
| **AIM Toolkit** | AGPL，与本文项目**共用 `phdskmnt` 驱动** | **架构同类**：同一条技术路线上别人做到哪一步 |
| ImDisk / ImDisk 3.0 | AGPL，`imdisk.sys`，**卷级**模拟（非盘级） | 上一代设计，仅作背景 |

一句话区分：**Primo 是"别人家的产品"，AIM Toolkit 是"同一个驱动上的另一个产品"。**
本项目的差异化只有一处——**断电后能证明数据回到哪一代**；其余功能面，AIM Toolkit 已经免费覆盖。


---

## 0. 证据边界（先读这一节，否则会误用本文）

**我实际看过的**（可直接引用）：

| 对象 | 看了什么 |
|---|---|
| `fancyrd.sys` | 全量反编译 + 45 个 IOCTL + 派发表 + 许可链 + 并发模型（另一份报告） |
| `FancyRd.exe` | 导入表、`DeviceIoControl` 的 18 个调用点、字符串常量、设备/注册表路径 |
| 本项目 | 目录结构、316 个源文件 / 104,788 行的**规模统计**、`aim_protocol.h` 全文、`AGENTS.md`、`INDEX.md`（43 行全读）、`m5-lessons.md`（138 行全读）、关键文件的**行数** |
| 运行现场 | 服务配置、事件日志、注册表值（只读） |

**我**没有**逐行读过的**（本文对它们的描述来自文件名、行数、文档转述，标注为 `[未验证]`）：

- `src/management/service_core.cpp`（9,111 行）
- `src/persistence/*`（journal / checkpoint / compaction / recovery）
- `src/storage/*`（dynamic block store / snapshot coordinator）
- `src/run/windows_volume.cpp`（1,663 行）
- 测试用例的实际断言内容

**因此**：本文里凡是写 `[未验证]` 的，请在采纳前用代码核对；凡是没有标注的，都有具体文件/行号/地址可追。

---

## 1. 一句话定位

| | Primo Ramdisk | My_ramdisk |
|---|---|---|
| 本质 | **内核态的虚拟 SCSI 端口驱动 + 镜像文件管理器** | **用户态的盘生命周期管理器 + AIM 端口驱动** |
| 谁实现块设备 | 自己的 `fancyrd.sys` | 第三方 `phdskmnt`（AGPL） |
| 谁保证数据不丢 | **不明确**（见 §4.3） | 自己的 journal + checkpoint |
| 目标用户 | 终端用户，装完即用 | `[未验证]` 看起来是"可运维的持久化内存盘" |

**关键判断**：这不是"同一件事的初级版和高级版"，而是**两条不同的技术路线**。Primo 把复杂度压进内核（换来"无外部依赖"，代价是签名/许可/蓝屏风险）；你们把复杂度放在用户态（换来可测试性，代价是依赖 AGPL 驱动）。

---

## 2. 架构对比

| 维度 | Primo Ramdisk | My_ramdisk | 差异性质 |
|---|---|---|---|
| 内核组件 | 自研 `fancyrd.sys`（约 200 KB，569 函数） | `phdskmnt`（Arsenal AIM，已签名） | **路线差异**，不是水平差异 |
| 签名负担 | 必须 WHQL（否则无法加载） | 无（用户态 EXE 不需要） | 你们占优 |
| 许可负担 | 自研 RSA + SMBIOS 绑定 | 无 | 你们占优 |
| 崩溃风险面 | 内核里任何 bug = 蓝屏 | 用户态进程崩溃可重启 | 你们占优 |
| 性能上限 | 内核态直接映射页帧 | 共享内存映射（`kProxyTypeShm`） | Primo 略优 `[未验证]` |
| 依赖风险 | 无 | **AGPL-3.0**（`aim_protocol.h` 自述） | **你们的最大风险** |
| 派发模型 | `MajorFunction[0..27]` → 跳板 → 双 bank 表（按 `DeviceExtension+0x40` 选） | 服务 IPC → operation → worker 进程 | 不可直接比较 |
| 并发 | CSQ 停泊 + KQUEUE 执行 + 每 CPU 线程 | 多进程 + job object + 命名管道 | 你们的边界更硬 |
| 状态持久化 | 注册表值（`Data2`/`Info`/`Scode`） | bundle 目录 + operation store + profile store | **你们强得多** |

---

## 3. 功能逐项对比

图例：✅ 有 · ⚠️ 部分/存疑 · ❌ 无 · `[未验证]` 需代码确认

| 功能 | Primo | My_ramdisk | 证据 |
|---|---|---|---|
| 内存盘（RAM disk） | ✅ | ✅ | 关键词 `ramdisk` 45 处 |
| 盘符分配/更改 | ✅ `0x04604C` | ✅ | `windows_volume.cpp` |
| 卷序列号可控 | ✅ `0x046074` | ✅ | 关键词 57 处 |
| 簇大小可控 | ✅ `0x046018` 回 `{12,2,32,512}` | ✅ | 关键词 49 处 |
| TRIM / UNMAP | ✅ `0x2D9404` | ✅ | 关键词 793 处 |
| 断电一致性（journal） | ❌ 未见 | ✅ **journal + 两阶段提交 + generation** | `src/persistence/*`、R-02 三次定点断电 |
| 在线整理（compaction） | ❌ 未见 | ✅ | `compaction_coordinator.cpp`、T4-03 |
| VHDX 导出 | ❌ | ✅ | `vhdx_writer/reader.cpp`、T4-02d |
| 单盘准入保护 | ❌ | ✅ `0x5609` | M5-STAB-01 |
| 内存超配拒绝 | ❌ | ✅ `0x560A` | M5-R01 |
| **镜像文件保存/加载（.vdf 类）** | ✅ | ❌ **0 处** | 关键词 `image_save/load` 零命中 |
| **压缩**（zlib 1.2.3 内嵌） | ✅ | ❌ **0 处** | 关键词 `zlib/lz4/zstd/deflate` 零命中 |
| **混合盘（.hbd：内存+文件）** | ✅ | ❌ **0 处** | 关键词 `hybrid` 零命中 |
| **文件盘（镜像文件作后端）** | ✅ `.fld` | ⚠️ **ABI 支持但未用** | 见 §5.1，重要 |
| **引导扇区生成（可启动盘）** | ✅ 内嵌 NTFS/BOOTMGR 引导码 | ❌ | fancyrd `0x14001C520` 起有引导错误串 |
| 目录联接/重解析点 | ✅ `Junctions` 键 | ⚠️ `[未验证]` 71 处命中，用途不明 | — |
| 临时文件重定向 | ✅ | ⚠️ `[未验证]` 37 处命中 | — |
| GUI | ✅ MFC | ✅ | `src/gui` |
| 命令行 | ❌ | ✅ | `src/cli`，`disk/export/import/reclaim/repair-profile` |
| 多盘并存 | ✅ | ⚠️ **G4 已延后** | `AGENTS.md` §2.1 |
| 热升级/回滚 | ⚠️ 重装 | ✅ 覆盖安装 + schema 版本 | R-08 |
| 可复现构建 | ❌ | ✅ `/Brepro`，两次构建 SHA 一致 | R-10 |
| 一键回归 | ❌ | ✅ 160 用例 | STAB-04 |
| 服务级诊断 | ⚠️ 注册表状态字 | ✅ `logs\service.log` + operation | R-08 |
| 开机自动拉起 | ✅ BOOT_START | ✅ 但踩过坑 | M5-AUTOSTART-104 |

---

## 4. 三个最值得注意的差距

### 4.1 缺"文件盘"——但只差一层开关（**最高性价比**）

`src/driver/aim_protocol.h` 里声明的 ABI **已经包含**文件后端所需的全部字段：

```cpp
constexpr std::uint32_t kTypeProxy      = 0x00000300u;
constexpr std::uint32_t kProxyTypeShm   = 0x00003000u;
constexpr std::uint32_t kOptionSparseFile = 0x00000004u;

struct DeviceConfiguration {      // pack(4)
    std::uint32_t DeviceNumber;
    std::int64_t  DiskSize;
    std::uint32_t BytesPerSector;
    std::uint32_t Reserved;
    std::int64_t  ImageOffset;    // ← 镜像内偏移
    std::uint32_t Flags;
    std::uint16_t FileNameLength;
    // WCHAR FileName[] follows     ← 文件名
};
```

但 M0 只用了共享内存那一支：

```cpp
constexpr std::uint32_t kM0CreateFlags =
    kDeviceTypeHd | kTypeProxy | kProxyTypeShm;   // 0x00003310
```

**含义**：AIM 的设备只有两条后端路径，不是四种模式：

| 后端 | Flags | 谁做 I/O |
|---|---|---|
| **文件后端** | `kDeviceTypeHd`（**不带** `kTypeProxy`）+ `FileName` / `ImageOffset` | `phdskmnt` **在内核里自己开文件** |
| 代理后端 | `kDeviceTypeHd \| kTypeProxy \| kProxyTypeShm` | 用户态进程经共享内存服务 I/O |

证据：本机 `phdskmnt.sys`（1.2.28.93）的导入表里有 `ZwCreateFile`、`ZwReadFile`、`ZwSetInformationFile`、`FileObject` —— 内核侧确实会直接打开镜像文件。所以"文件盘"不是缺失的能力，是**没被使用的另一条支路**。

→ "从现有镜像文件挂载一块盘"的工作量：**去掉 proxy flags、填 FileName/ImageOffset**，而不是重写持久化层。
→ **但必须先确认**（见 §8 Q2）：`ImageOffset` 在 1.2.28.93 上的实际语义、稀疏（`kOptionSparseFile`）行为、并发写/只读映射、镜像被外部修改时的处理。

#### 4.1.1 一个前置风险：装的驱动比头文件旧

| 项 | 值 |
|---|---|
| 本机 `phdskmnt.sys` | **1.2.28.93**（2025-10-20，65,280 B，SHA256 `3C23DF3D…9DF6F9`） |
| `THIRD_PARTY.md` 钉的上游 | **1.2.29.95**（commit `d379caa…`） |
| 代码里的版本闸门 | `adapter_discovery.cpp:127` `return_code != kDriverVersion`，`kDriverVersion = 0x0101` |

**问题**：`0x0101` 是 **API 版本常量**，不是构建号。1.2.28 和 1.2.29 都会返回 `0x0101`，所以这个"版本校验"**发现不了 1.2.28 与 1.2.29 的差异**——而差异是真实存在的（头文件按 1.2.29.95 声明）。

后果：如果 1.2.29 改了 `imdproxy` 的 wire 结构或 `DeviceConfiguration` 语义，你们会在**检查通过**的情况下用一个不匹配的 ABI。

→ 建议：把 `query_version()` 已经拿到的 `sub_version` 一起纳入判定和错误信息（`direct_aim_adapter.cpp:14-24` 已经解析出来了，只是发现流程没用）；错误消息里的实际值现在还是十进制冒充十六进制（见 `adapter_discovery.cpp:131/133` 的 `std::to_wstring`）。

### 4.2 缺压缩——和你们的 journal 设计**天然冲突**

Primo 在 `fancyrd.sys` 里内嵌了完整 zlib 1.2.3（`deflate` @ `0x14001EEF0`、`inflate` @ `0x1400203C0`），**而且确实用在镜像文件上**：

| 路径 | 调用链 | 证据 |
|---|---|---|
| 保存镜像 | `sub_140006F60` → deflate `sub_140011270` → `ZwWriteFile` | 反编译确认 |
| 加载镜像 | `sub_140007078` → `ZwReadFile` → inflate `sub_140011F58`（3,795 B，zlib 错误串 `0x140016810`–`0x1400169A0` 的引用者） | 反编译确认 |

即：**Primo 的镜像文件是压缩存储的**，你们是"未压缩块 + 追加 journal"。

**但"压缩效果更好"要拆成两件事**：

1. **稀疏 ≠ 压缩**。如果你们的 `dynamic_block_store` 已经跳过从未写过的块，那么"空盘"的镜像体积差距不大——那时赢的是稀疏。
2. **压缩收益取决于数据熵**。文本/日志/数据库/重复块收益巨大（zlib 常见 3–10×）；已加密或高熵数据几乎为零。所以"效果好得多"是否成立，取决于被测盘里装的是什么。

**你们做压缩的代价**：journal 是追加写 + 随机定位，压缩会破坏 O(1) 寻址；checkpoint 要原子，压缩后的 base 与 journal 边界要重新定义；compaction 要流式折叠，压缩块需要索引。

**可行的折中**（供评审）：只在 **checkpoint 的 base image** 上做块级压缩（4–64 KB 固定块 + 索引表），journal 保持未压缩。这样随机读仍然 O(1)，压缩率损失可控。

### 4.3 缺"引导扇区/可启动盘"

`fancyrd.sys` 的 `.rdata` 里有一整套引导扇区模板：

| 地址 | 字符串 |
|---|---|
| `0x14001C3A3` | `NTFS    `（OEM 字段） |
| `0x14001C520` | `\r\nA disk read error occurred` |
| `0x14001C53D` | `\r\nBOOTMGR is missing` |
| `0x14001C552` | `\r\nBOOTMGR is compressed` |
| `0x14001C56A` | `\r\nPress Ctrl+Alt+Del to restart\r\n` |

这说明它能**写 MBR/引导扇区**，造出可启动的盘。

你们没有。这条要谨慎：写引导扇区意味着直接操作裸扇区、要处理分区表对齐、要兼容 UEFI/GPT，**风险远高于收益**（除非有明确的"可启动内存盘"需求）。

### 4.4 关于"崩溃一致性"——你们**领先**，但对手可能没打算比

必须说清楚：我**没有**逆向 `fancyrd.sys` 的写路径与镜像持久化逻辑（那需要跟踪 `sub_140003A40`~`sub_140005xxx` 那一大片块设备代码）。所以"Primo 没有 journal"是**推断**，依据是：

- 它的 IOCTL 面里没有任何"generation / checkpoint / commit"语义
- 它的状态字 `Info` 只表达许可状态，不表达数据一致性
- 它的镜像格式是单文件（`.vdf`），没有配套的 journal 文件

如果这条推断成立，那**这是你们最大的差异化优势**，而且竞争对手短期补不上（要改数据格式）。

---

## 5. 差距的成因分类（给评审的关键输入）

这一节是本文的核心：**把"差距"拆成"能补"和"不能补"**。

| 差距 | 成因 | 可补性 | 量级 |
|---|---|---|---|
| 文件盘/镜像挂载 | **只差 flags**（§4.1） | 高 | 小 |
| 压缩 | 与 journal 耦合 | 中 | 中 |
| 混合盘 | 纯上层策略（phdskmnt 只提供块设备） | 中高 | 中 `[未验证]` |
| 引导扇区 | 需要裸扇区 + 分区表 | 低（风险高） | 大 |
| 多盘并存 | **用户已裁定延后**（G4） | 高（但会带回 11 s 阻塞问题） | 大 |
| 性能上限 | 内核态 vs 共享内存的**架构差** | 不可补（除非自研驱动） | — |
| 依赖 AGPL | 架构选择 | 可换（WinFsp / VHD API / 自研） | 大 |
| 签名 | 你们不需要 | — | — |

**两条不能靠写代码解决的**：
1. **性能上限**：共享内存代理 vs 内核直接映射，差一个数量级的内存拷贝 `[未验证具体数值]`。
2. **AGPL**：这是分发合规问题，不是技术问题。

---

## 6. 改进候选池（按"投入 / 收益"排序）

### P0（小投入、直接补功能缺口）

1. **文件盘支持**：扩 `kM0CreateFlags`，填 `FileName`/`ImageOffset`，加只读/可写两种模式。→ 补 `.vdf`/`.fld`。
2. **构建戳**：编译期注入 `git describe --dirty`，`--version` 打印，runner 启动记录。`m5-lessons.md` §1 里"陈旧二进制"咬了四次，其中一次让六个假设全错。

### P1（中投入、差异化）

3. **压缩 base image**（块级 + 索引，journal 不压缩）。
4. **状态外置**：在一个稳定的注册表位置写 `Status`/`LastError`，状态机迁移时更新。参考 Primo 的 `Info` 字（`0x40000000`/`0x50000000`/`0x80000000`），但用途是**开机窗口排障**，不是授权。
5. **降级面显式化**：把"blocked 时哪些命令仍可用"写成一页矩阵 + 对应测试。

### P2（大投入、需要决策）

6. **拆分 `service_core.cpp`**（9,111 行）——纯工程债。
7. **AGPL 合规路径**：开源 / 买商业许可 / 换驱动。**这是唯一可能阻断分发的项。**
8. **多盘（G4）恢复**：需要先解决"设备退役窗口 11 s 阻塞"（`m5-lessons.md` 与 `AGENTS.md` §2.1 已记录）。

### 不建议做

9. **自研内核驱动**：为了"1:1"去拿 Partner Center 签名（个人开发者基本拿不到）+ 承担蓝屏面，收益不成比例。
10. **引导扇区生成**：风险 >> 收益，除非有明确需求。

---

## 7. 不要照抄的部分

| Primo 的做法 | 为什么不要抄 |
|---|---|
| 把授权判断放内核（RSA + SMBIOS） | 它没得选（本来就在内核）。你们放 **service**（SYSTEM + 受保护目录）成本更低、效果接近 |
| `DriverEntry` 永远返回成功、失败装桩表 | 对驱动是合理的（不能蓝屏），对服务不适用——服务启动失败应该明确失败 |
| 用 `eventlog\System\RxDeliveryStamp` 伪装注册表路径 | 隐蔽性靠"藏在系统键下"，这是对抗思路，不是工程思路 |
| 1 个文件塞下所有状态 | 你们的 bundle 目录 + generation 更好调试 |
| `Info` 状态字当唯一诊断 | 你们有 operation store + trace，信息量大得多 |

---

## 8. 给评审的提问清单（可直接粘贴给 GPT）

> 背景：我们有一个用户态的持久化内存盘产品（Windows 11 x64），块设备由第三方已签名驱动 `phdskmnt` 提供（AGPL-3.0），我们自己做生命周期管理、journal + checkpoint + 在线 compaction + VHDX 导出。竞品 Primo Ramdisk 是纯内核驱动，功能更全但没有崩溃一致性设计。
>
> 现状：316 个源文件 / 约 10.5 万行；4 个二进制（service / cli / gui / worker）；160 个测试；已发布 v1.0.4。
>
> **Q1** 我们的 AIM ABI 已经声明了 `DeviceConfiguration.ImageOffset` / `FileNameLength` / `kOptionSparseFile`，但创建时只传 `kProxyTypeShm`。如果只想加"从镜像文件挂载"能力，最小改动面是什么？有哪些语义陷阱（稀疏、并发写、只读映射、镜像被外部修改）？
>
> **Q2** 在"journal 追加写 + 随机读 O(1)"的约束下，给 base image 加块级压缩（4–64 KB 固定块 + 索引）会在哪些地方破坏现有不变式？checkpoint 原子性、compaction 流式折叠、断电恢复各需要改什么？
>
> **Q3** 我们的 `service_core.cpp` 有 9,111 行。在不改变行为的前提下，按什么边界切分最不容易引入回归？（提示：现有边界是"准入/生命周期/恢复/查询"，但耦合点未知）
>
> **Q4** 竞品的"混合盘"（一部分内存 + 一部分文件，按热度迁移）在只有块设备接口、没有内核配合的前提下，能做到什么程度？缺页/迁移策略放在用户态会不会引入不可接受的延迟抖动？
>
> **Q5** 如果 AGPL 成为分发障碍，在"保留现有 journal/checkpoint 设计"的前提下，换成系统自带 VHD API（`virtdisk.dll`）或 WinFsp，各会失去什么能力？有没有第三条路？
>
> **Q6** 我们的测试已经在崩溃一致性上投了很多（三次定点断电、bundle 损坏矩阵、卷满、ACL 破坏）。在没有竞品对照的情况下，怎么判断"这块已经足够好"？有没有公认的验收基线（例如 crash-consistency benchmark）？

---

## 9. 附录：证据索引

### 9.1 Primo Ramdisk 侧（地址均可在 `fancyrd-analysis.md` 或 IDA 中复现）

| 结论 | 地址 |
|---|---|
| 派发表双 bank、按 `DeviceExtension+0x40` 选择 | `sub_140005E80` |
| 装表 | `0x140030510` |
| 降级表（28 项全桩） | `0x1400302B4` |
| 45 个 IOCTL | `sub_140025848` / `sub_140008360` / `sub_1400088CC` / `sub_140008AEC` / `sub_14000D550` |
| zlib deflate / inflate | `0x14001EEF0` / `0x1400203C0` |
| 引导扇区模板 | `0x14001C520` 起 |
| 许可链 | `0x140017C5C → 0x140017504 → 0x1400179FC` |
| 机器绑定 | `Data2 == ~SMBIOS_UUID`（现场实测） |

### 9.2 本项目侧

| 结论 | 文件 / 证据 |
|---|---|
| AIM ABI 与 proxy flags | `src/driver/aim_protocol.h:52-95`、`:144-199` |
| 设备发现与版本校验 | `src/driver/adapter_discovery.cpp:117-135` |
| 已知付费学费 | `docs/operations/m5-lessons.md`（138 行） |
| 当前状态与版本 | `INDEX.md`（v1.0.4，2026-11-13） |
| 范围裁定（G4 延后） | `AGENTS.md` §2.1 |
| 测试环境边界 | `AGENTS.md` §4 |
| 关键文件规模 | `service_core.cpp` 9,111 行；`m2_report.cpp` 2,724；`m5-handoff.md` 409.7 KB |

### 9.3 关键词扫描结果（`src/**/*.cpp|h`，2026-11-14）

| 关键词 | 命中数 |
|---|---|
| `vhdx` | 941 |
| `trim/unmap/discard` | 793 |
| `junction/symlink/reparse` | 71 |
| `volume_serial/serial_number` | 57 |
| `cluster_size/bytes_per_cluster` | 49 |
| `ramdisk/ram_disk/memory_backed` | 45 |
| `temp_file/tempfile` | 37 |
| `zlib/lz4/zstd/deflate/compress` | **0** |
| `hybrid` | **0** |
| `image_save/image_load` | **0** |

---

## 10. 一句话总结

**你们不缺"能不能做"，缺的是"要不要做"。**

功能面上，文件盘只差一层 flags，压缩是中等工程，混合盘是策略层；真正的硬差距只有两个——**内核态性能上限**和**AGPL 分发合规**。前者决定了产品的天花板，后者决定了产品能不能发出去。

而你们领先的那块（journal + checkpoint + compaction + 崩溃一致性验证），恰恰是竞品**结构上补不上**的。
