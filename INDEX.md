# INDEX

| 日期 | 编号 | 试了什么 | 结论 | 详情 |
|---|---|---|---|---|
| 2026-10-27 | M5-T4-03 | 压缩自动触发：策略函数 → tick 候选 → 派发 → 去重守卫 → 可观测 | **PASSED**。客机三次通过（130/103/118 s），日志 `maintenance: compaction dispatched (checkpoint-count)`，`disk show` 出 `compactions=N`，e2e 以断言守住 | `docs/operations/m5-handoff.md` §3.127–§3.143 |
| 2026-10-27 | M5-T4-02a | 卷序列号端到端 | **完成**：客机 `serial=5A42-9D28` | §3.118–§3.126 |
| 2026-10-27 | M5-T4-02b | 易失盘数据丢失保护 | **完成**：无标志 `exit=1`、有标志 `exit=0`，已进 e2e；期间修掉"保护从未实现"与"确认标志无法编码"两个缺陷 | §3.159–§3.162 |
| 2026-10-27 | M5-T4-02c | Load Only（`create --no-start`） | **语义已验证**（`Ready stopped/configured runtime=Stopped mem=0/0`），**被缺陷 A 阻塞** | §3.147–§3.153 |
| 2026-10-27 | M5-DEF-A | 缺陷 A：`--no-start` 的 create 不终态、留 worker、占设备 | **已定位未修**：`ServiceWorkerControl::current_dirty_epoch()` 阻塞等 `command_gate_`；同文件 `observe_runtime()` 已用 `try_to_lock` 且注释写明理由 | §3.169–§3.187 |
| 2026-10-27 | M5-INFRA | 可诊断性：`shutdown` 结果入 trace、重复注册错误报明冲突字段、`killed` 旧任务清理 | 完成（零成本 trace，长期保留） | §3.172、§3.175 |
| 2026-10-27 | M5-T4-04 | release manifest 冻结 | **未开始**（须在 T4-02 之后） | tasklist T4-04 行 |

**当前状态**：M5 未达"可进入虚拟机验收"。剩余 T4-02（VHDX 导出）、缺陷 A、T4-04 冻结。