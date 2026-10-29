# STAB-04 CI 一键全量：**PARTIAL**——一条命令跑完 160，但尚未全绿

**结论**：一条命令（`tools/m5vm/Invoke-M5FullSuite.ps1`）在干净客机上跑完 **160/160** 个用例，
输出机器可读 `results.json`、人读 `summary.txt`、总时长；请求队列隔离、`results.json` 原子写、
`vswhere` 缺失 SKIP 都已落地并有读数。**但 STAB-04 的验收未达成**：本轮 **155 passed / 2 skipped /
3 failed**，两个失败源（一个 model 用例、powerloss 两段）尚未关闭，且“连续两次结果一致”还没做。

**第二次一键读数（客机，Release，`Reset-M5Guest` 后）**：

```text
M5 full suite: 160 cases, 155 passed, 2 skipped, 3 failed in 1065.9 s
  unit                96 passed   0 skipped   0 failed     231 s
  model               14 passed   0 skipped   1 failed   103.4 s
  integration.m1m4     5 passed   0 skipped   0 failed     6.9 s
  integration.m2       4 passed   0 skipped   0 failed    17.8 s
  integration.m3      31 passed   0 skipped   0 failed      85 s
  integration.m5       4 passed   0 skipped   0 failed     138 s
  structural           1 passed   2 skipped   0 failed     5.2 s
  powerloss            0 passed   0 skipped   2 failed   438.2 s
```

结果：`C:\tmp\m5-vm\full-suite\20261029-235529\results.json`（含每个用例的 group/status/exit_code/
seconds）与同目录 `summary.txt`。

## 1. 已实现并有读数

| 要求 | 实现 | 读数 |
|---|---|---|
| 启动前隔离请求队列 | 宿主在触发前写 `C:\m5\control\pending.id`；agent 只处理该 id（其余记录 `skipping ...`），处理完删除 | 16 次分组触发全部只跑本次请求；此前“160 个请求排旧队列后面”的卡死不再出现 |
| `results.json` 原子写、写坏明确失败 | 客机写 `.tmp` → rename → 读回校验条目数；宿主拒绝 0 字节/非 JSON/空结果并抛错 | 第二次运行 8 个分组的结果全部成功读回；guest 侧第一版校验把 96 条读成 1 条，已修（见 §4） |
| `vswhere` 缺失 → SKIP | 两个 structural 脚本在调用 `Enter-MsvcEnvironment.ps1` 前检查 `vswhere.exe`，缺失打印 `SKIP: vswhere.exe not found ...` 并 `exit 77`；ctest 加 `SKIP_RETURN_CODE 77`；guest runner 与宿主摘要都单列 skip | `structural.m2.storage_independent_build`、`structural.m5.execution_boundary` 都是 `SKIP exit=77`，不再计 FAIL |
| 一条命令、内部分组、powerloss 自动串起 | `Invoke-M5FullSuite.ps1`：unit / model / integration.m1+m4 / m2 / m3 / m5 / structural / powerloss（prepare → vmrun 硬断电 → 启动 → verify） | 160 例全覆盖（第一次运行只覆盖 158，见 §4）；总时长 1065.9 s 记录在 JSON |
| 组间干净 | 每组前通过 agent 执行 `Reset-M5Guest`，并确认输出 `RESET` | 每组前重置成功 |

## 2. 未关闭的两个失败源

### 2.1 `m5.production_execution_backend`（model 组，1 个）

- 失败点：`tests/integration/m5_production_execution_backend_tests.cpp:2177`，`reconcile` 的完成态
  不是 `succeeded_clean`。
- 探针：`outcome=0 observed_generation=0 (prior=1 candidate=2)`；同一 correlation 的直接
  `inspect_checkpoint` 却是 `verdict=2 mask=0x0`（可证明）。即“bundle 本身可证明、worker 的
  proof 说不可证明”。
- **不是 STAB-02 的回归**：把工作树切到 `f933def`（STAB-01/02 之前）、`--clean-first` 重建 worker
  与测试、同一客机重跑，**同样失败**。所以这是该客机状态或既有间发问题的复现，需要在干净的
  参考机上再判一次，而不是回滚产品改动。
- 现状：连续 4 次（含两次硬重启后）稳定失败，因此不能被当作“偶发”忽略。

### 2.2 `integration.m5.powerloss_verify`（powerloss 组，2 个）

- `prepare` **通过**：安装 `MyRamDiskService`（`start= auto`）、64 MiB 盘挂 `T:`、generation 2 →
  committed 3、marker 在 3 内、`C:\m5\powerloss-state.txt` 写出、`PREPARED`。
- 硬断电 + 重启后 `verify` 失败：`Wait-ForService 300` 超时，`sc query` 返回 **1060：指定的服务
  未安装**。也就是说：**状态文件跨断电还在，服务注册却没了**。
- 两次一键运行都是同一形态（第一次与第二次相隔一次硬重启）。prepare 与 verify 之间没有重置、
  没有卸载；`Test-M5PowerLoss.ps1` 的断电段只做 `vmrun stop hard` / `start`。
- 这是下一次接手的第一件事：确认是 SCM 注册在硬断电后丢失（注册表未落盘）还是 guest 启动路径
  的问题；在拿到“服务在 boot 后仍存在”的读数之前，不要把 powerloss 计为通过。

## 3. 复现命令

```powershell
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'; $env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
$env:M5VM_VMX='<vmx>'; $env:M5VM_PASSWORD='<vm password>'
tools/m5vm/Invoke-M5FullSuite.ps1            # 一条命令：stage + 全部分组 + powerloss
tools/m5vm/Invoke-M5FullSuite.ps1 -SkipStage # 复用已 stage 的树
tools/m5vm/Invoke-M5FullSuite.ps1 -Only unit,model -SkipStage   # 分组调试
```

`results.json` 的 schema 是 `myrd.m5.full-suite.v1`，顶层有 `started_utc` / `finished_utc` /
`duration_seconds` / `groups[]`（每组 `passed/skipped/failed/seconds/cases[]`）/ `totals` /
`expected_cases`。`summary.txt` 是人读版本。任一失败或结果读不回来时进程以 1 退出。

## 4. 本轮自己踩到并修掉的三个坑（都是“工具在说谎”）

1. **PS 5.1 `ConvertFrom-Json` 的数组是一个对象**：guest 的 `results.json` 回读校验写成
   `@(ConvertFrom-Json ...)`，96 条被数成 1 条，每个分组请求都以“did not round-trip”失败（结果
   文件其实是对的）。改成 `foreach` 枚举。
2. **同一个坑在宿主的分组分配里**：`$allTests` 只有 1 个元素（名字是全部用例拼接），完整性检查
   变成 1 对 1 并通过；于是 `integration.shm` 与 `model.m3.checkpoint_crash` 两个用例没有分组，
   第一次运行只跑了 158 例。改成显式 `foreach` 后 160/160 全覆盖，两个用例已并入
   `integration.m1m4` 与 `model` 组。
3. **powerloss 失败时整个聚合文件丢失**：驱动器在 `Test-M5PowerLoss.ps1` 抛错时直接终止，前 7 组
   的结果永远不会写成 `results.json`。改成捕获并记录该组失败，聚合照常落盘。

## 5. 发布影响

本轮只改 harness（`tools/m5vm/`、`tests/CMakeLists.txt` 的 SKIP 属性、两个 structural 脚本），
没有改产品二进制；manifest 仍需在 H1 收尾时因 STAB-01/02 的产品改动重新冻结。
