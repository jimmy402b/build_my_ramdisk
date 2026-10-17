# 已解决：create 路径定位到一块非空的盘

> **状态：已解决（2026-10-17，part 95）。**
> 根因与六个被排除的假设**都不是一回事**，所以下面那份排查记录保留原样 ——
> 它的价值在于说明"为什么六个看起来最像的方向全是错的"。
> **根因见 §0，修复见 §0.2。** 下面的 §1–§8 是当时的排查记录。

---

## 0. 根因（一句话）

**失败的 create 走的是"采纳"路径，而它采纳了另一个 profile 的设备。**

**这不是"定位错盘"，而是"从一开始手上拿的就是别人的设备"。**

### 0.1 决定性读数

在 `create_generation_zero` 入口加打点，与 create 的其它打点对齐：

```
ADOPTING the existing device              ← 第一次（attach）
create_generation_zero: entered
create_generation_zero: wrote generation zero
create proxy disk                          ← 六次正常 create
（× 6）
ADOPTING the existing device              ← ★ 失败的那次：采纳，不是创建
disk #1 is not empty; retrying the locate
```

**根因代码**（`m3_windows_acceptance.cpp`）：

```cpp
const bool identity_ok =
    queried.value().id == *owned_device_ &&
    queried.value().size_bytes == geometry.virtual_bytes &&
    queried.value().sector_size == geometry.logical_sector_size &&
    (adopting_existing ||                                  // ← 采纳时这个条件被跳过
     queried.value().native_object_path == names.driver_path);
```

**采纳时只比"大小 + 扇区大小"，不比身份。**

代码里给出的理由是：*本运行时持有 bundle 的独占写锁，所以同尺寸的设备就是本 bundle 的。*
**这条对"服务本 bundle 的设备"成立，对"服务别的 profile 的 bundle 的设备"什么也没说**
——而那正是出事的场景：**两个 profile 的盘都是 64 MiB，bundle 锁排除不了另一个 bundle 的持有者。**

**这一条解释了此前全部读数的每一个细节**：`baseline` 为空（采纳路径跳过 baseline）、
驱动里 1 个设备（A 的）、只有一块 64 MiB 盘（A 的）、它的头是 MBR（A 已格式化）、
前六次正常（那时台面上没有别人的设备）。

### 0.2 修复

**create 路径不再采纳。** 创建运行时的职责就是把新设备造出来；
属于本 bundle 的残留是 sweep 与 reclaim 的事。
**attach-only 路径仍然采纳**——找到"已经在服务本 bundle 的那个设备"正是
`recover` 与 `reclaim` 必须做的事。

**读数**：`-Repeat 3` → **3 / 3 全绿，一条 FAIL 都没有**。

### 0.3 从这次排查里能带走的

1. **"这次操作到底走了哪条分支"是最便宜、也最常被跳过的一问。**
   六个假设全在追问"环境里有什么"，而答案是"代码走了另一条路"。
2. **一个"因为 X 所以 Y 安全"的推理，必须问 X 是否覆盖了所有情况。**
   这里 X（bundle 锁）只覆盖"本 bundle 的并发持有者"，
   而威胁来自"另一个 bundle 的持有者"——**两者是不同的集合**。
3. **错误信息指名对象**这条纪律直接换来了这次突破的前置条件（§4.15.61）。

---

## 附：当时的排查记录（保留，说明六个方向为什么全错）

**状态**：未解决，**不阻塞其他增量**。

**影响**：`m5.production_execution_backend` 第 17 条（`stopping_one_profile_does_not_require_the_other_gone`）
在建卷时失败，因此整套件 16/18，**T1-03 无法收尾**。

**这份文档存在的理由**：这条缺陷已经排除了六个假设。**没有这份汇总，接手的人会重走一遍。**

---

## 1. 现象（一句话）

**create 路径在建卷前定位到的盘，其头部不是零，而它应该是一块刚创建的、SHM 清零的盘。**

精确的失败形态（**已自述清楚**，这是本轮改进的结果）：

```
create failed: op=M3AimRuntime::start
  msg=located a disk that is not empty, so it is not the disk this runtime just created
```

**在此之前的形态**是一条几乎无法诊断的消息：

```
provisioning helper stderr: disk is not RAW
create failed: op=provision_new_volume msg=provisioning helper failed native=1 code=21001
```

---

## 2. 决定性读数（这是全部工作的基石）

在每次 create **定位到盘之后、provisioning 之前**读该盘前 1 KiB
（overlapped + 5 s 有界等待）。一次运行的七次 create：

```
disk #1 head=0000000000000000 nonzero=no  read=1024   ← 前六次
disk #1 head=0000000000000000 nonzero=no  read=1024
disk #1 head=0000000000000000 nonzero=no  read=1024
disk #1 head=0000000000000000 nonzero=no  read=1024
disk #1 head=0000000000000000 nonzero=no  read=1024
disk #1 head=0000000000000000 nonzero=no  read=1024
disk #1 head=33c08ed0bc007c8e nonzero=yes read=1024   ← 第七次：MBR
```

**同一个盘号 `#1`，前六次读出纯零，第七次读出真实的 Windows MBR 引导码**
（`33 c0 8e d0 bc 00 7c 8e`），第二扇区是 `EFI PART`。

**⇒ 这条读数的意义**：它**否掉了"存储总是返回陈旧数据"**这个说法——
前六次它老老实实返回零。**差异只在第七次，而第七次正好是失败的那次。**

---

## 3. 已经排除的六个假设（每个都有读数）

| # | 假设 | 排除它的读数 | 位置 |
|---|---|---|---|
| 1 | 传给脚本的**盘号是错的**（比如落到了系统盘） | `#1`、64 MiB、`boot=False`、`sys=False`；系统盘是 `#0`、64 GiB | §4.15.61 |
| 2 | **SHM 映射太小**（8 MiB）撑不住 64 MiB 盘 | `kDataAreaSize` 是**协议单次载荷上限**，不是盘容量；盘内容由本项目的 `request_processor` 供 | §4.15.62 |
| 3 | **旧设备被 `kAutoDeviceNumber` 复用** | create 之前 `list_devices()` **每次都是 0 个设备**（六次） | §4.15.64 |
| 4 | **定位错盘**（两个同尺寸盘导致歧义） | baseline **每次都是 `[0]`**，创建前只有系统盘；`#1` 必是新建的 | §4.15.68 |
| 5 | **清盘再建**可以绕过 | `Clear-Disk` 版本读数更差：12 / 16 / 2，且 258 超时复现 ⇒ 已回退 | §4.15.65 |
| 6 | **拆卸失败留下盘符**，下一轮 create 命中它 | 静止时 `workers: 0`、`disks: #0`、`volumes with letters: C:` ⇒ 无残留盘符、无残留盘 | §4.15.73 |

**补充排除**：`bundle` 不是复用的——`TempBundle` 用**纳秒时间戳**命名（代码核实）。
**重启客机不改变结果**，而静止时没有任何残留 ⇒ **没有东西可被重启清掉**，
这也与假设 6 的排除互相印证。

---

## 4. 已经确立的事实（不需要再验）

- **设备是新建的**：create 前驱动里 0 个设备。
- **创建前只有系统盘**：baseline 恒为 `[0]`。
- **bundle 是新的**：纳秒时间戳目录。
- **SHM 创建时清零**：`shared_memory_channel.cpp` 里 `memset(view, 0, kMappingSize)`，
  且映射已存在时会拒绝（`ERROR_ALREADY_EXISTS`）。
- **重试无效**：非零后等 500 ms 重新定位，**仍然是那块非空的盘** ⇒ 它是**稳定的**，不是"还没稳定"。
- **静止时环境是干净的**：无 worker、无残留盘、无残留盘符。

---

## 5. 已经装上的东西（无论缺陷是否解决都应当保留）

**create 路径现在校验"这块盘是空的"**：定位后读前 1 KiB，要求全零；
非零则重试一次定位，仍非零则**明确失败，不做任何写操作**。

**为什么保留它**：在没有这道检查之前，代码会**直接对那块盘做
`Initialize-Disk` + `Format-Volume`**。
**"绝不 format 一块无法证明属于自己的盘"从一句原则变成了可执行的检查**——
**这是独立于本缺陷的安全改进**，不该因为测试还红着就被回退。

**代价**：create 路径两次有界 1 KiB 读（overlapped + 5 s），不在 attach-only 路径上。

---

## 6. 下一步该测什么（这是接手者唯一需要读的一节）

**先读这一条 —— 分叉已经在 part 94 被走掉了**：

在失败瞬间同时抓"驱动视图"和"Windows 盘视图"，读数：

```
at failure the driver holds 1 device(s)
at failure 1 disk(s) of size 67108864 exist: [#1(len=67108864,bus=0,tgt=0,lun=0)]
```

**⇒ 失败瞬间只有一块属于我们的 64 MiB 盘，驱动里也只有一个设备（就是我们的）。**

**⇒ 按本来的判据，这走的是"盘就是我们的"那一支。**
所以**"残留盘"这个方向可以彻底放弃**——不是残留，**就是我们自己的盘，而它不空**。

**且 M2 的生产路径不使用命名共享内存**
（`CreateFileMapping`/`MapViewOfFile` 在 `src/m2/` 下无命中；仅 `m2_local_fake` 与测试用了
`Global\myrd-m2-*` 这样的名字）。**因此不存在"两个 bundle 共享同一块映射"这种事。**

**⇒ 问题被压到最后一层：我们自己的 store 对一块刚创建的盘返回了非零内容。**
**而前六次它返回零。** 差别在**第七次的那次 create 本身**，不在机器状态。

**建议按这个顺序做，每一步都能独立证伪：**

### 6.1 抓"第七次 create 与第六次"的差异

现有打点能证明"前六次零、第七次 MBR"，**但没有对比这两次 create 的输入**。需要记录：

- 每个 create 的 **bundle 路径**与**它是否被判定为"已存在"**
  （`classify_bundle` 的答案，以及 `create_generation_zero` 是否真的被调用）；
- 每个 create 的 **generation / manifest 选择结果**；
- 第七次读到的 MBR **是否与前一次（第六次）provisioning 写下的内容一致**。
  若一致，那么"第七次的盘承载了第六次的数据"就是事实，
  而两者唯一的联系就是 **store 的读取回答**。

**判据**：若 `create_generation_zero` 在第七次被**跳过**（例如 bundle 被判为已存在，
或模式被当成 recover），那就是它；这也能解释为什么前六次正常。

### 6.2 如果 `create_generation_zero` 确实被调用：查"未提交块是否按零返回"

直接对 M3 存储问第一块（不经驱动、不经代理），
看它返回的是零还是那份 MBR。
**这一步能一次把"store 有错"与"代理层有错"分开**——目前这两者还没被分开过。

### 6.3 顺带要确认的一件事

`m5.production_execution_backend` 的**第 17 条之前的某一条**是否真的完成了拆卸。
证据里反复出现 `myrd-m5-worker: the worker's resources could not be released (op=flush ...)`，
**它属于哪一条用例、是失败原因还是失败结果，尚未确认**。
在失败瞬间抓 worker 的存亡状态可以一次回答。

---

## 7. 不要重复做的事


- **不要**再怀疑盘号错、映射太小、旧设备复用、定位错盘、清盘、残留盘符——**六个都验过了**。
- **不要**用 `Clear-Disk` 绕过：读数更差，且会把 258 超时带回来（§4.15.65）。
- **不要**只加界了事：`enumerate_disk_candidates` 的界早已存在，
  **它不是这条缺陷的答案**（加界之后失败形态只是换了个样子）。
- **不要**相信"重启客机就好了"：重启前后结果一致，静息态也确实干净。

---

## 8. 测量纪律（踩过的坑）

- 改 `src/run/provision_volume.ps1` 之后、上客机之前，**必须先在开发机跑
  `unit.m4.provision_script`**。它是那个脚本的唯一守卫（AST 白名单，
  只允许固定的存储命令集），**`ForEach-Object` 与 `New-Object` 都不在白名单里**。
  **已因漏跑而红过两次。**
- 该脚本通过 `src/run/run_resources.rc` 以 `RCDATA` 嵌入，**改 `.ps1` 不会触发重编**，
  必须 `touch` 那个 `.rc`；改完先确认新串真的进了 `myrd-m5-worker.exe` 再上客机。
- **读盘要等完成**：`FILE_FLAG_OVERLAPPED` 之后必须 `WaitForSingleObject` +
  `GetOverlappedResult`，否则读到的是缓冲区里的任意内容——**比没有读数更糟**。
  这个错我在写探针时犯过一次。
- 失败时的错误信息**必须指名被检查的对象**（盘号、样式、大小、字节）。
  `disk is not RAW` 这句话本身无法区分"盘号错了"和"盘真的有旧分区表"，
  而两者的修法相反——**这条教训直接换来了 §4.15.61 的读数**。
