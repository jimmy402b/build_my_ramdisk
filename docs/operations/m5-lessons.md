# M5 已付学费清单（动手前扫一遍）

**这份文档只收"已经付过代价、再犯还要再付一次"的条目。**
每条都带读数或现场；没有读数的条目不属于这里。
最后更新：2026-10-25。

---

## 1. 构建与产物：陈旧二进制会伪造出任何结论

| # | 规则 | 为什么（读数） |
|---|---|---|
| 1.1 | **改过任何头文件 ⇒ 测量前 `cmake --build out/build/msvc --config Release --clean-first`** | 增量构建不重编所有包含该头文件的 TU → ABI 错配 → `0xC0000005` 假崩溃。**已咬过三次** |
| 1.2 | **还原源码后必须 `touch` 它（或 `--clean-first`）** | `Move-Item` 还原备份会保留旧时间戳 ⇒ CMake 静默不重编。曾把 `1/1 passed` 误读成"完整套件通过" |
| 1.3 | **核对二进制用间接信号，不要运行它** | 完整套件 ≈ **50–75 s**；被裁剪过的 ≈ **10 s**。耗时是判断"客机上跑的到底是哪个二进制"最省事的信号。**直接在开发机运行设备类测试会真的建 worker 与卷**（已违反过一次硬门） |
| 1.4 | **改 `src/run/provision_volume.ps1` 必须 `touch src/run/run_resources.rc`** | 嵌入式资源不会因脚本变化重编；且 stage 前先在开发机跑 `unit.m4.provision_script`（AST 白名单守卫，`ForEach-Object` / `New-Object` 都不在白名单） |
| 1.5 | **测试清单只在 `-Stage` 刷新** | `tests.json` 来自 `ctest --show-only=json-v1`。加了测试却只跑 `-Run`，客机上就没有那条。**判据是总数**：`staged 152 tests` ↔ `1 of 152`；看到 151 说明清单没刷新 |
| 1.6 | **冷会话构建前先 `Set-ExecutionPolicy -Scope Process Bypass` 再 dot-source `tools/Enter-MsvcEnvironment.ps1`** | 不 dot-source 就没有 `LIB`/`INCLUDE`，链接报 **`LNK1181: 无法打开输入文件 bcrypt.lib`**——看起来像缺 SDK，其实是环境没进来。**每个新会话第一次构建都会撞**（2026-10-26 复现，记在 `m5-review-2026-10-26.md` §3.3）。这条以前只在评审文档里，不在本清单 |

**最贵的一次**：一个陈旧二进制让"枚举会阻塞"这条结论活了整整几轮，
六个假设全部围着它转，真实原因是**走了另一条分支**（见 §7.1）。

---

## 2. 测量纪律

| # | 规则 | 为什么 |
|---|---|---|
| 2.1 | **单次绿是弱证据** | `unit.m2.backing_generation` 在开发机 20/20 绿、在客机 ~50% 崩溃 |
| 2.2 | **修复类结论 ≥ `-Repeat 20`**，且声明必须写明跑了几次 | 25% 和 70% 的缺陷，单次跑显示绿的概率分别是 75% 和 30% |
| 2.3 | **不同命令的结果不一致时，以 `Sweep`（连跑整套）为准** | `Consecutive` 与 `Sweep` 的干扰面不同 |
| 2.4 | **用测得更少的替代测试把增量标成 PASSED，等于拿"看起来绿"换"实际不知道"** | T1-04 曾因此保持 PARTIAL 直到命令面就绪——这是对的 |
| 2.5 | **任何"性能变差/偶发/卡住"的结论，先确认二进制来自当前源码** | 见 §1.3；本项目的第四次同类教训 |

---

## 3. 断言与诊断

**规则**：凡是断言一个 `Result`，就把错误打出来：

```cpp
if (!x.ok()) {
    std::fprintf(stderr, "myrd-test: <what> failed: op=%ls msg=%ls native=%u code=%u",
                 x.error().operation.c_str(), x.error().message.c_str(),
                 x.error().native_code, x.error().code);
    std::fputc(10, stderr);
}
REQUIRE(x.ok());
```

**为什么**：`m5_backend_a_reclaim_dismantles_the_leftover_a_kill_created`
曾连续两轮 0/20、0/3 全红，stderr 只有一行 `reclaimed.ok()`——看不出是 adopt、verify
还是拆卸失败，**每轮白跑一整次客机循环（5–30 分钟）**。这条规则加上去的当轮就回本。

**推论**：先给现象加一条**能区分假设**的诊断，再下结论。不要用"再跑一遍"代替诊断。

---

## 4. 工具链与脚本

| # | 规则 | 为什么 |
|---|---|---|
| 4.1 | **改文本文件一律用 `edit` 工具；不要用 `Set-Content`** | PS 7 的 `-Encoding ASCII` **真的按 ASCII 写**，每个非 ASCII 字符都变成半角问号（实测一次毁掉 368 处）。**已犯三次，属纪律问题**。第二个实例：`m5-t1-05-cli.md` 在 `8a22bbb` 被写成 **UTF-8 → GBK 的双重编码**（336 行正文塌成 222 行、685 处错字），§1–§7 直到 2026-10-25 才从 `87c62f1` 的干净副本恢复 |
| 4.2 | 确需写文件时只能 `utf8NoBOM`（PS7）或 `utf8`（5.1）；改完查有没有出现**连续三个半角问号**（编码被破坏的典型痕迹）与 `git diff --stat` 比例 | 小改动不该有 409/658 那种比例 |
| 4.3 | **仓库脚本 ASCII-only** | PowerShell 5.1 把无 BOM UTF-8 当 ANSI，注释会把解析搞坏 |
| 4.4 | **经 SSH 跑 PowerShell 用 `-EncodedCommand`** | `ssh host "<cmd>"` 走 `cmd.exe`，引号会被反复吃掉 |
| 4.5 | **脚本文件不要带 BOM** | `powershell -Command -` 不剥 BOM，BOM 粘在正文第一个 token 上 ⇒ "无法将该项识别为 cmdlet"。修的时候注意别造出空行让 BOM 落到第二行 |
| 4.6 | `@(... \| ConvertFrom-Json)` 会把 JSON 数组**嵌套成单元素** | 用显式 `foreach` 归一化 |
| 4.7 | `-join` 不能当 `Select-Object` 的参数 | 先括号收口：`($x \| Select-Object -First 5) -join ', '` |
| 4.8 | **`-Stage` 可能先删后用**：上传没落地却仍打印 `staged N` | 症状是 runner 报 `MISSING .../<test>.exe`，**读起来完全像构建问题**。看到 `MISSING` 先怀疑 staging |
| 4.9 | **客机 runner 在 `C:\m5\agent\`，不是 `C:\m5\control\`** | 两者有同名文件；改了 `control` 那份没有任何效果（已白等两轮） |
| 4.10 | `dumpbin /directives` 面向 `.obj`/`.lib`；查已链接 exe 用 **`/dependents`** | 对 exe 用 `/directives` 会得到"没有任何 DEFAULTLIB"，容易被误读成链接配置缺失 |

---

## 5. VM 运维

| # | 规则 | 为什么 |
|---|---|---|
| 5.1 | **杀 host 侧的 `Invoke-M5Vm.ps1` 不会停掉客机上的运行** | 客机上测试继续跑，留下孤儿 worker 占住 AIM 盘 ⇒ `Get-Disk` 多出物理盘、`diskpart rescan` 挂住。要停就停客机上的进程 |
| 5.2 | **恢复客机存储栈的唯一可靠手段是 `vmrun stop <vmx> hard` + `start <vmx> nogui`** | `Set-Disk -IsOffline` / `diskpart` 清残留**会挂住**。硬重启更快更可靠 |
| 5.3 | **污染状态下失败会伪装成产品缺陷** | 同一个二进制：污染时 `m5_backend_creates_a_bundle_and_mounts_it_end_to_end` 失败在 `created.ok()`；干净重启后 13/13 通过 |
| 5.4 | **残留是合法的中间状态，但要有意为之** | 强杀用例**故意留下** `disks: 1,0 / letters: C,Z` 并断言它存在。跑完它之后客机不干净，**下一个依赖干净设备的用例之前必须硬重启** |
| 5.5 | 设备类用例失败前，先确认 `workers: 0 / disks: 0 / letters: C` | 同上 |
| 5.6 | 挂住超过 ~10 分钟就硬重启，**不要等 30 分钟** | 曾出现 >30 分钟无结果同时 SSH 停止响应 |

---

## 6. Windows / 驱动 / 卷的事实（不是猜测）

| # | 事实 | 怎么知道的 |
|---|---|---|
| 6.1 | **卷被本进程无共享独占打开后，同进程第二次打开会失败**，且错误是 `ERROR_NOT_READY(21)` 而不是 `busy` | 重入用例实测。⇒ 冻结必须**保留并向下传递**那把锁，不能"再 lock 一次" |
| 6.2 | **刚 `CloseHandle` 的卷句柄与新的一次无共享打开会撞 `ERROR_SHARING_VIOLATION`**，冲突句柄 **250 ms 内**释放 | 诊断打印 `first=SHARING_VIOLATION after-250ms=OPENED` ⇒ **瞬时竞态，不是句柄泄漏**（我先判成泄漏，错的）。修法：在既有预算内有界重试，耗尽仍返回 `busy` 并带原生码 |
| 6.3 | **打开一个正在退役的设备会阻塞约 11 s** | `CreateFileW took 11017 ms`，且在 `remove_owned_device` 等退役时交错发生。`FILE_FLAG_OVERLAPPED` **无效**：它把已打开句柄的 I/O 变异步，但 create IRP 必须同步完成 |
| 6.4 | **提交会重置 dirty epoch**，所以"提交后 epoch 更小"是 re-baseline，不是回归 | `dbb25a3`。跨会话比较 epoch 是 Project 的既有病根（`dirty_epoch` 只在一个 worker session 内有序） |
| 6.5 | 无介质光驱上 `std::filesystem::exists()` **会抛异常** | 客机 `D:` 曾是光驱。已整个移除该设备 |
| 6.6 | 硬崩溃会**吞掉全缓冲 stdout** | 已给 `test_main.cpp` 加 flush + `MYRD_TEST_TRACE` + `set_terminate` 上报；崩溃先配 WER `LocalDumps`，`__fastfail` 原因码可定性（`tools/m5vm/read-minidump.py`） |

---

## 7. 排查方法论

### 7.1 遇到"这个操作看到了不该看到的东西"，第一问是**它走了哪条分支**

`m5.production_execution_backend` 第 17 条长期失败，**六个假设全被实测推翻**
（盘号错 / 映射太小 / 旧设备复用 / 定位错盘 / 清盘 / 残留盘符）。
真因：那次 create 走的是**采纳（adopt）**路径，采纳时**跳过了 backing name 校验**，
于是采纳了另一个 profile 的设备。它的正当性论证写的是"本运行时持有 bundle 的独占写锁"，
**而这句话只覆盖"本 bundle 的并发持有者"，覆盖不了"另一个 bundle 的持有者"**。

> 六个假设都在追问"环境里有什么"，而答案是"代码走了另一条路"。

### 7.2 `std::async` 的 future **不能用来放弃**任务

`auto w = std::async(...); w.wait_for(t);` 看起来加了界，**实际什么都没改变**——
future 析构会等任务完成。正解是 **detached 线程 + 堆上共享状态**（`make_shared`，线程持副本），
超时即返回、**不 join**。
**两条例外**：① 不要移动一个仍被锁住的 `std::mutex`（同步原语一开始就放堆上）；
② **放弃一个线程只在"被放弃的工作是只读的"时候安全**——拆卸/写入要用 `CancelIoEx` 这类真正取消。

### 7.3 "报错信息相同"不代表"原因相同"

`Invoke-M5Vm.ps1` 的 BOM 问题修了两次才对：第一次加在 `Invoke-Remote`（漏了直接调
`Invoke-Native` 的解包步骤）；第二次移过去却没删第一份 ⇒ 空行变两行、BOM 落到第二行，
**同样的报错、不同的原因**。改工具时要重跑**所有**受影响的调用。

---

## 8. 文档维护

| # | 规则 |
|---|---|
| 8.1 | 状态只写在 `2026-10-14-m5-v3-tasklist.md`；"下一步"只写在 `m5-handoff.md`；master-plan 不写"当前位置" |
| 8.2 | 被取代的文档不要留原文，改成 3 行存根并写明"已并入哪一份"（本仓库已有先例） |
| 8.3 | 改完中文文档先查有没有出现**连续三个半角问号**（编码被破坏的典型痕迹）；再查 `git diff --stat` 的比例是否合理 |
| 8.4 | 一个结论要写进文档，先问"有读数吗"；没有就写"未验证"，不要写成结论 |
