# INDEX

| 日期 | 编号 | 试了什么 | 结论 | 详情 |
|---|---|---|---|---|
| 2026-10-27 | M5-T4-03 | 压缩自动触发：策略函数 → tick 候选 → 派发 → 去重守卫 → 可观测 | **PASSED**。客机三次通过（130/103/118 s），日志 `maintenance: compaction dispatched (checkpoint-count)`，`disk show` 出 `compactions=N`，e2e 以断言守住 | `docs/operations/m5-handoff.md` §3.127–§3.143 |
| 2026-10-27 | M5-T4-02a | 卷序列号端到端 | **完成**：客机 `serial=5A42-9D28` | §3.118–§3.126 |
| 2026-10-27 | M5-T4-02b | 易失盘数据丢失保护 | **PASSED**：无标志 `exit=1`、有标志 `exit=0`；**2026-10-28 补修 dirty force-stop**：`accept_data_loss` 经 removal authorization 到 worker，易失盘改为“先 checkpoint、再写文件”制造 dirty，forced stop 成功，trace `dirty writes discarded…` | §3.159–§3.162、§3.193–§3.194 |
| 2026-10-27 | M5-T4-02c | Load Only（`create --no-start` + 之后 `disk start`） | **PASSED**：客机 `cli_e2e` 三次通过（120 s / 122 s / 109 s）——create 30 s 内 terminal、无残留 worker、start terminal succeeded、`Y: Ready running generation=1`、checkpoint/stop、forget `device_absent=1` | §3.188、§3.191 |
| 2026-10-27 | M5-DEF-A | 缺陷 A：`--no-start` 的 create 不终态、留 worker、占设备 | **PASSED**。根因是 publish-provisioning 二次观测已退出的 worker（失败分支还读错变量）+ `current_dirty_epoch` 阻塞拿 gate；客机三判据两次通过 | §3.188 |
| 2026-10-27 | M5-INFRA | 可诊断性：`shutdown` 结果入 trace、重复注册错误报明冲突字段、`killed` 旧任务清理 | 完成（零成本 trace，长期保留） | §3.172、§3.175 |
| 2026-10-27 | M5-T4-02d | 单文件 VHDX 导出 | **PASSED**：客机 `export exit=0`、14,680,064 B、generation 不变（5）、二次导出拒绝覆盖；导出路径在发布前用 `VhdxReader` 逐 payload 读回 | §3.189 |
| 2026-10-27 | M5-T4-04 | release manifest 冻结 | **PASSED（G3 修复后重新冻结）**：manifest 4 binaries + 8 readings（`frozen_at 2026-10-29T10:05:04Z`；service `8f8f855d`、cli `ebdedbf6`、gui `744a68f5`、worker `7fb9032b`）、validator `accepted` | §3.190、§3.197 |

| 2026-10-28 | M5-ACC-01 | M5 验收 | **G2/G1/T4-01 通过**（cli_e2e 1/1 120 s、gui 1/1 29 s、install 1/1 7.5 s）；**G3 修复后连续两次通过**（`T: running`、generation 4/3、marker 65 字节一致、worker 1、aim 0）；全量 160 卡在 `unit.m2.harness_preflight`（agent 旧请求重放，未完整跑完） | §3.196、§3.197 |

**当前状态**：G1/G2/G3 单例全部通过（G3 连续两次）。剩余：m1–m4/unit/model 全量回归未完整跑完，之后是最终验收。