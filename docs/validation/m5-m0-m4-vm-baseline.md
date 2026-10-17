# M0–M4 在虚拟机上的可靠性基线测量

日期：2026-10-17
分支：`feature/m5-lifecycle`（HEAD `37d1115`）
请求：`ea76efa7beee4a6686dc904af80a69fd`（1 遍）、`ff7c84d057754c70bd226ac76a72d5c7`（3 遍）
地址：客机 `myrd-vm`，harness `tools/m5vm/Invoke-M5Vm.ps1 -Run`

## 这次测量在回答什么问题

用户提出的问题：**M0–M4 提供的服务是否可靠？如果不可靠，建在其上的 M5 就是南辕北辙。**

因此这里**只跑 M0–M4**，把 M5 的 17 个测试排除，得到 M0–M4 自己的读数。

**筛选方式**（可复现）：`tests.json` 共 151 条，用负向前瞻排除 M5 后剩 **134 条**：

```
-Filter '^(?!unit\.m5\.|integration\.m5\.|m5\.|structural\.m5\.)'
```

被排除的 17 条是：`unit.m5.registration_codec`、`unit.m5.service_worker_control`、
`unit.m5.worker_protocol`、`unit.m5.worker_session`、`unit.m5.service_worker_session`、
`unit.m5.worker_private_pipe`、`integration.m5.checkpoint_inspection`、
`m5.production_execution_backend`、`m5.service_operation_store`、`m5.service_orchestration`、
`m5.execution_scheduler`、`m5.service_host`、`m5.worker_sweep_policy`、`m5.worker_sweep`、
`m5.worker_binding_store`、`m5.startup_sweep`、`structural.m5.execution_boundary`。

## 客机环境（测前）

```
workers: 0
disks:   1        （VMware Virtual NVMe Disk，即系统盘）
letters: C
phdskmnt: Running
MYRD_VM_ELEVATED=True
```

## 读数

### 第 1 遍（单次，7 分 39 秒）

```
[134/134] 完成
133 of 134 passed
  FAIL structural.m2.storage_independent_build (exit 1)
```

### 第 2–4 遍（`-Repeat 3`，14 分 37 秒，共 402 次执行 = 134 × 3）

```
399 of 402 passed
  FAIL structural.m2.storage_independent_build#1 (exit 1)
  FAIL structural.m2.storage_independent_build#2 (exit 1)
  FAIL structural.m2.storage_independent_build#3 (exit 1)
```

## 唯一失败项：不是产品缺陷，是客机环境限制

`structural.m2.storage_independent_build` 的 stderr：

```
vswhere.exe not found
   at tools\Enter-MsvcEnvironment.ps1:6
   +  (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found' }
```

**该用例要验证"不依赖 Visual Studio 也能构建"，而验证方式是去调 `vswhere.exe`——
客机上没有 Visual Studio，所以它连前提都不具备。**

⇒ **它不是"产品行为不可靠"，而是"这台客机上该测试无法执行"。**
**这与计划 §2 早已记录的一条一致**：
"唯二失败是客机无 Visual Studio 的两个 structural 用例（已知环境受限）"。
本次测量里另一个（`structural.m5.execution_boundary`）被排除在 M0–M4 之外，所以只看到这一个。

## 结论：M0–M4 在客机上是可靠的，而且稳定性是"逐项一致"

| 指标 | 读数 |
|---|---|
| M0–M4 测试数 | **134** |
| 单遍通过 | **133 / 134**（99.3%） |
| 三遍合计 | **399 / 402** |
| 三遍失败集合 | **完全相同**（同一个用例 × 3） |
| 真实产品缺陷数 | **0** |
| 偶发（flaky）项 | **0** |

**"三遍失败集合完全相同"这一点比通过率更重要**：
它说明 M0–M4 **没有间歇性**。对照之下，M5 的
`m5.production_execution_backend` 是间歇失败的（16/18 与 3/18 交替）。
**所以"不稳定"这个属性属于 M5 的使用方式，不属于 M0–M4 本身。**

覆盖到的真实设备路径（不只是逻辑测试）包括：
`integration.m1.shm`、`integration.m3.windows_vhdx`、`integration.m3.windows_acceptance`、
`integration.m3.cli_local_e2e`、`integration.m3.image_create`、`integration.m3.checkpoint`、
`integration.m3.recovery`、`integration.m4.run_loop_runtime`、
`integration.m4.product_composition`、`integration.m4.multi_instance`、
`m4.recovery_commands`、`m4.checkpoint_operation`。

## 但是：这个读数**不能**推出"M0–M4 对 M5 的所有用法都可靠"

必须把边界说清楚，否则会得出错误结论。

**测试套件通过 ≠ 所有服务形态可靠。** 理由是具体的：

**M0–M4 的测试从来没有测过"在一块设备正被退役的同时去打开它"。**
这个组合是 **M5 的 reclaim 第一次引入的**——
reclaim 要在拆卸路径上取盘号，而拆卸本身正在让设备消失。

而这恰好就是 T1-03 卡住的那 11 秒阻塞
（`CreateFileW`，实测 11017 ms，证据见
`docs/validation/m5-t1-03-worker-job.md` §4.15.57）。

⇒ **正确的因果是**：
**M0–M4 自身可靠；M5 首次行使了一条 M0–M4 从未被测试过的用法；
那条用法踩到了 M0 层的一个真实脆弱点。**

## 对"是否从零重写 M5"的直接含义

**这个读数不支持"重写 M5"，理由是具体的、可检验的**：

**阻塞点 `CreateFileW` 位于 `src/driver/physical_drive_locator.cpp`——那是 M0 层，
不是 M5 层。** 从零重写 M5 不会改动这个文件的一行，
**所以重写 M5 并不能消除这个阻塞**。

而如果新的 M5 设计选择"reclaim 完全不取盘号"，
那**同样绕不过去**——因为 part 72 已实测 `find_recovered_volume` 确实需要盘号
才能在那块盘上找到卷。

⇒ **真正待决定的仍然是那个设计问题（见 handoff §7.0.10）：
如何在不"打开候选盘"的前提下取得盘号。** 这是设计缺口，不是 M5 的实现质量问题。

## 这次测量没有覆盖什么（诚实标注）

1. **只跑了本仓库自己的测试套件**。套件本身可能有盲区（上面的
   "退役窗口内打开设备"就是一个具体例子）。
2. **没有重跑 M4 的 VM-1…VM-5 五场景战役**（那次战役的资格包在
   `C:\m4-final-evidence-20260905\qualification`，日期 2026-09-05），
   本次只跑了 CTest 套件。
3. **没有覆盖"断电"这类真物理事件**——套件里的
   `integration.m3.crash_*`、`soak_*` 是模拟/编排的崩溃路径，
   真断电只有 M4 那次 VM 战役做过（VM-2、VM-4）。
4. **一次测量、一台客机**。跨机器/跨 Windows 版本的可用性未测。
