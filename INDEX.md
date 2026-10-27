# INDEX

| 日期 | 编号 | 试了什么 | 结论 | 详情 |
|---|---|---|---|---|
| 2026-10-27 | M5-T4-03 | 压缩自动触发：策略函数 → tick 候选 → 派发 → 去重守卫 → 可观测 | **PASSED**。客机三次通过（130/103/118 s），日志 `maintenance: compaction dispatched (checkpoint-count)`，`disk show` 出 `compactions=N`，e2e 以断言守住 | `docs/operations/m5-handoff.md` §3.127–§3.143 |
| 2026-10-27 | M5-T4-02a | 卷序列号端到端 | **完成**：客机 `serial=5A42-9D28` | §3.118–§3.126 |
| 2026-10-27 | M5-T4-02b | 易失盘数据丢失保护 | **完成**：无标志 `exit=1`、有标志 `exit=0`，已进 e2e；期间修掉"保护从未实现"与"确认标志无法编码"两个缺陷 | §3.159–§3.162 |
| 2026-10-27 | M5-T4-02c | Load Only（`create --no-start`） | **create 半边 PASSED**：客机 `cli_e2e` 两次通过（30 s 内 terminal、无残留 worker、该盘 forget `device_absent=1`）。**start 半边仍未完成**：M3 `match_created_disk` 拒绝非空的续跑盘 | §3.188 |
| 2026-10-27 | M5-DEF-A | 缺陷 A：`--no-start` 的 create 不终态、留 worker、占设备 | **PASSED**。根因是 publish-provisioning 二次观测已退出的 worker（失败分支还读错变量）+ `current_dirty_epoch` 阻塞拿 gate；客机三判据两次通过 | §3.188 |
| 2026-10-27 | M5-INFRA | 可诊断性：`shutdown` 结果入 trace、重复注册错误报明冲突字段、`killed` 旧任务清理 | 完成（零成本 trace，长期保留） | §3.172、§3.175 |
| 2026-10-27 | M5-T4-02d | 单文件 VHDX 导出 | **PASSED**：客机 `export exit=0`、14,680,064 B、generation 不变（5）、二次导出拒绝覆盖；导出路径在发布前用 `VhdxReader` 逐 payload 读回 | §3.189 |
| 2026-10-27 | M5-T4-04 | release manifest 冻结 | **PASSED**：干净 Debug/Release 451/451（`/WX`）、manifest 4 binaries + 8 readings（`frozen_at 2026-10-27T15:37:45Z`）、validator `accepted`、自检四拒四中 | §3.190 |

**当前状态**：三件任务（缺陷 A、VHDX 导出、T4-04 冻结）均已完成。**唯一已知缺口：Load Only 的 start 半边**（`--no-start` 创建的盘无法 `disk start`，§3.188）；冻结基于当前构建，修它会改变哈希、必须重新冻结。可向用户报告并请求进入 M5 虚拟机验收，同时请其决定该缺口是否作为遗留项。