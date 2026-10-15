# M5 间发失败调查：两个已定量的缺陷

日期：2026-10-14
基线：`feature/m5-lifecycle` @ `9e66cdf`
环境：客机 `myrd-vm`（`192.168.180.129`），计划任务提权执行，见 `docs/operations/m5-vm-harness.md`
方法：`tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter <name> -Repeat 10`（重复在客机内做，一次往返）

---

## 0. 结论摘要

| 用例 | 复现率 | 性质 |
|---|---|---|
| `m5.production_execution_backend` | **5 / 20 = 25%** | **根因已定位**（stop 路径缺写冻结） |
| `unit.m2.backing_generation` | **14 / 20 = 70%** | 硬崩溃 `0xC0000409`，根因**未**定位；已排除 ABI/陈旧对象 |

两个都不是环境问题，也都不"偶发"到一个可以忽略的程度。

---

## 1. `m5.production_execution_backend`：stop 路径在提交与拆卸之间没有写冻结

### 1.1 现象

失败固定发生在第 10 个用例 `m5_backend_stop_walks_quiesce_flush_commit_remove`，
断言在 `tests/integration/m5_production_execution_backend_tests.cpp:661`（`require_action` 助手）：

```
myrd-test: stop B failed: op=service_worker_control.remove_mount
          msg=The worker refused to remove: its runtime stands dirty native=0 code=23559
```

`native=0 code=23559` 与 transfer manifest §1.39 记录的签名**逐字相符**。

### 1.2 机制（逐段可查）

1. worker 在拆卸前调自己的 `observe()`，`dirty` 的判定是
   `m5_worker_runtime.cpp:290`：
   ```cpp
   status.dirty = stats.dirty_bytes != 0;
   ```
2. `dirty_bytes` 的来源是 `runtime_adapter.cpp:472`：
   ```cpp
   return {(stats.resident_dirty_blocks + stats.zero_dirty_blocks) * kBlockBytes, 0, stats.backing_generation};
   ```
   —— **这正是设计不变量 #6 要求的 M3 块级 dirty 真相**。所以**谓词没错**，
   报的是真实的脏状态。
3. 若 `dirty`，worker 返回**拒绝回复**而不是错误（`m5_worker_runtime.cpp:486-489`），
   适配器把它转述成 `removal_refused`（`service_worker_control.cpp:763-772`）。
4. 而 stop 的计划顺序（`state_machine.cpp`）是：
   ```
   quiesce_mutations → flush_exact_volume → checkpoint_if_required → remove_mount → …
   ```
5. **`quiesce_mutations` 并不冻结写。** 它的实现自己写着
   （`service_worker_control.cpp:492-495`）：
   > Strict: "could not observe a live runtime" must not read as clean. **This
   > adapter holds no write freeze to take**, so quiescing is a proof and not a
   > mutation …

**所以：checkpoint 提交完成之后、`remove_mount` 检查 dirty 之前，仍然挂载着的 NTFS
卷还可以继续写。** 只要在这段窗口里有写落到卷上，`dirty` 就重新为真，拆卸被拒绝。
这解释了全部观察到的特征：**间歇（时序相关）、专属 stop 路径、失败点固定在 remove_mount**。

### 1.3 为什么 M4 前台不会撞上

M4 的干净关机顺序（`src/run/run_loop.h` 的 `ShutdownHooks`）是：

```
lock_volume → verify_volume → flush_volume → final_checkpoint → dismount_volume
→ remove_drive_letter → stop_runtime
```

**先取 `FSCTL_LOCK_VOLUME` 独占锁，再做最终 checkpoint。** 卷被锁住时文件系统写不进去，
所以 checkpoint 就是最后一句话，不存在"提交后又被写脏"的窗口。

M5 的 stop 路径缺的正是这一步。

### 1.4 修法（需要裁决）

| 方案 | 说明 | 评价 |
|---|---|---|
| **(a) 补上写冻结** | 让 worker 在 `quiesce_mutations` 阶段取卷独占锁（复用 M4 已有的 `WindowsVolumeAdapter::lock` / `LockedVolume`），顺序改成"锁卷 → flush → checkpoint → 拆卸" | **推荐**：按构造消除竞态，复用已验收的 M4 机制，不引入重试 |
| (b) 有界重试 | `remove_mount` 因 dirty 被拒时，重新 flush+checkpoint 后再试，有界次数 | 治症状；且本项目一贯避免重试循环 |
| (c) 放宽判定 | 改成只比较 generation | **拒绝**：那正是"把未提交的写丢掉"这条拒绝要防的事 |

---

## 2. `unit.m2.backing_generation`：70% 的硬崩溃，根因未定位

### 2.1 现象（已由崩溃转储定性）

- 退出码 `-1073740791` = `0xC0000409`；
- **stdout 与 stderr 都是空的**（捕获文件只有 `Set-Content` 写出的 BOM+换行 = 5 字节）；
- 崩溃率的另一次表现为 **TIMEOUT**（10 次里有 1 次）——WER 在崩溃时挂住进程所致，
  已通过 `DontShowUI=1` 消除；
- 通过的那次 stdout 有 12 行（`11/11 passed`）。

### 2.1b 转储给出的定性（2026-10-14）

客机与宿主机都没有调试器（`Windows Kits` 下没有 `Debuggers` 目录），所以用
`tools` 之外的临时脚本 `read-minidump.py` 直接解析 minidump 的异常流——
不需要符号，只需要那个失败原因码。结果：

```
exception code  : 0xC0000409
param[0]        : 7
VERDICT         : __fastfail reason 7 -> FATAL_APP_EXIT (CRT abort / std::terminate)
faulting module : C:\Windows\System32\ucrtbase.dll   (RVA 0xA527E)
```

**关键结论：这不是 stack cookie 越界，也不是堆元数据损坏，而是 `abort()`。**

- MSVC 的 `std::terminate` **在 abort 之前不输出任何东西**，所以 stdout/stderr 为空
  是**必然的**，不是"输出丢失"；
- 最常见的成因是**异常从线程逃逸**（`std::thread` 里逃出的异常不在 `main` 的
  try/catch 覆盖范围内）。而本文件恰好有两个用线程的用例；
- 竞态决定线程是否抛异常 → 解释间歇性，以及开发机 0% / 虚拟机 ~50% 的差异。

> 这条同时证明：**"空输出"不等于"没有失败信息"，也不等于抓不到**——
> 换个数据源（转储里的失败原因码）就够了。

### 2.2 为什么"空输出"不能读作"没有信息"

`tests/support/test_main.cpp` 用 `std::printf("PASS …")` 且**没有 fflush**；当 stdout 是管道时
是**全缓冲**。而 `test_framework.h` 的 `REQUIRE` 走 `std::cerr`（无缓冲）再 `std::_Exit(1)`。

推论两条，都可以从证据读出：

- **不是断言失败**：断言失败会经 `cerr` 打印 FAIL 并 `_Exit(1)`（退出码 1），
  而这里 stderr 为空且退出码是 `0xC0000409`；
- **stdout 的 PASS 行可能是在硬退出时丢掉的**（§1.31 记过同样的现象），
  所以无法从"没有 PASS 行"推断崩溃发生在第一个用例之前。

### 2.3 已排除的假设：ABI / 陈旧对象

§7 item 17 记录过 `0xC0000409` 的一个成因是**头文件改动后 `.obj` 陈旧导致的 ABI 错配**。
因为该用例测的是 **M2** 代码（此前只干净重编过 M5 的 6 个目标），所以先做了这个检验：

- 全量干净重编：`cmake --build out/build/msvc --config Release --clean-first` → **403/403，无 error 无 warning**；
- 重新 Stage 后复测 10 次 → **仍然是 7/10 崩溃**。

**复现率完全不变，ABI 假设被否定。** 这是真实缺陷。

### 2.4 下一步（取证路径）

要拿到故障栈，需要**崩溃转储**。建议按顺序做：

1. 在客机上配置 `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps`
   （对 `myrd_m2_backing_generation_tests.exe` 落 dump），并关掉 WER 的对话框；
2. 复跑直到崩溃，取回 dump；
3. 用 `cdb`/WinDbg 打开，看故障栈与 stack cookie 被破坏的位置。

若暂时不做，也可以：把 `test_main.cpp` 的 stdout 设为行缓冲
（`setvbuf(stdout, nullptr, _IOLBF, 0)`），这样至少能区分"崩在第一个用例之前"还是
"崩在全部通过之后"——但那是测试基础设施的改动，且只能缩小范围、给不出栈。

---

## 2.6 根因与修复（已实证）

**根因：一个测试专用钩子在冒充生产不变量。**

链条如下，每一环都有证据：

1. **追踪插桩**（`test_main.cpp` 的 `MYRD_TEST_TRACE`）把范围钉到单个用例：两次崩溃都在
   第 6 个用例 `m2_backing_install_wait_is_interrupted_by_close`；通过的那次 11 个用例全跑完。
2. **转储**给出失败类别：`__fastfail` reason 7 = `abort()`（**不是** stack cookie）。
3. **用例代码**：它需要 installer 停在"等待 backing publication"上，于是调用
   `DynamicBlockStoreTestAccess::begin_backing_publication(*store)`——**这个钩子直接自增
   生产的 `backing_publications_in_progress_`，且不进入请求**。
4. **产品代码**：`finish_close()`（`dynamic_block_store_core.cpp:445`）断言
   `backing_publications_in_progress_ == 0`，而 `finish_close()` 只在
   `active_requests_ == 0` 之后到达。生产路径里每一次自增都在请求作用域内
   （`read`/`write`/`ranges` 都在入口 `try_enter_request` 并用 `ScopeExit` 持有），
   **所以生产路径到不了那个状态——只有这个测试钩子能。**
5. **竞态**：测试在观察到 `install_finished` 之后才释放 publication，而 `close()` 的
   `finish_close()` 同时往下走。谁先到谁赢：赢了就过，输了就 `std::terminate()`。

**所以这是测试编排的竞态，不是 M2 产品缺陷。**

**为什么测试侧没有解**：释放 publication 会 `notify_all` 唤醒 installer，它会看到谓词成立
而**成功安装**——测试就失去意义了。所以修复必须落在钩子上。

**修法**：给测试专用的 publication 一个**自己的**计数器，不再冒充生产不变量；只有"必须
观察到 publication 被持有"的那一个谓词（install 等待）同时看两者。

```cpp
// dynamic_block_store.h
std::uint64_t backing_publications_in_progress_{};      // 生产：不变量的依据
std::uint64_t backing_publications_held_by_test_{};     // 测试专用：仅被等待谓词观察
```

`finish_close()` 的断言**只看生产计数器**，它的含义因此被保留（而不是被削弱）；
生产代码永不触碰新字段，所以**生产行为零变化**。

**修复前后（同一台客机、同一份用例、各 20 次）**：

| | 结果 |
|---|---|
| 修复前（累计 40 次） | **约 20 次崩溃**（`0xC0000409`） |
| **修复后** | **20 / 20 通过，零崩溃** |

## 2.7 附带发现：头文件改动必须全量干净重编

修完之后我先做了一次**增量**构建，结果 20/20 **全失败**，退出码变成 `0xC0000005`
（访问违例）且 stdout/stderr 全空——典型的 **ABI 错配**：给 `DynamicBlockStore` 加成员
改变了类布局，而增量构建没有重编所有包含该头文件的 TU。

`--clean-first` 全量重编（403/403）之后，**同一份代码** 20/20 通过。

> **规则：本仓库任何头文件改动之后，测量之前必须 `--clean-first`。**
> 这与 transfer manifest §7 item 17 记录的是同一个陷阱；本次是它的第二次现身。
> 反过来说，这也是我先前能正确排除"ABI 导致 M2 崩溃"的原因——那次检验本身就是全量重编。

---

## 2.8 为什么前四层"都通过了"——完整答案

这个问题有确切的、可查的答案，**不是"运气"也不是"记录不实"**，而是四件事叠在一起：

### (1) 这个测试的依赖链 M5 完全没碰过

```
tests/unit/dynamic_block_store_backing_generation_tests.cpp   [未改]
tests/support/（test_framework.h、fake_backing_store.h）      [未改]
src/storage/（dynamic_block_store.h、_internal.h）             [未改]
```

该测试只 include 这三处。所以它与 M4 基线是**同一份源码构建的**，崩溃不是 M5 引入的。

### (2) 它在开发机上不崩，在虚拟机上崩

| 环境 | 结果 |
|---|---|
| **开发机**（Release，20 次） | **0 / 20 崩溃** |
| **虚拟机**（Release，累计 40 次） | **约 20 / 40 崩溃（~50%）** |
| 虚拟机换工作目录 A/B | 4/10 与 2/10 —— **都与启动上下文无关** |

### (3) 前四层的证据都取自开发机

- M0–M3 的验收全部在**开发机**上执行。而缺陷在开发机上是**潜伏的**——
  那些绿是真实的测量结果，只是测在了不显现它的平台上。
- 这个测试的用例名全是**时序/生命周期**类
  （`retired_view_lives_until_last_shared_reference_exits`、
  `install_wait_is_interrupted_by_close`）。同一份代码在两台机器上表现不同，
  最合理的解释是**竞态**：开发机更快/更慢的调度改变了交错顺序，
  从而改变了越界写是否命中 stack cookie。

### (4) M4 的发布门**明确排除**了这批测试

`docs/validation/m4-vm-execution-report.md:50-52` 原文：

> The complete historical CTest suite is not a release gate for this branch; it
> contains **unrelated legacy M2/M3 administrator/shared-memory tests that fail
> in this host environment**.

M4 的虚拟机战役只跑了**聚焦的 9 个** M4 相关测试（`9/9 passed`）。
所以这个 M2 测试**从未在虚拟机上被执行过**。

### 结论

> **前四层能通过，是因为从来没有人在这台会让它失败的机器上跑过这个测试。**

- 开发机（M0–M3 证据来源）：缺陷潜伏，绿是真绿；
- 虚拟机（M4 战役）：跑的是聚焦子集，这个测试不在里面；
- **M5 是第一个在虚拟机上跑完整 146 项套件的**——所以缺陷现在才浮出来。

这不是"地基是坏的"，而是"**虚拟机上的完整套件是一片从未勘测过的区域**"。
M4 发布门针对它自己的行为（VM-1…VM-5、聚焦 9 项）的证据仍然成立。

### 首次勘测的结果：不可靠集合只有 2 个

2026-10-14 做了第一次稳定性普查：**146 个用例 × 5 次 = 730 次运行**，结果
**718/730 通过**，而且只有 4 个用例曾经不绿：

| 用例 | 5 次结果 | 判定 |
|---|---|---|
| `structural.m2.storage_independent_build` | 1,1,1,1,1 | 确定性（客机无 VS）→ 环境 |
| `structural.m5.execution_boundary` | 1,1,1,1,1 | 确定性（客机无 VS）→ 环境 |
| `m5.production_execution_backend` | 0,0,**1**,0,0 | 真间发（见 §1） |
| `unit.m2.backing_generation` | 0,0,0,0,**崩溃** | 真间发（见 §2.6，**已修**） |

**其余 144 个用例 5/5 全绿。** 所以"前置内容可能全面不可靠"这个担心在数据上不成立：
已知的不可靠点是 2 个，不是一个面。

（普查用的是 Consecutive 顺序——每个用例连跑 5 次。这个顺序对"找单个用例的抖动"信号最强，
但设备类用例的连跑可能自我干扰；`-RepeatMode Sweep` 提供了逐遍整套的对照顺序，
结果不一致时应以 Sweep 为准。）

### 这对下一步意味着什么

M2/M3 的 legacy 测试集在虚拟机上**从未被完整跑过**。因此：

1. "稳定性普查"（整套重复 N 次）不只是找间发——它是在**首次勘测 VM 环境下的全套行为**；
2. §2 的崩溃需要 dump 来定位**到底在产品代码还是测试脚手架**——该测试 3 个 include 里
   有 2 个是脚手架（`test_framework.h`、`fake_backing_store.h`）；
3. 在拿到 dump 之前，**不能**把它定性为"只是测试问题"——它也可能正是
   backing view 生命周期上的真实竞态，而 M3/M4/M5 全都坐在动态块存储之上。

---

## 3. 本轮新增的工具能力

| 能力 | 用途 |
|---|---|
| `-Run -Filter <regex>` | 只跑匹配的用例 |
| `-Run -Repeat <N>` | 在**客机内**重复 N 次（一次往返拿到复现率；单用例 55 秒时这条很关键） |
| `-Run -MinTimeoutSeconds <N>` | 超时**下限**（只抬不降）：客机比 ctest 预算所假设的机器慢 |
| `-Run -TimeoutOverrideSeconds <N>` | 诊断用的强制覆盖 |

---

## 4. 建议的下一步

1. **先修 §1 的写冻结**：根因清楚、修法明确、复用 M4 已验收机制，而且它落在
   `disk stop`（目标 G2 的核心动作）与 preshutdown 的必经路径上。
2. **§2 的崩溃走取证**：配 LocalDumps → 复跑 → 读栈。在此之前不要凭猜测改 M2 代码。
3. 两者都不阻塞 T1-02 的开工，但 **§1 必须在 T1-09（单盘验收）之前修完**，
   否则 preshutdown 的逐盘 stop 会以 25% 的概率踩到同一处。
