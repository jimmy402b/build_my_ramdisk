继续 build_my_ramdisk 的 M5 实现。Task 0–5 已完成（Task 5 是"范围闭合"，不是产出完成——
见下），Task 6 尚未开始。现在要做的是 **create_bundle 私有协议命令**。

# 环境与约束

项目根：c:\Users\JimmyLin\Desktop\build_my_ramdisk
只在 worktree 里改代码：`.worktrees/m5-lifecycle`，分支 `feature/m5-lifecycle`。
main 必须保持为干净的 M4 基线（cb3df08），不要在上面提交。

项目根的 AGENTS.md 是最高优先级约束：

- 只用 deepseek-flash 模型；
- **禁止任何子代理**（不用 Agent 工具、不用 Workflow、不用 superpowers:subagent-driven-development）；
- 除非必须我决策，否则持续工作到所有 task 完成、进入验收阶段；
- 用 superpowers:executing-plans 单代理逐任务执行，用 TodoWrite 跟踪。

bash 的 cwd 会在调用之间重置——每次都显式 cd，或用 `git -C`。这个坑我已经撞过好几次。

# 权威文档（按顺序读）

1. `docs/validation/m5-transfer-manifest.md` —— **最高优先**。§1.7 起是逐增量的证据与偏离，
   §7 是全部偏离记录（item 17 = 构建坑，item 18–21 = 关键裁决，item 22 = bundle 证据定案）。
   本文件是"事实来源"，比任何计划文本都新。
2. `docs/superpowers/plans/2026-10-02-m5-inspection-private-ipc-solution.md` —— 覆盖更早的计划文本。
3. `docs/superpowers/plans/2026-09-30-m5-lifecycle-implementation.md` —— 实施计划 + 文件所有权表。
4. `docs/superpowers/plans/2026-09-30-m5-lifecycle-tasklist.md` —— 任务状态表。

**偏离已经发生，不要"纠正"回计划原文。** 计划里若干条目已被裁决推翻（例如
`windows_worker_recovery` 归属、`load_persisted_worker_status` 删除、若干无法实现的动作），
manifest §7 记的是最终结论。

# 当前状态

分支 `feature/m5-lifecycle`，HEAD = `a2e75d8`，工作树干净（`AGENTS.md` 故意 untracked）。
main 仍是 `cb3df08`。套件共 144 个测试，**143 通过，唯一失败是 `integration.m3.recovery`**
（既有缺陷，见下）。

最近提交（新→旧）：

```
a2e75d8 docs(m5): design the create_bundle private command
e89e3de docs(m5): the end-to-end attempt found that nothing creates a bundle
08cf7c2 docs(m5): the end-to-end lifecycle belongs on this host, not the VM
70b2cbf docs(m5): Task 5's scope is closed, its outputs are not
c4630b4 feat(m5): carry a recovery proof in the durable correlation
14d70e5 docs(m5): close Task 5 with the three scope rulings recorded
```

## Task 5 的真实状态：范围闭合，产出未完成

**不要说"Task 5 完成"**——我上一轮这么说过，被用户正确地追问了。准确的分法是：

- **闭合的是范围**：计划里每个文件要么存在、要么被明确重新归属。
- **未闭合的是软件**：三个功能缺口 + 大片从未执行过的代码。

三个功能缺口：

1. `create_and_verify_bundle` / `inspect_bundle_read_only` 在 adapter 里 fail-closed；
2. `attach` 恒返回 `nullopt`（等 Task 7 的活 worker 注册表），所以 `recover_bundle` 不通；
3. **worker 的 `start()` 在任何地方都没成功跑过**——这是本轮要解决的那个，见下。

"从未执行"的部分：adapter 的全部正面路径（`quiesce_mutations`、`flush_exact_volume`、
`checkpoint_across_boundary`、`remove_mount`、`confirm_removal`、`verify_startup_step`、
`record_volume_identity`、`resume_mutations`、`bind_worker` 与 `reconcile_checkpoint` 的成功分支），
以及生产侧数据面（`capture_bundle_evidence`、`fill_persistent_identity`、
`DiskRuntime::adopted_volume`）。模型测试用的是它自己的 fake IWorkerControl。

# 本轮任务：create_bundle 私有协议命令

设计与理由已定案，写在 manifest **§1.26**，实现前先完整读那一节。要点：

**为什么可行**：`create_and_verify_bundle` 排在 `publish_provisioning_profile` 之前，那一刻没有
durable profile，而 `bind_worker` 要求它才能启动。但 `WorkerActionRequest` 携带
`lifecycle_intent`，`LifecycleOperationIntent` 里有 `profile_id`、`expected_image_id`、
`bundle_path`、`mount_target`、`requested_capacity_bytes`——launch spec 需要的全在。
**所以计划里的顺序不用动。**

**为什么是"create 并 provision"一条命令，而不是只 create**：`DiskRuntime::create` +
`start()` 本来就一次做完 generation-zero → AIM start → provision → verify → flush → checkpoint，
这是运行时唯一提供的粒度。而"只创建 generation-zero、后续 start 再 provision"**行不通**——
没有 volume checkpoint 的 bundle 无法被 `recover`，这正是上一轮端到端撞到的缺陷。
**create 和 provision 在这个运行时里是一个操作，协议就该说一个操作。**

**回复不新增载荷类型**：`WorkerRuntimeStatus` 已经携带 `WorkerBundleEvidence`（manifest §1.18），
所以应答就是"带着 bundle 证据的 status"，形状表只多一个 bit。

**改动清单（按顺序）**：

1. launch spec 带上选择模式的字段。profile 的 lifecycle 本来就区分"新盘"和"采纳的盘"，
   而 `state_machine.cpp` 的 `append_start_actions` 恰好就是按它分支的——照抄那个判据。
2. worker runtime 记住模式，`start()` 据此选 `DiskRuntime::create` 或 `::recover`。
   现在是硬编码的 `DiskRuntime::recover`（`src/host/m5_worker_runtime.cpp`）。
3. worker 命令循环加 `create_bundle`（`src/host/m5_worker_session.{h,cpp}` + 协议
   `src/host/m5_worker_protocol.{h,cpp}`）。
4. adapter 的 `create_and_verify_bundle` 变为：从 intent 构造 launch → 发 `create_bundle`
   → 把返回的证据映射成 `BundleRegistrationEvidence`（注意区分 source：
   `created_and_verified` vs `read_only_inspection`）。`inspect_bundle_read_only` 走同一条路
   （import 没有 provisioning 要做）。这要动 `src/management/service_worker_control.{h,cpp}`
   的 `IWorkerSessionProvider::launch` 签名——目前它收 `const profile::ProfileRecord&`，
   需要能接受从 `LifecycleOperationIntent` 构造的 spec。
5. **把 manifest §1.25 里那个端到端用例加回来**，它就是本轮的验收：真 generation-zero bundle
   → 取空闲盘符 → launch → `start` → `require_runtime_observation` → `cleanup`。
   它上一轮被移除是因为框架没有 skip、而断言当时的失败等于把缺陷固化成期望。

# 工作方式

每个增量：测试先行 → **清 .obj** → 构建 → 跑测试 → 全量套件 → 更新 manifest 的证据与偏离
→ 提交 → 下一个。提交信息写清"为什么"，偏离处说明原因。

**如实报告**：没验证的就说不确定，验证了就直说通过。不要把预测写成结果——
写成"预测 X，待验证"是可行的，写成"已验证 X"则不可。这一路我已经因为过度声称被纠正过两次。

遇到必须我拍板的事（架构/范围/安全语义）才停下来问我，其余继续推。

# 已知坑（按踩到的概率排序）

**1. 改了头文件必须清 `.obj`，否则构建结果是假的。**
`out/build/msvc` 的 ninja 不记录头文件依赖。规则：改任何头文件后先
`find out/build/msvc -name '*.obj' -delete` 再重编。症状是 **ABI 错配**而非编译错误：
`0xC0000409` CRT abort、`ok()` 返回垃圾值、段错误无输出、或"已经修好的测试仍然失败"。
**我上一轮又踩了一次**（改了 `service_core.h` 没清），被测试抓住。这条不是建议。

**2. 测试框架在首个失败处 `std::_Exit(1)`，经管道缓冲会丢。**
断言失败时看不到真实错误码。定位方法是加临时
`std::fprintf(stderr, "op=%ls msg=%ls native=%u\n", e.operation.c_str(), e.message.c_str(), e.native_code);`
**定位后记得撤掉探针。**

**3. `product::Uuid` 没有公开默认构造，也没有 from_bytes。**
含它的结构体**不能值初始化**（`T x{};` 编译不过），局部变量也不能默认构造。
做法是聚合初始化，或先 `parse` 出 `Result` 再用 `.value()`。`parse()` 只接受 36 字符规范小写形式。
涉及类型：`WorkerBinding`、`WorkerActionRequest`、`LifecycleOperationIntent`、`Profile`、
`HeldCandidate`、`LaunchedWorker`、`WorkerRecoveryCorrelation`、`VolumeRegistrationEvidence`。

**4. 花括号初始化会选中 `initializer_list` 构造函数。**
`std::wstring mount{1, letter}` 不是"1 个 letter"，而是两个字符（`\x01` 加 letter）。
症状是错误信息与真实原因毫无关系。用圆括号。

**5. 本机有管理员权限，不需要 UAC 提权那一套了。**
`ctest` 直接跑即可。**但**有个反直觉的特例：`integration.m3.recovery` 在
`SeBackupPrivilege` **启用**时**失败**、禁用时**通过**——它用一个共享模式为 0 的独占句柄占住
`.vhdx`，而带 backup 语义打开会绕过共享检查（恢复路径正是这么打开的，见
`image_paths.cpp:549/613/1495`）。**这是既有的测试缺陷，不是回归，也不是你改坏的。**
验证命令：`C:\Windows\System32\whoami.exe /priv`（MSYS 的 `whoami` 会遮蔽它且不接受 `/priv`）。
链接图论证也独立成立：`myrd_m3_recovery_tests` 只链接 `myrd_test_support`（仅 `test_main.cpp`）
和 `myrd_persistence`（`myrd_storage/common/bcrypt`）。

**6. 构建与测试的调用方式。**
构建：`powershell -NoProfile -ExecutionPolicy Bypass -File c:/tmp/m5-wt.ps1 -Command "<cmd>"`
（已设好 MSVC 环境 + 切到 worktree）。配置 `cmake --preset msvc`，构建
`cmake --build out/build/msvc --config Release --target <t>`，
测试 `ctest --test-dir out/build/msvc -C Release -R <regex>`。全量构建约 5 分钟，套件约 4 分钟。

**7. 构建输出是 UTF-16 且含大量 `/showIncludes` 噪音。**
用 `iconv -f UTF-16 -t UTF-8` 转换后 grep `"error C|error LNK"`。

**8. 编译警告即错误**（`myrd_enable_warnings`）。未使用的参数、隐式转换都会挡住构建。

# 验收虚拟机（本阶段用不到，进入验收时才需要）

`phdskmnt` 在**本机**已经 RUNNING（`sc query phdskmnt` 可验证），
`integration.m3.windows_acceptance` 里 `M3AimRuntime::start()` 真的成功。
所以**执行层（AIM 代理盘、真卷、挂载）在本机就能驱动**，不需要虚拟机。

虚拟机是给**验收阶段**用的——把 `myrd` 产品命令打到装好的**服务**上，那是另一层。
连接方式：PuTTY 的 `plink`/`pscp`（`ssh.exe` 无法非交互传密码）。
两个坑：公控管道拒绝 Network Logon（SDDL 带 `(D;;GA;;;NU)`），所以 `myrd.exe` 经 SSH 跑
永远答"supervisor service is unreachable"，必须在**客机交互式桌面**里跑 `C:\m5\runner.ps1`
并保持开着；客机 IP 会变（靠 MAC `00:0c:29:02:90:c1` 找），主机密钥也换过至少一次——
用之前必须重新核对指纹，那是信任决定不是走过场。

# 施工约束（不要违反）

- `myrd_assert_execution_boundary` 禁止 `myrd_management` 链接 `myrd_m4_run` / `myrd_m3_windows` /
  `myrd_m3_management` / `myrd_m1` / `myrd_driver` / `myrd_proxy`。
  `myrd_persistence` 和 `myrd_m5_worker_protocol` **不在**禁止名单。
  这条在 `structural.m5.execution_boundary` 里有测试，每次全量都会跑到。
- `myrd-m5-worker` 是唯一允许链接 `myrd_m4_run` / `myrd_m3_windows` 的目标，
  所以它刻意不调用那个检查函数。
- **安全硬约束**：不得安装/替换/升级/卸载 `phdskmnt`；`disk remove/forget` 无法证明
  （writer lock / 设备 / bundle 身份）安全时**必须拒绝**、保留 registration 和 bundle、
  **绝不伪报成功**；worker replacement 不得触发 removal 或 synthetic checkpoint success。

# 本会话确立、后续必须遵守的约定

- 协议的"形状表"：载荷只在应答上；binding 与 operation id 是**信封**不是载荷；
  operation 作用域的命令在形状表里把 `kHasOperationId` 放进 required。
- `ServiceWorkerSession::exchange` 在**写出去之后**就推进序号，不是在应答被接受后。
- `cleanup` 与 `shutdown` 不是同义词；`shutdown` 后还能 `cleanup` 在协议上不成立。
- `M3Provider::inspect_checkpoint` 是 static（调用者是还没打开 bundle 的新 worker）。
- 共享字节编解码原语在 `src/host/m5_worker_bytes.h`，不要再写第二份小端读写器。
- **一个失败记录类型服务多处**：`WorkerPersistenceFailure`（commit 用；abort/removal 目前
  各自带 `WorkerAbortRefusal` / `WorkerRemovalRefusal` 枚举，因为底层没有可转发的上下文）。
- 每次做"值从哪来"的核对，而不是只问"它在哪停"——这个习惯已经救过三次
  （commit disposition 的第二处折叠、volume identity 无生产者、durable correlation 缺字段）。
