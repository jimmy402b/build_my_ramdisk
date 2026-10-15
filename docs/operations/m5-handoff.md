# M5 交接文档：新会话从这里开始

日期：2026-10-14
分支：`feature/m5-lifecycle`，HEAD `e9a4940`（工作树 `.worktrees/m5-lifecycle`）
基线：`main` = `cb3df08`，**必须保持不动**

> 可粘贴的提示词在 **§10**。

---

## 1. 一句话现状

**T1-01（VM 测试回路）已完成并验证**，读数 **144/146**（唯二失败是客机没有 Visual Studio
的环境受阻）。**下一步是 T1-11**（stop 路径补写冻结），实施方案已写定但**尚未实现**。

已完成的两个实质修复：
- `067f9b3` —— 修掉一个 **50% 崩溃的 M2 测试缺陷**（测试钩子在冒充生产不变量），
  修复前 ~20/40 崩溃 → 修复后 **20/20 通过**；
- `8a310bd` —— 建立 VM 回路，并做了**首次稳定性普查**：146 用例 × 5 = 730 次运行，
  **718/730 通过，只有 2 个用例真间发**。

---

## 2. 项目是什么

`myrd`：Windows 上的 RAM disk。物理层用 AIM（Arsenal Image Mounter，`phdskmnt.sys`，
**外部安装、不得重装/替换/升级**）提供代理盘；数据用 VHDX 基础镜像 + journal +
双槽 manifest + generation/checkpoint 模型。

| 层 | 内容 |
|---|---|
| M0 / M1 | 驱动 PoC / 共享内存 proxy |
| M2 | 动态块存储（64 KiB 块，lazy backing view，CLOCK 驱逐，UNMAP→Discard） |
| M3 | 持久化（journal、checkpoint 两阶段 P4/P5、崩溃恢复） |
| M4 | 前台单进程单盘运行时（`DiskRuntime`），已在 VM 上做过真实设备验收 |
| M5 | **当前工作**：Windows 服务 + 每 profile worker + typed IPC + CLI + GUI |

架构铁律：物理分层由 CMake 强制（`myrd_assert_execution_boundary`），只有
`myrd-m5-worker` 能链接 `myrd_m4_run` / `myrd_m3_windows`。

---

## 3. 关键路径

| 用途 | 路径 |
|---|---|
| **实现工作树**（所有代码改动在这里） | `.worktrees\m5-lifecycle` |
| 主检出（只用于 `main`，不要在这里改代码） | `C:\Users\JimmyLin\Desktop\build_my_ramdisk` |
| 构建目录 | `out\build\msvc`（Ninja Multi-Config，MSVC 14.44） |
| VM 回路 | `tools\m5vm\Invoke-M5Vm.ps1` |
| 回路运维文档（三条通道、一次性配置、**11 个已修的坑**） | `docs\operations\m5-vm-harness.md` |
| 当前读数与失败分类 | `docs\validation\m5-current-state.md` |
| **两个间发缺陷的完整调查**（含"为什么前四层能过"） | `docs\validation\m5-intermittent-findings.md` |
| 增量清单与状态 | `docs\superpowers\plans\2026-10-14-m5-v3-tasklist.md` |
| 设计（v3） | `docs\superpowers\specs\2026-10-14-m5-service-cli-gui-design-v3.md` |
| T1-11 实施方案 | `docs\superpowers\plans\2026-10-14-t1-11-stop-write-freeze.md` |
| 无调试器时读崩溃转储 | `tools\m5vm\read-minidump.py` |

---

## 4. 构建与测试循环

```powershell
# 构建（改过任何头文件就必须加 --clean-first，见 §5）
cmake --build out/build/msvc --config Release            # 或 --clean-first

# VM 回路
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'
$env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
tools/m5vm/Invoke-M5Vm.ps1 -Probe                                   # 环境体检
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release            # 送进 VM
tools/m5vm/Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 180              # 跑整套
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^unit\.m2\.backing_generation$' -Repeat 20
tools/m5vm/Invoke-M5Vm.ps1 -Run -Repeat 5 -RepeatMode Sweep         # 5 遍整套
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '<x>' -TraceTestCases       # 逐用例 stderr 追踪
tools/m5vm/Invoke-M5Vm.ps1 -Run -RequestId <id>                     # 收集已提交的请求
```

- **`-Stage` 是必需的**：二进制在开发机构建，**执行只在 VM**。
- 整套一遍约 9–11 分钟；146 用例跑 5 次约 49 分钟。
- `-MinTimeoutSeconds` 是**下限**（只抬不降）：客机比 ctest 预算所假设的机器慢，
  4 个 verifier 用例的 30/60 秒预算在客机上不够。
- **host 侧等待超时不会中断 guest 的运行**：报错会给出 `-Run -RequestId <id>` 的收集命令。

---

## 5. 已确立的规则（不要重新学一遍）

| # | 规则 | 为什么 |
|---|---|---|
| 1 | **改头文件 ⇒ 测量前 `--clean-first`** | 增量构建不重编所有包含该头文件的 TU → ABI 错配 → `0xC0000005` 假崩溃。transfer manifest §7.17，**已两次现身**（我本人刚被咬一次：20/20 访问违例） |
| 2 | **单次绿是弱证据** | `unit.m2.backing_generation` 在开发机上 20/20 绿、在 VM 上 ~50% 崩溃。任何"绿"的声明**必须写明跑了几次** |
| 3 | **修复类结论至少 `-Repeat 20`** | 25% 和 70% 的缺陷，单次跑显示绿的概率分别是 75% 和 30% |
| 4 | 开发机只跑**不创建真实 worker / AIM 盘 / Windows 卷**的测试 | AGENTS.md §4；否则孤儿 worker 会占着设备，开发机存储栈会退化到重启 |
| 5 | 脚本一律传 `myrd-vm` 别名，**不硬编码地址** | 地址将来若变，脚本不用改 |
| 6 | 仓库脚本 **ASCII-only** | PowerShell 5.1 把无 BOM UTF-8 当 ANSI，会把解析搞坏 |
| 7 | 经 SSH 跑 PowerShell 用 **`-EncodedCommand`**（base64/UTF-16LE） | `ssh host "<命令>"` 走 `cmd.exe`，引号会被反复吃掉（我在这上面浪费了很多轮） |
| 8 | `@(... \| ConvertFrom-Json)` 会把 JSON 数组**嵌套成单元素** | 用显式 `foreach` 循环归一化 |
| 9 | `-join` 不能当 `Select-Object` 的参数 | 先括号收口：`($x \| Select-Object -First 5) -join ', '` |
| 10 | 硬崩溃会**吞掉全缓冲 stdout** | 已给 `test_main.cpp` 加 flush + `MYRD_TEST_TRACE` 追踪 + `set_terminate` 上报 |
| 11 | 崩溃先配 WER `LocalDumps`，再复现 | 无调试器时，转储里的 `__fastfail` **原因码**就能定性（见 `read-minidump.py`） |
| 12 | 无介质光驱上的 `std::filesystem::exists()` **会抛异常** | 客机的 `D:` 曾是光驱；`startConnected=FALSE` 会造出"无介质"状态。已整个移除该设备 |

---

## 6. 当前读数与它的含义

```
144 / 146 passed
  structural.m2.storage_independent_build   ← 客机无 Visual Studio（环境）
  structural.m5.execution_boundary          ← 同上（环境）

稳定性普查（Consecutive，730 次）：718 / 730
  从未全绿的只有 4 个：上述 2 个 + 下面 2 个
  m5.production_execution_backend   0,0,1,0,0   ← 真间发，根因已定位（T1-11）
  unit.m2.backing_generation        0,0,0,0,崩 ← 真间发，已修（067f9b3）
  其余 144 个用例 5/5 全绿
```

**未做**：`Sweep` 顺序的对照普查（连跑可能自我干扰）。若两轮结果不一致，以 Sweep 为准。

---

## 7. 下一步：T1-11（方案已定，未实现）

见 `docs/superpowers/plans/2026-10-14-t1-11-stop-write-freeze.md`。要点：

- **缺陷**：`disk stop` 实测 **25%（5/20）** 在 `remove_mount` 被拒
  （`The worker refused to remove: its runtime stands dirty`），
  因为 `quiesce_mutations` **只证明不冻结**（`service_worker_control.cpp:492-495` 自述
  "holds no write freeze to take"），提交之后仍挂载的卷还能被写入。
- **M4 是对的**：`ShutdownHooks` 的顺序是 `lock_volume → verify → flush → final_checkpoint
  → dismount`——**先锁卷再提交**。
- **关键细节**：锁已经存在（`runtime_adapter.cpp:443`），但**太晚（提交之后）太短
  （函数返回即释放）**；而 `DiskRuntimeOperations` **没有 `lock_volume` 钩子**。
- **最大风险**：`FSCTL_LOCK_VOLUME` 非重入。**第一步必须先写"已持锁时再 lock"的重入测试**，
  通过之后才动 `dismount()`。
- **验收**：`-Repeat 20` 要求 **20/20**（修复前 5/20 失败）。

---

## 8. 未解 / 待查（不要当成已解释）

1. **`set_terminate` 处理器没有触发**：那个守卫调的是 `std::terminate()`，而我的处理器
   从未打印。修复后守卫不再触发，所以不影响结论；但可能意味着**静态/动态 STL 混链**
   （两套 terminate 指针）。检查很便宜：`dumpbin /directives` 对比 `myrd_storage.lib`
   与测试 exe 的 `/DEFAULTLIB`。
2. **测试硬编码开发机绝对路径**（`MYRD_M3_*_FIXTURE_PATH`、`MYRD_PRODUCT_EXE_PATH`）：
   现靠"在客机上镜像开发机路径布局"绕过。长期正解是改测试从环境变量取。
3. **两个 structural 用例需要 Visual Studio**：已归类为环境受限并接受；
   真要 146/146 就得给客机装 Build Tools，性价比不划算。
4. **设计未决项**（需要用户裁决）：Primo 的 "Mini Tools" 定义；T4-02 单文件 VHDX 导出是否保留。
5. **§7.17 那个 ABI 陷阱**仍未被根治：项目没有可靠的头文件依赖跟踪，只靠"记得加
   `--clean-first`"这条纪律。

---

## 9. 本地一次性测试凭据（仓库若要共享，请先删除本节）

| 项 | 值 |
|---|---|
| 虚拟机 | VMware Workstation，`Windows 11 x64`，MAC `00:0C:29:02:90:C1` |
| VMX | `C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx` |
| **VM 加密密码**（每条 vmrun 都要 `-vp`） | `12345678`（`123` / `111` 都不对） |
| 客机账户 | `usr` / `123` |
| SSH 密钥 | `~\.ssh\id_ed25519`，别名 `myrd-vm` → `192.168.180.129` |
| 客机主机密钥指纹 | `SHA256:VnBT/Zzkxj+hxwIBUsdgFOyS0oKkZD/MrRZ7P3axKIE` |
| 驱动 | `phdskmnt` 必须 Running |

VM 是**加密的**（vTPM，Win11 必需），所以 vmrun 必须带 `-vp`；且 **`-T ws` 必须是
vmrun 的第一个参数**，否则 `-gu/-gp` 被忽略。

---

## 10. 提示词（新会话直接粘贴）

```
继续 myrd 项目的 M5 实现。

工作目录 C:\Users\JimmyLin\Desktop\build_my_ramdisk
实现在工作树 .worktrees\m5-lifecycle（HEAD e9a4940）；main 必须保持在 cb3df08 不动。

动手之前先读这四份：
1. docs\operations\m5-handoff.md            —— 交接文档（环境、规则、待办）
2. docs\superpowers\plans\2026-10-14-m5-v3-tasklist.md        —— 增量清单与状态
3. docs\validation\m5-intermittent-findings.md                —— 两个间发缺陷的完整调查
4. docs\superpowers\plans\2026-10-14-t1-11-stop-write-freeze.md  —— 当前任务的实施方案

当前任务：T1-11（stop 路径补写冻结）。严格按方案第 2 节执行，并且
第一步是先写"已持锁时再 lock"的重入测试，通过之后才动 dismount()。
验收用 tools\m5vm\Invoke-M5Vm.ps1 -Run -Filter '^m5\.production_execution_backend$'
-Repeat 20，要求 20/20（修复前实测 5/20 失败）。

硬性规则：
- 改任何头文件之后、测量之前必须
  cmake --build out/build/msvc --config Release --clean-first
  （否则会得到 0xC0000005 的假崩溃，已咬过两次）。
- 开发机上只跑不创建真实 worker / AIM 盘 / Windows 卷的测试；其余一律在 VM 上跑。
- 不接受单次绿：修复类结论至少 -Repeat 20，任何"绿"的声明必须写明跑了几次。
- 每个增量收尾：干净构建 → VM 端到端 → 证据写入 docs\validation\ → 提交到分支。
- VM 用 ssh 别名 myrd-vm 与 tools\m5vm\Invoke-M5Vm.ps1 驱动，不硬编码地址；
  经 SSH 跑 PowerShell 用 -EncodedCommand，避免引号被 cmd 吃掉。
- 只在设计未决项、验收声明变更、或需要重写某一层并重挣 M3/M4 证据时停下来问我。

T1-11 完成后按 tasklist 顺序继续 T1-02 → T4-04。
```
