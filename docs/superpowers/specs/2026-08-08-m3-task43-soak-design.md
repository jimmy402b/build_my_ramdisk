# M3 Task43 Soak、资源与性能验收设计

## 目标

Task43 为 M3-V7 提供可重复、机器可审计的正式验收。它必须证明生产持久化路径在混合 I/O、checkpoint、reopen、compaction、内存压力、故障和重复 close 下保持数据正确、资源有界，并把结果与同一源码提交的 Task41 RealFunctional 和 Task42 Crash Full 证据绑定。

Quick profile 只验证 harness 本身，不能完成 Task43。只有 Formal profile 同时通过本地 soak、独立 verifier、RealFunctional 和 Crash Full 输入门禁，才可输出 `task43_complete=true` 和 `milestone_eligible=true`。

## 运行分工

- 宿主机运行 MSVC Release 构建、本地 production-path soak、资源和性能测量。
- Windows 11 来宾运行需要管理员权限的 AIM/NTFS、原生 VHDX 和真实进程终止/受控断电场景。
- Formal 报告只消费显式路径，不保存 SSH 密钥、虚拟机密码或机器可变凭据。
- Task41、Task42 和 Task43 的 `source_commit` 必须完全相同；二进制 SHA-256 也必须与输入证据一致。

## 组件

### C++ soak executable

新增 `myrd_m3_soak_tests`，只链接现有 production storage/persistence 实现和测试支持，不修改产品算法。它接受环境变量选择 Quick/Formal profile 和确定性 seed，运行：

1. mixed read/write/zero/unmap 与独立 shadow model；
2. checkpoint、reopen、full-hash verify 和 compaction 循环；
3. 并发 first-touch 与 checkpoint cut；
4. low-memory cache/COW pressure 和注入 I/O failure；
5. 重复 close/reopen、句柄/线程/临时文件回收；
6. Formal 专用 100 GiB logical/1 GiB allocated 稀疏 profile；
7. Formal 专用 256 MiB dirty checkpoint 写放大 profile。

可执行文件向 stdout 输出版本化的单行 JSON summary；非零退出码表示 correctness 或 threshold gate 失败。失败输出 seed、profile、phase 和最小可复现计数。

### PowerShell orchestrator

`tests/e2e/Invoke-M3Soak.ps1` 负责：

- 校验 evidence 路径位于仓库外、非 reparse point、初始为空；
- 取得当前 Git commit 和所有输入 SHA-256；
- 启动独立子进程、限制超时并保存原始 stdout/stderr；
- Quick 使用固定 seeds `0`、`1`、`0xC0FFEE`、`0xDEADBEEF`；
- Formal 额外使用 `SHA256(source_commit | 43 | i)` 低 64 位生成至少 100 个 seeds；
- Formal 读取并绑定 RealFunctional 和 Crash Full 报告；
- 原子发布 `myrd.m3.soak-evidence.v1` JSON；
- 清理残留子进程和临时目录，任何残留使结果失败。

### 独立 verifier 与 route

`tools/Verify-M3Json.ps1` 增加 soak schema 分支，拒绝：

- source commit、可执行文件或原始日志哈希不匹配；
- Quick 报告声称 milestone eligible；
- Formal 缺少同 commit 的 RealFunctional/Crash Full 证据；
- 少于 100 个 Formal seeds 或任一 seed 非零退出；
- mixed model/hash、完整代、lazy-load 或 cleanup 证明缺失；
- 100 GiB startup read 超过 64 MiB；
- M3 额外 committed memory 超过 128 MiB；
- 256 MiB dirty checkpoint 物理 journal 增量超过 payload 的 120%；
- mutation admission pause 或单次 writer stall 超过 250 ms；
- handle/thread/device/temp/view/COW/cache/pin/waiter 残留。

`tools/Invoke-M3Verification.ps1 -Tier Soak` 默认 Formal。缺少显式证据返回 `BLOCKED/77`；格式或阈值错误返回 FAIL；Quick 只能输出开发级 PASS 且 `task43_complete=false`。

## Evidence schema

Formal 报告至少包含：

- `schema/status/terminal_verdict/profile/source_commit`；
- executable、raw log、Task41、Task42 SHA-256；
- seeds、operation counts、checkpoint/reopen/compaction counts；
- correctness hashes 和 old/new complete-generation proof；
- startup read、committed memory、journal growth、COW/stall 指标；
- throughput、P50/P95/P99、checkpoint/compaction duration；
- handle/thread/device/temp/view/cow/cache/pin/waiter cleanup；
- `task43_complete`、`milestone_eligible` 和 limitations。

## 测试和完成条件

- verifier mutation tests 必须先 RED，再覆盖每个 fail-closed gate；
- route tests 证明缺参 `BLOCKED/77`、Quick 不夸大、Formal 输入严格绑定；
- C++ focused tests 和 `m3-v7` Quick CTest 通过；
- Formal route 在最终 commit 上通过；
- Formal 后重跑同 commit 的 RealFunctional 和 Crash Full，并由 verifier 重新绑定；
- 任何 required SKIP、阈值超限、不可复现 seed 或资源残留均为 NO-GO。

