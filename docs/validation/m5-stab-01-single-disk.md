# STAB-01 单盘硬保护：第二块盘在准入阶段被拒绝

**结论**：`disk create` 与 `disk start` 在已有盘处于 up 状态时，于**准入阶段**返回稳定错误码
`0x5609`（`ServiceCoreError::single_disk_violation`），文案写明 “M5 is a single-disk product”。
拒绝发生在任何 worker / 设备 / bundle / 持久化记录产生之前：客机读数里，第二块盘 13–16 ms 被拒，
第一块盘的 generation、挂载点、worker 全部不变，第一块盘的 `forget` 仍然证明 `device_absent=1`。

**客机**：`myrd-vm`，Windows 11 x64，Release，`Reset-M5Guest` 后空机；`integration.m5.cli_e2e`
连续两次通过（109 s / 108 s）。

---

## 1. 为什么这是一个增量

M5 是单盘产品（G4 于 2026-10-17 延后），但此前没有任何东西**执行**这条范围。用户可以创建第二块盘；
两块盘同时运行时，第二块的拆卸不会真正移除设备：restart 报
`no volume carries the recorded identity`，forget 报 `device_absent=0`。这比“不支持多盘”更糟——
产品看起来支持，然后把设备留在那里。T1-08 的证据文档（§3.1）已经量到过这个形态。

修法只有一条：**不承认第二块盘**。拒绝必须发生在工作之前，否则“拒绝”也会先开一个设备。

## 2. 实现落点

| 层 | 落点 | 作用 |
|---|---|---|
| 契约 | `src/product/service_status.h:62` `ServiceCoreError::single_disk_violation` | 稳定错误码 `0x5609`；**追加**在枚举末尾，已有编号不变 |
| 准入 | `src/management/service_core.cpp:1508` `ServiceCore::refuse_if_another_disk_is_up()` | 唯一的执行点；返回 `Result<void>`，调用方无法把“拒绝”当布尔忽略 |
| 创建 | `service_core.cpp:1880`（`service_core.single_disk_create`） | `disk create` 在派生 profile id 之后、reservation/admission 之前拒绝 |
| 启动 | `service_core.cpp:2483` / `:2600`（`service_core.single_disk_start`） | 真实路径与 `--dry-run` 同判；只对 `plan.disposition == allowed` 生效，幂等 `already_running` 不受影响 |
| 日志 | `ServiceLogKind::admission_refused`（`service_core.h:863`；写入 `service_core.cpp:1552`） | 被拒命令没有 durable operation 记录，原因必须落在 service.log |
| CLI | `src/cli/cli_renderer.cpp` | 新错误码映射到退出码 `1`（产品拒绝），不是 `2`（用法）或 `3`（不可达） |

**“up”的定义**：`RuntimeState != stopped`，即 `starting` / `running` / `stopping` / `orphaned` 都算。
`orphaned` 正是第二块盘绝对不能去撞的那个残留。

**两个检查，缺一不可**：

1. **pending 队列**：create 在 execution 之前不发布 profile，所以“两个 create 背靠背”在 profile 列表里
   都是空的；第一个 create 的 pending 条目是第二个被拒的唯一依据。start 在准入时就发布 `running`，
   pending 条目补上“发布仍在途中”的窗口。
2. **durable profile 列表**：另一块盘已经发布为 up（或刚从断电恢复、状态还是 running）。

排除项 `excluded_profile` 只用于**幂等重放**：同一个 request id 的 create 会派生出同一个 profile id，
必须落到 durable operation 记录，而不是被当成“第二块盘”。

`disk import` 不在此列：它只发布一个 stopped profile、不拉起盘；对一个 stopped 的 import 目标执行
`disk start` 时，同一道 guard 仍然生效。

## 3. 本机纯逻辑读数（开发机）

`m5.service_orchestration` 1/1，新增 4 个用例：

| 用例 | 断言 |
|---|---|
| `m5_single_disk_refuses_a_create_while_another_disk_runs` | 0x5609；step=`single_disk_create`；消息含 `single-disk`；**没有** operation 记录、**没有** registration reservation；profile 快照逐字段不变；日志 kind=`admission_refused` |
| `m5_single_disk_refuses_a_create_while_the_first_is_still_pending` | 第一个 create 已准入但未发布 profile 时，第二个 create 仍被拒；只有一条 operation、一次 reservation |
| `m5_single_disk_refuses_a_start_while_another_disk_runs` | 0x5609；step=`single_disk_start`；目标 profile 没有被发布成 running |
| `m5_single_disk_allows_a_start_when_nothing_else_is_up` | 反向对照：没有别的盘时 start 照常准入（guard 不能变成“start 一律拒绝”） |

受影响套件本机全绿：`m5.service_orchestration`、`m5.execution_scheduler`、`m5.service_host`、
`m5.service_maintenance`、`m5.service_operation_store`、`m5.cli`、`m5.gui`、`m4.checkpoint_operation`、
`m4.recovery_commands` 等 12 个（`m5.startup_sweep` 按设计拒绝在开发机运行）。

## 4. 客机读数

### 4.1 第一次（`83824dd2…`）：拒绝正确，断言写错

```text
--- STAB-01: a second disk is refused while the first one is running
  second create on T: exit=1 in 16 ms
  error: M5 is a single-disk product: only one disk may be up at a time. ...
  command: disk.create
  step: service_core.single_disk_create
  code: 0x5609 domain 8
FAIL: the refused second disk appears in disk list at T:
```

拒绝本身完全正确。失败的是断言：PowerShell `-match` **默认不区分大小写**，而 `disk.list: completed`
里含有 `t:`，于是 `T:` 命中了 disposition 行。修法：断言改为 `-cmatch`（并要求 `T:` 后跟空白）。
这条假失败保留在此，因为它证明“读数脚本也会说谎”。

### 4.2 通过（`a093080a…`，109 s）

```text
--- STAB-01: a second disk is refused while the first one is running
  second create on T: exit=1 in 13 ms
  error: M5 is a single-disk product: only one disk may be up at a time. Stop the running disk before creating or starting another one.
PASS STAB-01: second create refused (code 0x5609), first disk unchanged (generation=2 mount=U: workers=1)
  ...
  first disk forgotten; device_absent=1 proved
  stopped disk forgotten; device_absent=1 proved
PASS integration.m5.cli_e2e
```

### 4.3 连续第二次（`ff421180…`，108 s）

同一段读数：`exit=1 in 16 ms`、`PASS STAB-01 … (generation=2 mount=U: workers=1)`、
`first disk forgotten; device_absent=1 proved`。两次 `cli_e2e` 的通过结果一致。

### 4.4 这段断言具体测了什么

| 断言 | 读数 |
|---|---|
| 第二次 create 被拒 | `exit=1`，`code: 0x5609` |
| 文案明确单盘 | 输出含 `M5 is a single-disk product` 与 `single-disk` |
| 拒绝在准入、不是工作后 | 13–16 ms；`< 30 s` 上限断言 |
| 第二块没有留下任何东西 | 它的 bundle 目录不存在；`disk list` 里没有它的盘符；worker 进程集合逐 PID 不变 |
| 第一块不受影响 | `generation=2` 不变、`mount=U:` 不变、`runtime=Running`、worker 1 个 |
| 第一块仍可 checkpoint/stop | 后续 T1-07 读写、checkpoint、stop、restart、export、Load Only 段全部沿用原断言并通过 |
| 第一块 forget 仍安全 | service.log 的 `forget safety proved:` 行含 `device_absent=1` |

## 5. 没有覆盖的部分（明确写出来）

- **历史遗留的第二块盘**：旧版本已经创建、且仍处于 running 的第二块盘不会被自动拆除；它会被 guard
  当作“另一个 up 的盘”拒绝新的 create/start。清理它仍走 `disk stop` / `disk forget`。
- **两块盘同时 running 的断电恢复**：`admit_auto_starts` 会先退休陈旧 running，再逐块 dispatch；
  第二块会被 guard 拒绝并写 `auto-start refused` 日志，保持 stopped。该形态没有客机读数
  （正常路径不可能再产生它）；如果将来需要，用 STAB-03 的故障矩阵补。
- **G3 未重跑**：本次改动落在 start 的准入点，而 G3 的自动启动走 `admit_auto_starts`（先发布
  stopped 再 start），逻辑上不受影响；但按“G3 已验证、不重做”的指示没有重跑。H1 收尾时若要更硬的
  证据，按 `Test-M5PowerLoss.ps1 -SkipStage` 补一次即可。

## 6. 发布影响

改了产品代码（`service`、`cli` 至少变化；`worker` 不变），所以 `out/m5-evidence` 现在的 release
manifest 与二进制**不再对应**，`Verify-M5Evidence.ps1` 会拒绝。H1 三项全部完成后再重新冻结，
不要用旧 `frozen_at` 作为 STAB-01 的证据。
