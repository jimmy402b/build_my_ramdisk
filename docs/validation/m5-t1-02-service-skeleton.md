# T1-02 证据：组合根 + 服务骨架（SCM 注册/启动/停止/preshutdown）

日期：2026-10-15（客机本地 2026-10-16）
分支：`feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`
设计：`docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md` §9
增量：tasklist T1-02
**前一轮进展**（只有 `ServiceHost`）见本文件 git 历史；本轮补齐组合根与 SCM 包装。

---

## 0. 结论

| 项 | 结果 |
|---|---|
| `sc create` | 成功 |
| `sc start` → `sc query` | `START_PENDING` → **`RUNNING`**（`STOPPABLE, NOT_PAUSABLE, ACCEPTS_PRESHUTDOWN`），`WIN32_EXIT_CODE 0` |
| `sc stop` → `sc query` | **`STOPPED`**，`SERVICE_EXIT_CODE 0`，进程退出 |
| 无盘干净退出 | 通过（`--console` 与 SCM 两种驱动方式都验过） |
| 退出后残留 | `myrd-service: 0 / myrd-m5-worker: 0 / disks: 0` |
| fail-closed | 未安装的机器上启动失败并给出原因（见 §4） |
| 开发机纯逻辑 | `m5.service_host` **8/8**（未改动，仍通过） |

**T1-02 达成验收标准。** 用词与量测范围见 §5。

---

## 1. 已交付

| 文件 | 内容 |
|---|---|
| `src/service/service_host.{h,cpp}` | 服务自身生命周期（上一轮） |
| `src/service/service_system.{h,cpp}` | 三个生产接缝：`SystemClock`、`SystemIdGenerator`、`SystemPhysicalMemoryQuery`，加 `StderrServiceLogger` |
| `src/service/service_runtime.{h,cpp}` | **组合根**：持有全部部件，按依赖顺序装起来；`ProductionRepairBindingVerifier` |
| `src/service/service_main.cpp` | **SCM 包装**：`StartServiceCtrlDispatcherW`、`RegisterServiceCtrlHandlerExW`、状态上报、`--console` |
| `src/CMakeLists.txt` | `myrd_service` 库 + `myrd-service` 可执行文件（两者都挂了 `myrd_assert_execution_boundary`） |

三个"只存在于测试替身里"的接口（时钟 / ID / 物理内存）现在有了生产实现——此前没有任何东西组装真实服务，所以它们从未被需要过。

---

## 2. 组合根的两个真实约束（写下来，因为编译器逼出来的）

**① 引用循环。** 调度器借用 `ServiceCore`，而 `ServiceCore` 借用存储。所以 core 必须**声明在前、构造在后**。
`ServiceRuntime` 因此把 `core_` 与 `scheduler_` 放进 `std::optional`，由 `create` 最后 emplace。
普通成员做不到这件事，注释里写了原因而不是留个谜。

**② 存储借用时钟。** `ServiceOperationStore::open_existing` 借 `IServiceClock&`。
如果工厂用局部时钟建它，服务一启动就是悬垂引用。所以存储也在 `create` 第二阶段对着
**runtime 自己的 `clock_`** emplace。两者都没有默认构造函数，这也是必须用 `optional` 的第二个原因。

**③ `ServiceHost` 的移动是手写的。** `std::mutex` 成员会让**隐式**移动构造被删除，
所以 `= default` 也是删除的，调用方会静默回落到已删除的拷贝。
显式版本重建互斥量（移动一个 mutex 没有意义）、搬移状态字段。
移动**赋值**是 `= delete`：它持有引用，引用不能重绑——这正是这个对象要消灭的悬垂状态。

---

## 3. SCM 实测（客机 `myrd-vm`，管理员，session 0）

```
=== create ===
[SC] CreateService 成功

=== start ===
        STATE              : 2  START_PENDING
=== query while running ===
SERVICE_NAME: MyRamDiskService
        TYPE               : 10  WIN32_OWN_PROCESS
        STATE              : 4  RUNNING
                                (STOPPABLE, NOT_PAUSABLE, ACCEPTS_PRESHUTDOWN)
        WIN32_EXIT_CODE    : 0  (0x0)
        SERVICE_EXIT_CODE  : 0  (0x0)
        CHECKPOINT         : 0x0

=== stop ===
        STATE              : 1  STOPPED
=== state after stop ===
        STATE              : 1  STOPPED
        SERVICE_EXIT_CODE  : 0  (0x0)

=== 残留 ===
service: 0   workers: 0   disks: 0
```

`ACCEPTS_PRESHUTDOWN` 是设计 §9 要求的（OS 关机时 SCM 先发 preshutdown），
它出现在 `sc query` 里，说明 `SERVICE_ACCEPT_PRESHUTDOWN` 真的报给了 SCM。

**无盘的实证**：这台客机的 `C:\ProgramData\MyRamDisk` 只有安装器建的 `profiles` / `operations`
两个空根，服务启动时 `recovered=8`——那 8 条是此前测试留下的 durable operation 记录，
被 `recover_after_restart()` 正确对账。**没有盘被拉起，服务照常 up/down**，这就是"无盘干净退出"。

---

## 4. fail-closed（按裁决）

**决策**：ProgramData 根由 **T4-01 安装器**创建（它同时拥有 ACL）；服务**只打开已存在的东西**。
一个自己创建根的服务会把一次提权写操作放进自己的启动路径——而那正是它最不能承受这种操作的地方；
半建好的根比不存在的根更糟，因为它看起来像已安装。

实测（开发机，未安装）：

```
myrd-service: build_state failed: op=profile_store.directory_acl
              msg=A profile directory ACL is not protected native=0
```

服务启动失败、退出码非 0。**注意错误来自存储层而不是我的前置检查**，这是刻意的：
产品根存在但不是目录时才由前置检查报错；**名字不存在**时，第一个存储的失败才是更准确的答案
（它会说是哪个受保护根打不开，而前置检查只能说某个名字不在）。这条写进了代码注释。

---

## 5. 量测范围（重要，不含糊）

- 本节全部读数取自**虚拟机客机**，真实 SCM、真实 ProgramData 布局、管理员 session 0。
- 每个结论**各 1 次**：`sc create` / `start` / `query` / `stop` 各一次，退出后用 `Get-Process` /
  `Get-Disk` 各查一次。这是**生命周期验收**，不是稳定性验收；稳定性（`-Repeat`）在
  T1-11/§4 那类间歇缺陷上是要求，这里没有可重复的竞态可供重复测量。
- 未验证：**preshutdown 真的被 OS 发起**。`ACCEPTS_PRESHUTDOWN` 已上报，控制码分支已实现，
  但没有真的关机去触发它——那属于 T1-09 的断电/关机矩阵。

---

## 6. 本轮踩到的两个坑（都值得记）

### 6.1 `-Stage` 会**静默跳过**正在运行的 exe

服务进程占用着 `myrd-service.exe`，`scp` 覆盖失败但 `-Stage` 仍报成功。
结果我对着**旧二进制**测了很久，诊断代码明明在源码里却从不出现。
判别方法：把已 staged 的 exe 读成字符串找标识串。

**规则：测一个服务之前，先把它停掉再 `-Stage`。**

### 6.2 `RegisterServiceCtrlHandlerExW` 的 context 到达时是 `nullptr`

这是**产品缺陷级**的发现。我按文档把 `ServiceState*` 作为 context 传进去，
处理器收到的却是 `nullptr`——于是它给每个控制都回失败，**服务无法停止**。
而 `sc stop` **不报任何错**、状态一直是 `RUNNING`：

```
control=4 context=0000000000000000
control=1 context=0000000000000000     ← 停不下来的原因
```

`ServiceMain` 没有自己的 context 参数，所以状态改由文件静态持有；context 仍然接受但不再信任。
**如果没加那条 trace，这个缺陷会表现为"SCM 说停成功了但服务还在跑"**，很容易被误判成 SCM 的问题。

> 附带教训：服务没有控制台，"什么也没发生"必须靠**写文件**才看得见。
> 我最初用 stderr——**那是错的**，因为服务模式下 stderr 无处可去；而且日志目录
> `C:\tmp\m5-vm` 在客机上并不存在，`fopen_s` 失败得无声无息，第一轮 trace 一条都没写出来。
> 写文件 + 确认目录存在，这两步都不能省。

---

## 7. 尚未做（不阻塞 T1-02 的验收，但要说清）

- **逐盘关停**（设计 §9："按预算逐盘 checkpoint + 拆卸"）不在这里，归 T1-03/T1-05。
  这里只有 scheduler 的停止，因为那是"任何被它调用的东西被销毁之前"必须先发生的一步。
- **public pipe** 是 T1-05。`execution_gate_` 现在没有任何竞争者，但已经按"将来与 pipe 共用"
  建好了，免得以后重建调度器。
