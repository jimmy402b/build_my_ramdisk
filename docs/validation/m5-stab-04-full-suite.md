# STAB-04 CI 一键全量：PASSED——一条命令、160/160、连续两次一致

**结论**：在干净客机上，`tools/m5vm/Invoke-M5FullSuite.ps1` 一条命令跑完 **160/160** 个用例
（内部按 8 组串行，组间 `Reset-M5Guest`），输出机器可读 `results.json` + 人读 `summary.txt` + 总时长；
**连续两次逐用例状态完全一致（0 处差异）**，**158 passed / 2 skipped / 0 failed**；两个 `vswhere`
用例是 SKIP；powerloss 三段（prepare → `vmrun` 硬断电 → 开机 → verify）自动串起。发布证据集已按
本轮二进制重新冻结，validator `accepted`。

## 1. 两次连续读数

| 项 | Run A | Run B |
|---|---|---|
| 用例数 | **160** | **160** |
| passed / skipped / failed | **158 / 2 / 0** | **158 / 2 / 0** |
| 总时长 | **837.5 s** | **829.4 s** |
| results.json | `C:\tmp\m5-vm\full-suite\20261030-015446\results.json` | `C:\tmp\m5-vm\full-suite\20261030-020847\results.json` |
| 逐用例状态差异 | — | **0**（160 个用例名与 status 完全相同） |

分组（两次都是同一形态）：

```text
  unit                96 passed   0 skipped   0 failed   251.8 / 254.3 s
  model               15 passed   0 skipped   0 failed   110.7 / 115.2 s
  integration.m1m4     5 passed   0 skipped   0 failed     7.4 /   7.8 s
  integration.m2       4 passed   0 skipped   0 failed    20.7 /  18.8 s
  integration.m3      31 passed   0 skipped   0 failed    94.9 /  92.5 s
  integration.m5       4 passed   0 skipped   0 failed   165.3 / 159.6 s
  structural           1 passed   2 skipped   0 failed     7.3 /   5.1 s
  powerloss            2 passed   0 skipped   0 failed   135.3 / 133.3 s
```

`structural.m2.storage_independent_build` 与 `structural.m5.execution_boundary` 两次都是
`SKIP exit=77`（客机无 Visual Studio，`vswhere.exe` 不存在），不计 FAIL。

## 2. 两个失败源的根因与修复（本轮真正“跑通”的部分）

### 2.1 `m5.production_execution_backend`：64 MiB 是**下限**，不是上限

- 该用例的盘是 **8 MiB**。上一轮把 worker 的 `max_virtual_bytes` 从常量 64 MiB 改成“worker 自己的
  `virtual_bytes`”，于是 8 MiB 盘的上限被降到 8 MiB；直接 inspection（硬编码 64 MiB）证明得了的
  checkpoint，worker 的 proof 却说 `outcome=ownership_unprovable`。
- 修复：`max(64 MiB, virtual_bytes)` —— 旧常量本来就是下限，小盘保持旧行为，大盘才提高。
  `src/host/m5_worker_runtime.cpp` `default_recovery_options()`。
- 客机单例：`m5.production_execution_backend` **1/1 通过**；随后两次全量 model 组 **15/15**。

### 2.2 powerloss verify：SCM 注册在硬断电时丢了

- 形态：prepare 通过（服务安装为 `start= auto`、64 MiB 盘 `T:`、committed generation 3、
  `powerloss-state.txt` 写出、`PREPARED`）；硬断电重启后 `sc query MyRamDiskService` 返回
  **1060（服务未安装）**，`Wait-ForService 300` 超时；同一时刻写入的文件却还在。
- 对照实验：在空闲客机上用同一 `myrd-service.exe` 建一个 auto 服务，60 s 后硬断电重启，服务**存活**；
  说明不是“服务注册本身不持久”，而是全量套件负载下 Windows 注册表 lazy writer 在约 55 s 内没有把
  SCM 数据库刷到盘上。
- 修复：`tests/e2e/test-m5-powerloss-prepare.ps1` 在 `PREPARED` 之前对
  `HKLM\SYSTEM\CurrentControlSet\Services\MyRamDiskService` 调 `RegFlushKey`，强制 SYSTEM hive 落盘。
  断电实验测的是产品恢复，不是 Windows 的刷盘时机。
- 客机：`Test-M5PowerLoss.ps1 -SkipStage` 三段 **PASS**；随后两次全量 powerloss 组都是 **2/2**。

## 3. 已实现并有一次/两次读数的要求

| 要求 | 实现 | 读数 |
|---|---|---|
| 启动前隔离请求队列 | 宿主写 `C:\m5\control\pending.id`；agent 只处理该 id，其余 `skipping ...` | 两次全量共 20 次触发，每次都只跑本次请求；不再出现旧请求重放 |
| `results.json` 原子写、写坏明确失败 | 客机 `.tmp` → rename → 回读校验；宿主拒绝 0 字节/非 JSON/空结果并抛错 | 两次全量 8 组结果全部成功读回；负例（96 条读成 1 条）已修 |
| `vswhere` 缺失 → SKIP | 两个 structural 脚本先查 `vswhere.exe`，缺失 `exit 77`；ctest `SKIP_RETURN_CODE 77`；runner/宿主/manifest 都单列 skip | 两次 `1 passed / 2 skipped / 0 failed`；manifest 里 structural 是 `skipped=2 failed=0` |
| 一条命令、内部分组、powerloss 自动 | `Invoke-M5FullSuite.ps1`；组间 reset；powerloss 由 `Test-M5PowerLoss.ps1` 串三段 | 两次 160/160、0 failed、总时长入 JSON |
| 连续两次结果一致 | 逐用例对比 `name+status` | **160/160 相同，0 差异** |

## 4. 发布证据（本轮重新冻结）

`tools/New-M5ReleaseManifest.ps1` 按 Run B 的分组 readings 重新生成，`tools/Verify-M5Evidence.ps1`
→ **`accepted`**（exit 0）：

```text
frozen_at = 2026-10-29T18:23:10Z
myrd-service.exe    1134592  eba14b75…
myrd-cli.exe         188416  2dd2a28d…
myrd-gui.exe         162304  57b599e4…
myrd-m5-worker.exe  1512448  b3226f5e…
readings = 9（unit 96 / model 15 / m1m4 5 / m2 4 / m3 31 / m5 4 / structural 3(skip 2) /
            powerloss prepare 1 + verify 1），全部 failed=0
```

旧的、已被分组读数覆盖的 6 个 results 文件移到 `out/m5-evidence/archive-*`，没有删除。
manifest 生成器同时修正了一处：`status=skip` 的用例不再计入 `failed`（此前 structural 会显示
“2 failed”）。

## 5. 复现

```powershell
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'; $env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
$env:M5VM_VMX='<vmx>'; $env:M5VM_PASSWORD='<vm password>'
tools/m5vm/Invoke-M5FullSuite.ps1 -SkipStage   # 约 14 分钟；输出 results.json + summary.txt
```

`results.json` schema `myrd.m5.full-suite.v1`：`started_utc` / `finished_utc` / `duration_seconds` /
`groups[]`（每组 `passed/skipped/failed/seconds/cases[]`）/ `totals` / `expected_cases`。任一失败、
结果读不回来或用例数不等于 160 时进程以 1 退出。
