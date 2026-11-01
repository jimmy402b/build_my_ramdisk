# R-13：24 h soak（进行中，试跑已完成）

## 1. 设计

- **guest case** `integration.m5.soak`：一轮 = create 64 MiB 盘（bundle 在 `C:\m5\soak\run-*`）→
  写 16 MiB 伪随机数据并记 SHA-256 → checkpoint → **再写一份不提交的数据** → stop → start
  （adopt）→ 回读已提交文件比对哈希 → stop → forget → 记录读数。
- **跨轮复用同一个 service 进程**：若 `disk list` 已经能答，就直接用；只有 `stop-service` 模式
  或断电才会结束它。这样 handle / 工作集读数描述的是**一个长寿命进程**，不是每轮新建的进程
  （后者无法说明泄漏）。
- **host driver** `tools/m5vm/Test-M5Soak.ps1`：reset 客机 → stage 一次 → 循环跑 case → 每
  `-CutEveryRounds` 轮 `vmrun stop … hard` → 重启 → 等 ssh **和** scp → 以 `verify` 模式跑一轮
  （无残留、遗留 profile 可恢复）→ 继续。读数落在宿主机 `soak.log`，失败落 `failures.log`。
- 「不提交的写入」是有意的：断电后下一次 start 必须恢复到**最后一次已提交**的 generation，
  第 4 步的哈希就是判据。

## 2. 试跑（6 轮 + 1 次断电，约 15 min）全绿

```text
round 1: passed=True  workers=1 svc_handle=166 wrk_handle=119 svc_ws=10.1MB wrk_ws=33.7MB generation=4 verified=True
round 2: passed=True  workers=1 svc_handle=166 wrk_handle=119 svc_ws=10.1MB wrk_ws=33.7MB generation=4 verified=True
round 3: passed=True  workers=1 svc_handle=166 wrk_handle=119 svc_ws=10.1MB wrk_ws=33.7MB generation=4 verified=True
round 4: passed=True  workers=1 svc_handle=166 wrk_handle=119 svc_ws=10.1MB wrk_ws=33.7MB generation=4 verified=True
cut 1 → verify: passed=True  post-cut workers=0
round 5: passed=True  workers=1 svc_handle=166 wrk_handle=119 svc_ws=10.0MB wrk_ws=33.7MB generation=4 verified=True
round 6: passed=True  workers=1 svc_handle=166 wrk_handle=119 svc_ws=10.2MB wrk_ws=33.7MB generation=4 verified=True
stop-service: passed=True
soak finished: rounds=6 cuts=1 failures=0
```

试跑结论：每轮数据校验通过、断电后 `workers=0` 且 verify 通过、handle/内存无增长趋势。
（轮数太少，不能据此下“无泄漏”结论；24 h 跑完再判定。）

## 3. 调试中修掉的三个真实问题

1. **`disk forget` 是异步的**：不等终态就删 bundle，会让 forget 的 proof 报
   `image_path.open_root`，profile 残留，下一轮 create 报 “mount target is already registered
   to another disk”。现在等 forget 终态后才删 bundle。
2. **失败 create 留下的 profile 占住 mount target**：driver 在 soak 开始前 reset 客机（
   `profiles: 0 entries left`），case 里也保留一次清理。
3. **driver 读不到 `Write-Host` 输出**：case 把状态与读数写到 `C:\m5\soak\last.txt`，driver
   以该文件为准。

## 4. 24 h 正式跑

- 命令：`tools\m5vm\Test-M5Soak.ps1 -Hours 24 -CutEveryRounds 40 -SkipStage -OutDir C:\tmp\m5-soak\2026-11-01`
- 节奏：约 2 min/轮，每 40 轮（约 80 min）一次硬断电 + verify。
- 判定（跑完后填）：无泄漏（handle/工作集不随轮次单调增长）、内存回基线、每次断电后
  `verify` 通过且 `workers=0`、每轮 `verified=True`。
- 本页在跑完后补 §5 的首末读数与趋势，并把 tasklist 的 STAB-11 从 IN PROGRESS 改为最终状态。
