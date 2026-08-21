# M4 驱动不可用与断电恢复资格验收执行台账

> **唯一执行台账：** M4 的状态变更、实现提交、复盘记录和 VM 验收证据只写入本文件；不回写历史 `tasklist.md`，也不使用任何 M0/M1 台账作为进度来源。

> **执行依据：** [M4 驱动不可用与断电恢复验收重设计 Spec](docs/superpowers/specs/2026-08-20-m4-driver-unavailable-qualification-design.md) → [M4 驱动不可用与断电恢复资格验收实施计划](docs/superpowers/plans/2026-08-21-m4-driver-unavailable-qualification.md) → 本台账。发生冲突时按 `安全不变量 > Spec > Plan > 本台账摘要` 处理，并停止实施直至更新并批准上游文档。

> 状态：待实施。旧 kit、旧资格根目录和旧协议只可用于诊断，不得迁移、兼容、覆盖或作为通过证据。

## M4-0. 执行规则

1. 每次只将一个任务标记为 `IN_PROGRESS`；其前置任务必须是 `ON_TRACK`。
2. 每个任务先完成其 RED 项，再实现最小变更至 GREEN；未经记录的 GREEN 不得作为证据。
3. 每个实现任务完成后必须运行该任务列出的验证，执行 `git diff --check`，并在单独提交中仅记录本台账复盘。
4. `Blocked` 是资格根目录的产品终态；任何测试都必须断言同一资格根不自动重试、不改写状态、不再次触发子进程或 VMware 电源边界。
5. VMware 只能由用户人工操作。VM 正式验收总共恰好三次边界：Formal、Live Power-Cut、Missing-driver；每段选择 Reset 或一次 Power Off→Power On，绝不正常关机。
6. `pnputil.exe /restart-device` 不得进入实现。只有在干净 VM 前置实验已证明 `sc start phdskmnt` 不足时，才可停止本任务表、补充决策与 Spec 后重新规划。该实验即 **M4-PRE**，按 Spec §6「实施前必须」的要求排在 M4-10 之前，由用户执行；M4-PRE 未 `ON_TRACK` 前，M4-10 → M4-14 不得开始。
7. 物理盘号、分区号、PnP 实例号、序列号和显示文本仅能写入诊断；通过条件只能使用当前轮 AIM 所拥有的 `S:` 与 Formal 来源 bundle 最后 checkpoint 的 `A.txt`/`D.txt` 内容哈希。

### M4 状态定义

```text
NOT_STARTED -> IN_PROGRESS -> IMPLEMENTED -> REVIEWING -> ON_TRACK
                                  |                 |
                                  +-> DEVIATED      +-> BLOCKED
                                                    +-> NO_GO_EVIDENCE
```

`DEVIATED`、`BLOCKED`、`NO_GO_EVIDENCE` 均停止后续任务；必须先更新并批准 Spec/Plan，或由用户解除外部阻断。

## M4-0.1 Spec、Plan 与任务的完整对应

| Spec 要求 | Plan 任务 | 本台账任务 | 闭环证据 |
|---|---|---|---|
| §1–§3.1：服务缺失/禁用零 bundle 写入；SCM 其他错误独立报告；提权后预检 | 任务 1 | M4-01、M4-02、M4-03 | 纯函数/SCM seam RED→GREEN、产品边界顺序与字节协议合同 |
| §3.2：`discover_aim_adapter` 是唯一 AIM 权威；query 严格协议 | 任务 2 | M4-04 | discovery/query source、字节与类型 mutation 合同 |
| §4–§5：九负例、`missing_driver_create` 零写入、`driver_lost_recover` 克隆与恢复内容 | 任务 3 | M4-05、M4-06 | runner/fixture 生产形状 evidence、原始字节和目录树 mutation 矩阵 |
| §4、§6、§7：Reset3 顺序、服务恢复、独立阶段证据、精确 cleanup 与 Blocked | 任务 4 | M4-07、M4-08、M4-09、M4-10、M4-11 | production dispatcher fake flow、精确 seam 向量、失败矩阵与无重试断言 |
| §8：只读、离线、可重复复算 verifier | 任务 5 | M4-12 | evidence/hash mutation 矩阵与 AST/命令 allowlist 合同 |
| §2、§7：新协议 kit、manifest/哈希冻结、旧 kit/root 不兼容 | 任务 6 | M4-13 | packager/controller 双侧 manifest mutation 矩阵 |
| §6：`sc config`+`sc start` 是否足以恢复 query 探针（Spec 明文要求「实施前」验证） | 任务 7 中的前置实验 | **M4-PRE**（已按 Spec 前移至 M4-10 之前） | 干净 VM 的独立实验记录 |
| §9–§10：本地 Debug/Release、全新 kit/root、恰好三次人工电源边界 | 任务 7 | M4-00、M4-14、M4-15 | 基线台账、双配置 `m4-v3`、kit hash、VM 三边界与离线 verifier |

本表用于核查覆盖关系，不能替代每项任务中的精确文件、RED/GREEN、命令和完成条件。

## M4-0.2 开放决策与结构性风险

以下条目在关闭前不得被任何任务的「完成」掩盖。决策类条目只能由用户关闭；关闭时必须先更新 Spec/Plan，再回写本节。

### D-1（待决策，阻断 M4-14 的协议声明）— 冻结协议写「绝对 bundle 路径」，产品输出「原样路径」

Spec §3 冻结的 stderr 为：

```text
Error: run failed for <绝对 bundle 路径>: driver_unavailable: phdskmnt is disabled or absent\r\n
```

实测（`out/build/msvc/src/Debug/myrd.exe`，commit `0db6eb9`）：

```text
$ myrd.exe run --bundle docs --size 64M --drive-letter R:
Error: run failed for docs: classify_bundle: non-empty bundle has no disk.rdm
```

`src/run/cli.cpp` 的 `bundle_ok()` 显式允许相对路径（`colon == npos` 即通过），`parse_cli` 不做 canonicalize，`print_run_error` 原样打印 `options.bundle`。该行为对**所有** run 错误一致，不是 M4-03 引入的。

影响：资格 runner/控制器一律传绝对路径，因此正式流程碰不到；但冻结的字节协议与实现不一致，verifier 复算时若出现相对路径将不匹配。

二选一（必须由用户决定，不得由实施方自选）：

1. 修改 Spec §3，把「绝对 bundle 路径」改为「与命令行传入完全一致的 bundle 路径」，并在合同中断言不做 canonicalize；
2. 修改产品，在 `parse_cli` 或 `print_run_error` 处 canonicalize 为绝对路径，并为相对路径输入新增合同用例。

在 D-1 关闭前，M4-14 不得声明「`driver_unavailable` 字节协议已与 Spec 一致」。

### D-2（待决策，阻断 M4-13 的 kit 冻结）— 权威文档未入库

`docs/superpowers/plans/2026-08-21-m4-driver-unavailable-qualification.md` 至今为未跟踪文件，`tasklist.md` 有未提交修改。M4-13 要把 `source_commit` 冻进 kit manifest，但定义本协议的 Plan 文档不在任何 commit 内，kit 因此无法指向完整的权威设计。

在 M4-13 之前必须由用户决定该文档是否入库；实施方不得代为提交用户的未提交改动。

### R-1（结构性风险，影响 M4-07 → M4-11）— 控制器已超出可审阅规模

`tools/Invoke-M4GuestQualification.ps1` 当前为 301,504 字节（PowerShell AST 50,035 tokens）的单文件。M4-07 → M4-11 五个任务全部修改该文件，且 M4-11 还需在其中追加「每个 Reset3 子步骤 × 七负例 × fresh fake root」的完整失败矩阵。

后果：单次实施会话无法完整读取并安全修改该文件；任何一次中断都会留下半改状态。本风险不因任务拆分而消失——拆分反而使同一文件被五次连续重写。

建议在 M4-07 开始前单独立项处理（按状态机、证据 schema、seam 注入分离为多个脚本），或由用户明示接受该风险并说明如何保证可审阅性。

## M4-1. 总进度表

| Task | 状态 | 前置 | 实现提交 | 复盘提交 | 证据/阻断 |
|---|---|---|---|---|---|
| M4-00 | ON_TRACK | — | `c20c269` | `c2e8171` | `docs/validation/m4-driver-unavailable-baseline.md` |
| M4-01 | ON_TRACK | M4-00 | `c9bc81a` | `46e739d` | 预检纯分类 RED |
| M4-02 | ON_TRACK | M4-01 | `d134963` | `714ec09` | SCM 包装与错误映射 |
| M4-03 | ON_TRACK | M4-02 | `def1812` | `b949b44` | `main.cpp` 精确插入点 |
| M4-04 | ON_TRACK | M4-03 | `0db6eb9` | `84c1acf` | discovery/query 协议回归 |
| M4-05 | ON_TRACK | M4-04 | `8bb2410` + `b8a3162` | `9b0a986` | 九负例 manifest 与 fixture |
| M4-06 | ON_TRACK | M4-05 | `15e1c01` | `7c57b54` | 两种驱动缺失 runner evidence |
| M4-07 | ON_TRACK | M4-06 | `212f27e` | `PENDING` | Formal clone 与控制器状态/事件 schema |
| M4-08 | NOT_STARTED | M4-07 | — | — | Reset3 启动门与零副作用 |
| M4-09 | NOT_STARTED | M4-08 | — | — | `missing_driver_create` 控制器路径 |
| M4-PRE | NOT_STARTED | M4-09 | — | — | **用户执行**：`sc start` 恢复探针前置实验（Spec §6 强制先于 M4-10） |
| M4-10 | NOT_STARTED | M4-PRE | — | — | 服务恢复、query 与真实副本恢复 |
| M4-11 | NOT_STARTED | M4-10 | — | — | 七个其余负例、失败矩阵、Blocked |
| M4-12 | NOT_STARTED | M4-11 | — | — | 纯离线 verifier |
| M4-13 | NOT_STARTED | M4-12 | — | — | 新协议 kit 冻结 |
| M4-14 | NOT_STARTED | M4-13 | — | — | 本地全量验证与发布说明 |
| M4-15 | NOT_STARTED | M4-14（且 M4-PRE 必须 `ON_TRACK`） | — | — | VM 三边界人工验收（前置实验已移至 M4-PRE） |

## M4-2. 细颗粒度执行清单

### M4-00 — 冻结起点、读取约束并建立 M4 证据台账

**范围：** `M4_TASKLIST.md`、`docs/validation/m4-driver-unavailable-*`（新增）

- [x] M4-00.1 阅读 Spec §1–§10、Plan 全文及本节 M4-0；记录当前 `git status --short --branch` 与 `git log -5 --oneline`。
- [x] M4-00.2 记录当前 kit schema、kit 固定文件清单、`myrd.exe`/query SHA-256、控制器/runner/verifier 文件 SHA-256；不生成新 kit。
- [x] M4-00.3 运行只读基线：`powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-guest-qualification.ps1 -SourceRoot .`、`powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-qualification-kit.ps1 -SourceRoot .`、PowerShell AST 解析三份生产脚本。
- [x] M4-00.4 在 `docs/validation/` 写入仅含起始 commit、命令、退出码、文件哈希和“不触碰 VM/驱动/设备”的 M4 基线记录。
- [x] M4-00.5 提交基线记录，复盘文件范围只含该记录和本台账，状态改为 `ON_TRACK`。

### M4-01 — 为服务预检纯分类写出 RED 测试

**范围：** `tests/unit/m4_driver_preflight_tests.cpp`（新增）、`tests/CMakeLists.txt`、`src/run/driver_preflight.h`（仅声明）

- [x] M4-01.1 创建 `m4_driver_preflight_tests.cpp`，先编写 `classify_phdskmnt_service` 的失败测试：服务不存在与 `SERVICE_DISABLED` 返回 `unavailable`。
- [x] M4-01.2 在同一测试添加四种可用启动类型 RED：`SERVICE_BOOT_START`、`SERVICE_SYSTEM_START`、`SERVICE_DEMAND_START`、`SERVICE_AUTO_START` 均返回 `available`。
- [x] M4-01.3 为 SCM 包装定义可替换 I/O seam 的测试输入，加入 `OpenSCManagerW` 失败、`OpenServiceW(ERROR_ACCESS_DENIED)`、`QueryServiceConfigW` 失败三条 RED；每条断言 `ErrorDomain::win32`、具体操作名且渲染文本不含 `driver_unavailable`。
- [x] M4-01.4 在 `tests/CMakeLists.txt` 注册 `unit.m4.driver_preflight`、标签 `m4-v3`、超时 30 秒。
- [x] M4-01.5 构建并运行：`cmake --preset msvc`、`cmake --build --preset debug --parallel 1 --target myrd_m4_driver_preflight_tests`、`ctest --preset debug -R '^unit\.m4\.driver_preflight$' --output-on-failure`；预期失败为缺声明/缺实现。
- [x] M4-01.6 提交仅含 RED 测试与必要声明的变更；复盘 RED 输出和文件范围。

### M4-02 — 实现 SCM 包装及三类预检结果映射

**范围：** `src/run/driver_preflight.h`、`src/run/driver_preflight.cpp`、`src/CMakeLists.txt`、`tests/unit/m4_driver_preflight_tests.cpp`

- [x] M4-02.1 实现纯函数：服务缺失或 `SERVICE_DISABLED` 归为 `unavailable`；四种列出的有效启动类型归为 `available`。
- [x] M4-02.2 实现只含 SCM 访问的包装：`OpenSCManagerW`、`OpenServiceW`、`QueryServiceConfigW`、精确 close handle；不得读取 bundle、枚举 StoragePort、打开 AIM、检查盘符或创建设备。
- [x] M4-02.3 仅将 `ERROR_SERVICE_DOES_NOT_EXIST` 和 `SERVICE_DISABLED` 映射为：`ErrorDomain::usage`、code `1`、`operation=L"driver_unavailable"`、`message=L"phdskmnt is disabled or absent"`。
- [x] M4-02.4 将所有其他 SCM 失败映射为 `ErrorDomain::win32`、原始 `GetLastError()` 和各自操作名 `OpenSCManagerW`/`OpenServiceW`/`QueryServiceConfigW`；禁止借用 `driver_unavailable` operation/message。
- [x] M4-02.5 重跑 `unit.m4.driver_preflight` 到 GREEN，并添加 handle-close/错误优先级断言。
- [x] M4-02.6 提交实现；复盘确认没有 adapter handle、StoragePort、SRB QUERY_VERSION、设备、盘符或 bundle 依赖进入新模块。

### M4-03 — 将预检插入 `myrd run` 的唯一安全位置

**范围：** `src/run/main.cpp`、`src/CMakeLists.txt`、`tests/unit/m4_product_boundary_tests.ps1`

- [x] M4-03.1 在产品边界 PowerShell 合同中写 RED：`require_phdskmnt_service_available()` 必须在 `require_elevation()` 成功分支之后、`classify_bundle()` 之前；Create 与 Recover 不得各自有第二个预检入口。
- [x] M4-03.2 在该合同中写 RED 字节协议断言：已知不可用时 `print_run_error` 的字段必须是 `driver_unavailable` 和 `phdskmnt is disabled or absent`，stdout 空、exit 1、stderr CRLF；SCM I/O 失败 stderr 不得含该 operation。
- [x] M4-03.3 在 `main.cpp` 仅在提权成功后插入预检；失败时调用既有 `print_run_error(options.bundle, error)` 并 `return 1`，然后才调用 `classify_bundle()`。
- [x] M4-03.4 确认预检之前不读取/创建/修改/挂载 bundle；在合同中用源码位置检查覆盖 `classify_bundle`、`create_image`、recover、provider start。
- [x] M4-03.5 运行 `ctest --preset debug -R '^unit\.m4\.(driver_preflight|product_boundary)$' --output-on-failure`，预期全绿。
- [x] M4-03.6 提交实现并复盘错误优先级：未提权时仍优先报告 `require_elevation`，不会报告服务状态。

### M4-04 — 冻结 discovery 唯一权威与 query 字节协议

**范围：** `src/driver/adapter_discovery.cpp`、`src/m3/m3_windows_acceptance.cpp`、`tests/e2e/m4_aim_query.cpp`、`tests/unit/m4_product_boundary_tests.ps1`、对应 C++ 单元测试

- [x] M4-04.1 写 RED 源码合同：`discover_aim_adapter` 继续使用 `GUID_DEVICEINTERFACE_STORAGEPORT`、`SPDRP_SERVICE == phdskmnt`、既有 QUERY_VERSION 与唯一候选拒绝规则。
- [x] M4-04.2 写 RED 合同，禁止新预检或控制器使用 `FriendlyName`、Class、PnP 实例号、序列号或 PhysicalDrive 作为 adapter 匹配权威。
- [x] M4-04.3 在 query 测试冻结失败协议：exit 1、stdout 空、stderr 精确 `discover_aim_adapter failed\r\n`。
- [~] M4-04.4 **部分完成（发射端已冻结，拒收端移交 M4-10.7）**。已完成：`unit.m4.aim_query_protocol` 对 query 成功 JSON 的**发射**逐字节断言（单行、无 BOM、恰好一个末尾换行、`success` 为不带引号的 `true`、版本为不带引号数值、`ids` 为裸数值数组、uint32 全量程）。**未完成**：「BOM、多 JSON、错误 CRLF、额外/错误类型均**失败**」需要一个严格**拒收方**（parser），而消费 query 输出的是控制器；按 M4-0.1 的职责映射，该拒收矩阵属控制器任务，已移交 M4-10.7。在 M4-10.7 完成前，不得声称 query 严格协议已双向闭环。
- [x] M4-04.5 验证服务可用但 provider discovery 失败仍采用既有 `discover_aim_adapter: no AIM adapter found`，测试不得要求此场景零 bundle 写入。
- [x] M4-04.6 运行 query/selftest 与相关 unit 到 GREEN，提交并复盘 provider 中 discovery 的位置未前移。

### M4-05 — 将 negative fixture manifest 升级为精确九案

**范围：** `tests/e2e/Prepare-M4NegativeFixtures.ps1`、`tests/e2e/Invoke-M4RunLifecycle.ps1`、`tests/unit/test-m4-guest-qualification.ps1`、`tests/CMakeLists.txt`

**实施前置分析（已完成，供实施者直接使用，勿重新推导）：**

1. **原子性要求（安全约束）**：M4-05 必须在**一次提交内**同时改完 `Prepare-M4NegativeFixtures.ps1`（生产方）与 `Invoke-M4RunLifecycle.ps1`（消费方）。原因：runner 的 schema 闸门在**运行期**读取 fixture 文件（`Invoke-M4RunLifecycle.ps1:355`），而 runner selftest 自建 fixture、不读该文件。不得分两次提交。

   **实施后更正（本条原文有误，勿再引用旧说法）**：原文断言「只改 Prepare 会**通过全部本地测试**」。实测为假——共有**两处本地测试**断言案例清单，会捕获该改动：
   - `tests/unit/test-m4-formal12-support.ps1`（端到端跑 Prepare 并断言 manifest 形状）
   - `tests/integration/m4_product_composition_tests.cpp`（源码文本断言 `$NegativeCases` 整行）

   原子性结论本身仍然成立且更强：这两处必须与生产方/消费方**同批**改完，因此 M4-05 的允许文件清单原先是**不完整的**（漏列这两个文件）。真正的教训不是「本地测不出来」，而是「案例清单是跨 4 个文件的共享契约」。
2. **协议变更点**：`Invoke-M4RunLifecycle.ps1:265` 当前把 `missing_driver` 的期望 stderr 固定为 `discover_aim_adapter: no AIM adapter found`。按新 Spec §3，`missing_driver_create` 与 `driver_lost_recover` 两例的期望 stderr 都必须改为 `driver_unavailable: phdskmnt is disabled or absent`。这是本任务实质性的协议改动，旧文本不得保留为兼容分支。
3. **schema 必须升版**：`myrd.m4.negative-fixtures.v1` → `v2`。依据 Spec §2「不得提供自动迁移或兼容分支」。
4. **initial `active_cases` 应为两例驱动场景** `('missing_driver_create','driver_lost_recover')`（Spec §4 的执行顺序是先跑这两个），`-ActivateRemaining` 置为其余七例。
5. **各例初始存在性**：`unknown_nonempty`、`occupied_letter`、`missing_driver_create`、`driver_lost_recover` 初始**不存在**；`size_mismatch`、`gen0`、`writer_lock`、`busy_volume`、`compact_lock` 由 Prepare 复制生成并带 `disk.rdm`。
6. **`driver_lost_recover` 的副本由控制器创建**（M4-07.2 拥有 clone 与 inventory 的权威），Prepare 只声明 `path`、`letter`、`expected_size` 与 `clone_source`（即 Formal bundle 路径），不得自行 clone，避免与 M4-07 争夺权威。
7. **改动位置清单**（`Invoke-M4RunLifecycle.ps1`）：`224`（案例列表）、`231`（契约表）、`265`/`268`（stderr 协议）、`294`/`300`（oracle schema 与关系）、`312`/`326`–`334`（selftest fixtures）、`355`（schema 闸门）、`377`–`378`（实跑分支）。该文件为压缩风格，上述多行单行长度在 1000–1700 字符之间，编辑前必须读取整行原文。

- [ ] M4-05.1 写 RED：旧八案或旧单一 `missing_driver` manifest 必须被拒绝。
- [ ] M4-05.2 将 manifest 固定为恰好九项：`unknown_nonempty`、`size_mismatch`、`occupied_letter`、`gen0`、`writer_lock`、`missing_driver_create`、`driver_lost_recover`、`busy_volume`、`compact_lock`。
- [ ] M4-05.3 为每项写入并验证绝对 case 路径、相对路径/长度/SHA-256 清单、输入常量和确定性 schema；拒绝缺项、额外项、重排、路径逃逸、数组非字符串、BOM 与非 CRLF JSON。
- [ ] M4-05.4 更新 runner 自测的 producer-shaped 九案正例，运行 `MYRD_M4_NEGATIVE_SELFTEST=1` 得到 GREEN。
- [ ] M4-05.5 运行 guest qualification 合同与标签 `m4-v3` 中 runner 测试；若完整矩阵超时，只增加显式 CTest timeout，禁止删减案例。
- [ ] M4-05.6 提交 runner/fixture manifest 改动并复盘固定文件名、StateFile 与 Prepare/Negative 生命周期文件路径一致。

### M4-06 — 实现两种驱动缺失 runner evidence 与不变量

**范围：** `tests/e2e/Invoke-M4RunLifecycle.ps1`、`tests/e2e/Prepare-M4NegativeFixtures.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-06.1 写 `missing_driver_create` RED：预检拒绝后 case 根仍不存在，且没有 bundle、journal、AIM 或目标盘符证据。
- [ ] M4-06.2 写 `driver_lost_recover` RED：拒绝前后完整相对路径/长度/SHA-256 树不同（含 journal）时必须失败。
- [ ] M4-06.3 为两种拒绝固定 `driver_unavailable` run stderr，并把 query 的 exit 1/空 stdout/固定 stderr 作为独立协议；禁止比较二者 stderr 文本。
- [ ] M4-06.4 为真实 `driver_lost_recover` 成功 evidence 加入本轮 owned AIM/`S:`、`A.txt`/`D.txt`、Formal 来源最后 checkpoint、exit/stderr、精确清理和仅诊断性设备字段。
- [ ] M4-06.5 写 mutation RED/GREEN：截断 JSON、错误 CRLF/BOM、来源 hash、树成对伪造、owned `S:`、A/D hash、exit/stderr、数组元素/PID 类型、清理失败与重复运行。
- [ ] M4-06.6 运行 runner selftest、guest contract 和 `git diff --check`；提交后确认不存在旧 `missing_driver` 成功证据兼容分支。

### M4-07 — 创建 Formal 副本并扩展控制器状态/事件 schema

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-07.1 写 RED 状态转换测试：新增 `MissingDriverCreateCaptured`、`DriverLostRecoverCaptured`、`DriverRestored`、`DriverLostRecoverRecovered` 及其事件必须 forward-only；非法回退、跳跃、sequence 复用和 Blocked 恢复全部 typed 拒绝。
- [ ] M4-07.2 在 Formal 成功后复制已验证的 Formal bundle 到专用 `driver_lost_recover` 根；复制前后生成相对路径/长度/SHA-256 inventory 并写 durable event。
- [ ] M4-07.3 写 RED：控制器若把 Formal 原 bundle 作为第三阶段写入目标、复制树不相等、source commit/kit hash 不一致或 clone 路径逃逸，必须 Blocked。
- [ ] M4-07.4 实现严格事件 schema：clone source、clone inventory、路径、hash、时间、状态/sequence 与证据 SHA 全部明确类型并写后重读。
- [ ] M4-07.5 运行 controller SelfTest 和 guest unit；提交并复盘每个新状态都已接入 `WriteState → FlushEvent → ReReadState → VerifyEvidenceHash`。

### M4-08 — 实现 Reset3 的启动门与 same-boot 零副作用

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-08.1 写 RED：`AwaitingMissingDriverReset` 同 boot 重调必须以规定 exit 返回，状态/事件字节完全不变。
- [ ] M4-08.2 在 same-boot 测试中记录所有 seam；断言不调用 Facts、query、runner、Driver、Prepare、fixture、observer 写入或 VMware 相关动作。
- [ ] M4-08.3 写 RED：new boot 缺 41、缺 6008、boot 时间未前进、服务并非缺失/禁用、query 意外成功、AIM 盘存在或 `S:` 存在均 Blocked。
- [ ] M4-08.4 实现 Reset3 新 boot gate，并为每个失败先写独立阶段 Blocked evidence 后封存资格根。
- [ ] M4-08.5 运行 production dispatcher 的 fake same-boot/new-boot 集成 selftest 到 GREEN；提交并复盘调用向量精确匹配。

### M4-09 — 编排 `missing_driver_create` 并证明零 bundle 写入

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-09.1 写 RED：控制器必须在新 boot gate 后同时执行 run 与 query；任何一方 exit 0、超时、其他 exit、非空 stdout 或不匹配 stderr 均 Blocked。
- [ ] M4-09.2 将 run 拒绝 evidence 和 query evidence 分别严格解析并写入 `MissingDriverCreateCaptured` 的独立哈希锚点。
- [ ] M4-09.3 写 RED：case 根存在、树清单不为空、出现 bundle/journal/AIM/`S:` 任何一项时拒绝；控制器自身 evidence/临时文件不进入零写入比较。
- [ ] M4-09.4 实现 zero-write validator、路径 containment 与完整 typed schema；每个 validator 失败都写子步骤名、主错误和清理错误。
- [ ] M4-09.5 运行生产 dispatcher fake 流：Reset3 new boot → `missing_driver_create` 成功捕获；随后 mutation matrix 必须在启动下一个 runner 前阻止。
- [ ] M4-09.6 提交并复盘 query/run “一致”只按两者均 exit 1 判断，不比较两条 stderr 的字面文本。

### M4-PRE — 服务恢复前置实验（由用户在干净 VM 上执行）

**排期依据：** Spec §6 规定「**实施前**必须在干净 VM 上做一项独立前置实验，确认 `sc start phdskmnt` 足以恢复 query 探针」，并规定「若实验表明 `sc start` 不足，此设计须**先增补单独的设备恢复决策与测试，再开始实现**」。本任务表原先把该实验放在 M4-15.2（全部实现之后），与 Spec 冲突；按本台账 `安全不变量 > Spec > Plan > 本台账摘要` 的冲突规则，以 Spec 为准，实验前移至 M4-10 之前。

**为什么必须前移：** M4-10 实现的正是「只用 `sc config` + `sc start` 恢复服务」这一条路径，M4-11 → M4-14 全部依赖 M4-10。若实验证伪，这 5 个任务需要按新的设备恢复决策重做。

**范围：** 仅 `docs/validation/m4-driver-unavailable-vm-pre-experiment.md`（新增）。不改任何产品、runner、控制器、verifier、kit 或测试。

**执行者：** 用户。本实验必须改变 `phdskmnt` 服务状态并在真实 VM 上运行 query，实施代理无权执行。

- [ ] M4-PRE.1 在干净 VM 上记录基线：`sc qc phdskmnt`、`sc query phdskmnt`、`myrd_m4_aim_query.exe` 的 exit/stdout/stderr。
- [ ] M4-PRE.2 将 `phdskmnt` 置为 `DISABLED`，重启进入无驱动状态，确认 query 严格满足 exit 1、stdout 空、stderr `discover_aim_adapter failed\r\n`。
- [ ] M4-PRE.3 仅执行 `sc.exe config phdskmnt start= demand` 与 `sc.exe start phdskmnt`，**不得调用 `pnputil.exe /restart-device`**。
- [ ] M4-PRE.4 严格验证 `sc qc` 为 `START_TYPE : 3 DEMAND_START`、`sc query` 为 `STATE : 4 RUNNING`。
- [ ] M4-PRE.5 运行 `myrd_m4_aim_query.exe`，记录是否严格满足成功协议：exit 0、stderr 空、单一合法 JSON（`success` 布尔 true、版本为数值、`ids` 为数值数组）。
- [ ] M4-PRE.6 写入结论：
  - **成功** → M4-PRE 记 `ON_TRACK`，解锁 M4-10，且 M4-15.2 降级为「复用本实验结论，不重复执行」。
  - **失败（`sc start` 不足以恢复 query 探针）** → M4-PRE 记 `BLOCKED`。**停止 M4-10 → M4-14**，先补充设备恢复决策并更新 Spec §6，再重新规划。不得以 `pnputil` 作为临时补丁绕过。

**完成条件：** 有一份来自真实 VM 的独立记录，明确回答「`sc config` + `sc start` 是否足以让 query 探针严格成功」，且该结论未与任何资格根目录或正式三边界流程混用。

### M4-10 — 恢复服务、探测 AIM，并执行 `driver_lost_recover` 的真实恢复

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-10.1 写 RED：恢复只允许按顺序调用 `sc.exe config phdskmnt start= demand`、`sc.exe start phdskmnt`；`pnputil.exe /restart-device` 任何引用均被静态安全合同拒绝。
- [ ] M4-10.2 写 RED：`sc qc` 必须严格证明 `START_TYPE : 3 DEMAND_START`，`sc query` 必须严格证明 `STATE : 4 RUNNING`；非零 exit、stderr、重复/缺失字段、Disabled/Manual/Stopped 均失败。
- [ ] M4-10.3 写 RED：恢复后的 `myrd_m4_aim_query.exe` 必须 exit 0、stderr 空、单一严格成功 JSON；否则在真实恢复前 Blocked。
- [ ] M4-10.4 写 RED：`driver_lost_recover` 拒绝后副本树变化（包括 journal）或恢复后 `S:` 不属于本轮 owned AIM、A/D 不等于 Formal 来源最后 checkpoint 时失败。
- [ ] M4-10.5 实现服务恢复、query 探针、真实 recovery runner、来源内容/owned `S:` 验证和独立事件；保留 PhysicalDrive/分区/PnP 为诊断，不参与条件。
- [ ] M4-10.6 运行完整 fake production dispatcher：两个拒绝 → service restore → query success → real recovery success；提交并复盘每个失败均在后续 runner 前停止。
- [ ] M4-10.7 **（自 M4-04.4 移交）** 实现并测试控制器侧的 query **拒收**矩阵：对探针 stdout 严格拒绝 BOM、前后缀空白、多个 JSON 对象、JSON 数组、非 CRLF/多余换行、缺字段、额外字段、`success` 为字符串 `"true"` 或 `false`、`api_version`/`sub_version` 为字符串或非整数、`ids` 为非数组或含非数值元素；并拒绝非空 stderr 与非 0 exit。每条变异断言精确的 Blocked reason，而非「非空字符串」。发射端已在 M4-04 的 `unit.m4.aim_query_protocol` 冻结，本项闭合另一半。

### M4-11 — 完成其余七负例、全失败矩阵与永久 Blocked 行为

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`、`tests/e2e/Invoke-M4RunLifecycle.ps1`

- [ ] M4-11.1 写 RED：真实恢复未成功时，`unknown_nonempty`、`size_mismatch`、`occupied_letter`、`gen0`、`writer_lock`、`busy_volume`、`compact_lock` 任一项不得启动。
- [ ] M4-11.2 为七个 case 的每一项写 production-shaped evidence、严格 raw/schema/hash/路径/argv/process/oracle/inventory 验证，并将最终集合固定为完整九案。
- [ ] M4-11.3 为每一个 Reset3 子步骤和七个 case 建立 fresh fake root 的失败矩阵；每行断言精确 Blocked reason、事件 reason、调用全序列、后续 seam 为零、owned cleanup 完成。
- [ ] M4-11.4 在每行失败后重调同一资格根；断言没有 runner、service、query、observer 或状态字节改变，且无自动重试。
- [ ] M4-11.5 写 RED/GREEN：最终 `NegativeComplete` evidence 缺 case、额外 case、弱 schema、错误 final lifecycle、缺 missing-driver capture hash 或来源/kit/commit 不绑定时拒绝。
- [ ] M4-11.6 运行 controller SelfTest、guest contract、runner native/protocol/negative selftest，提交并复盘所有矩阵 reason 不是“非空字符串”而是每行精确期望。

### M4-12 — 实现纯离线最终 verifier

**范围：** `tools/Invoke-M4Verification.ps1`、`tests/unit/test-m4-guest-qualification.ps1` 或新增 `tests/unit/test-m4-verification.ps1`、`tests/CMakeLists.txt`

- [ ] M4-12.1 写 RED：verifier 缺任一 driver-unavailable evidence、缺九案、篡改 clone tree/来源 checkpoint/A-D/owned `S:`/event hash 时必须失败。
- [ ] M4-12.2 实现只读 `Verify-DriverUnavailable`：严格重读 kit/source commit、Formal clone、两个缺驱动 evidence、目录清单、恢复内容、九案、final lifecycle 和所有状态事件哈希。
- [ ] M4-12.3 写 AST/命令 allowlist RED：verifier 不得调用 `myrd.exe`、query、SCM、PnP、Disk、WinEvent、VMware、Reset 或任何设备/电源命令。
- [ ] M4-12.4 添加 raw bytes、BOM、非 CRLF、额外字段、错误类型/数组元素、资格根逃逸、重复/缺失事件、hash 不符、旧 protocol 的离线 mutation matrix。
- [ ] M4-12.5 运行 verifier SelfTest 和对应 CTest 到 GREEN；提交并复盘 verifier 可在资格根目录副本上运行，不依赖当前 VM。

### M4-13 — 冻结新的 qualification kit 协议

**范围：** `tools/New-M4GuestQualificationKit.ps1`、`tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-qualification-kit.ps1`、`tests/CMakeLists.txt`

- [ ] M4-13.1 写 RED：旧 manifest schema、旧文件数/顺序、旧 source commit、旧 artifact/query 或旧脚本 hash 不能被新 controller 装载。
- [ ] M4-13.2 升级 manifest schema，固定所有 payload 的相对路径、顺序、长度、SHA-256、source commit、artifact/query SHA 与确定性 UTF-8 无 BOM JSON。
- [ ] M4-13.3 写 packager mutation matrix：同名输出目录、缺文件、额外文件、重排、BOM、长度/hash/commit 改写、artifact/query 不匹配均 fail closed。
- [ ] M4-13.4 写 controller startup 复验：VM 内 kit manifest 与每个文件 hash/length 必须相等，运行中覆盖 kit 或混用旧 kit/root 必须 Blocked。
- [ ] M4-13.5 将所有新增原生/runner/controller/verifier/kit 测试纳入 `m4-v3`，调整超时但不得缩减测试。
- [ ] M4-13.6 运行 kit 合同与 Debug/Release focused CTest，提交并复盘 exact 文件清单没有遗漏或额外测试工具。

### M4-14 — 完成发布前本地验证和人工验收说明

**范围：** `README.md`、`docs/validation/*`、`M4_TASKLIST.md`

- [ ] M4-14.1 更新 README：给出 host kit 生成四参数命令、Release artifact/query 路径、hash 检查、完整 kit 复制、新 qualification root、控制器命令（不写不存在的 `-Run` 参数）。
- [ ] M4-14.2 更新人工步骤：恰好三次电源边界、每段 Reset 或 Power Off→Power On 二选一、不得正常关机、不得手动修改资格根、Windows 修复屏的判定规则。
- [ ] M4-14.3 运行 PowerShell AST、guest/kit/verifier selftest 和以下全量命令：

  ```powershell
  cmake --preset msvc
  cmake --build --preset debug --parallel 1
  cmake --build --preset release --parallel 1
  ctest --preset debug -L m4-v3 --output-on-failure
  ctest --preset release -L m4-v3 --output-on-failure
  git diff --check
  ```

- [ ] M4-14.4 记录命令、退出码、测试总数、commit、没有真实 VM 资格结论的原因；任何失败回到最早相关任务。
- [ ] M4-14.5 在宿主机生成 commit+GUID 的新 kit，逐文件验证 manifest/hash/length，并记录发布候选证据；禁止覆盖旧 kit。
- [ ] M4-14.6 提交文档/本地验证记录，复盘后才允许开始 VM 前置实验。

### M4-15 — 执行三次人工电源边界验收（前置实验见 M4-PRE）

**范围：** 仅新 kit、新 VM、新 qualification root 与 `docs/validation/m4-driver-unavailable-vm-*`（新增）

- [ ] M4-15.1 在干净 VM 上记录服务、query、AIM/盘符、事件和 kit hash 基线；新建从未存在过的 qualification root。
- [ ] M4-15.2 **复用 M4-PRE 的结论，不重复执行前置实验**。确认 M4-PRE 为 `ON_TRACK` 且其记录来自干净 VM；核对本次 VM 的 `phdskmnt` 恢复路径与 M4-PRE 所验证的一致（仅 `sc config` + `sc start`，无 `pnputil`）。若 M4-PRE 不是 `ON_TRACK`，本任务立即 `BLOCKED`，不得开始三边界流程。
- [ ] M4-15.3 仅在 M4-15.2 成功后，执行 Formal：运行控制器至 armed，用户做一次 Reset 或一次 Power Off→Power On，再运行同一命令并保存全部 evidence hash。
- [ ] M4-15.4 执行 Live Power-Cut：运行控制器至 armed，用户做一次且仅一次允许电源边界，再运行同一命令并保存 evidence hash。
- [ ] M4-15.5 执行 Missing-driver：运行控制器至 armed，用户做一次且仅一次允许电源边界；重新运行命令，让控制器完成两个缺驱动 case、服务恢复、真实副本恢复与余下七例。
- [ ] M4-15.6 在不改变 VM 的条件下运行最终离线 verifier；保存 stdout、退出码、资格根哈希与三段电源边界记录。
- [ ] M4-15.7 仅当 M4-14 全绿、M4-15.2 前置实验成功、三段都通过且 verifier 通过时记录“VM 已资格验证”；否则只记录“VM 验收未完成”，禁止夸大为 M4 已通过。

## M4-3. 每个任务的复盘模板

每个任务的 implementation commit 后，必须先将状态标为 `REVIEWING`，填完以下字段，再单独提交本 `M4_TASKLIST.md`：

```markdown
### Review Record — M4-XX

- Start commit: `<sha>`
- Implementation commit: `<sha>`
- 允许文件与实际文件：`一致 | 偏差说明`
- RED 证据：`命令、退出码、预期失败文本`
- GREEN 证据：`命令、退出码、通过数量`
- Debug/Release 证据：`命令、通过数量；未运行时写原因`
- Spec/Plan 对齐：`逐条确认或列出偏差`
- 安全检查：`无 VMware/驱动/设备动作，或已授权的精确动作与证据`
- 结论：`ON_TRACK | DEVIATED | BLOCKED | NO_GO_EVIDENCE`
```

只有 `ON_TRACK` 才能解锁下一项。任何偏差先更新并批准 Spec/Plan，再更新本台账。

## M4-4. 复盘记录

### Review Record — M4-00

- Start commit: `157bdb5cd5849b52386846d7c3fff8a7b1f9bb0a`
- Implementation commit: `c20c269`（仅新增 `docs/validation/m4-driver-unavailable-baseline.md`）
- 允许文件与实际文件：`一致`。实现提交只含 `docs/validation/m4-driver-unavailable-baseline.md`；复盘提交只含本 `M4_TASKLIST.md`。注意：本文件在 M4-00 开始前已存在未提交的用户改写（整份台账重写），该改写随本复盘提交一并入库，因为它就是 M4-00 所要更新的台账本体。`tasklist.md` 的未提交修改与未跟踪的 `docs/superpowers/plans/2026-08-21-m4-driver-unavailable-qualification.md` 均在允许范围之外，未被 stage、未被提交、未被改动。
- RED 证据：`不适用`。M4-00 是只读基线与台账建立任务，本任务表未为其定义 RED 项；不引入任何产品或测试代码。
- GREEN 证据：
  - `git status --short --branch` → exit 0
  - `git log -5 --oneline` → exit 0
  - `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-guest-qualification.ps1 -SourceRoot .` → exit 0，输出 `M4 guest qualification contract PASS`
  - `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-qualification-kit.ps1 -SourceRoot .` → exit 0，输出 `M4 qualification kit PASS`
  - PowerShell AST `Parser::ParseFile` 解析 controller/runner/verifier → exit 0，三份脚本各 0 个解析错误（tokens 分别为 50035 / 13059 / 5363）
  - 通过数量：两份合同脚本只输出单行 PASS 汇总，未输出用例计数，因此不记录用例数量。
  - `git diff --check` 与 `git diff --cached --check` → exit 0，无空白错误。
- Debug/Release 证据：`未运行`。原因：M4-00 范围不含 C++ 构建与 CTest；`cmake --preset msvc`、Debug/Release 构建与 `-L m4-v3` 属 M4-01 起的任务，M4-14 负责全量双配置验证。
- Spec/Plan 对齐：
  - Spec §9–§10 与 Plan 任务 7 要求先冻结起点与本地只读基线 — 已满足，且未生成 kit、未创建资格根目录。
  - Spec §2 “不迁移旧 kit/旧资格根目录” — 已满足：只记录既有 kit schema `myrd.m4.qualification-kit.v1` 与固定文件清单作为对照基线，未复用、未迁移。
  - Spec §6 “恰好三个 VMware 电源边界” — 本任务消耗 0 个边界。
  - Spec §7 证据字节规范 — 基线记录为 UTF-8 无 BOM、CRLF 结尾，写后重读校验：7216 字节、BOM=False、裸 LF=0、SHA-256 `9A8A5813B6693667696D622D4501DE58CAC125813A50F94F07310DD06EE342C1`。
  - 无偏差。
- 安全检查：`无 VMware/驱动/设备动作`。未执行任何 VMware 操作、SSH、`sc.exe`、`pnputil.exe`、`Get-PnpDevice`/`Get-Disk`/`Get-WinEvent`、`myrd.exe`、`myrd_m4_aim_query.exe`、AIM 设备创建、bundle 读写或电源边界。`out/` 下的原生 artifact 只被哈希读取，未被构建、运行或替换。
- 结论：`ON_TRACK`

### Review Record — M4-01

- Start commit: `c20c269`（M4-00 实现提交；本任务开始时 HEAD 为 `c2e8171`）
- Implementation commit: `c9bc81a`
- 允许文件与实际文件：`一致`。实际改动恰为 `tests/unit/m4_driver_preflight_tests.cpp`（新增）、`src/run/driver_preflight.h`（新增，仅声明与注释）、`tests/CMakeLists.txt`（新增一处测试注册）。未新增 `.cpp` 实现，未触碰 `src/CMakeLists.txt` 与 `main.cpp`。
- RED 证据：
  - `cmake --build --preset debug --parallel 1 --target myrd_m4_driver_preflight_tests` → 构建失败（ninja 停止）。预期失败文本：`error LNK2019: 无法解析的外部符号 ... myrd::classify_phdskmnt_service(bool,unsigned long)` 与 `... myrd::require_phdskmnt_service_available(struct myrd::ScmServiceProbe const &)`，收敛为 `fatal error LNK1120: 2 个无法解析的外部命令`。
  - `ctest --preset debug -R '^unit\.m4\.driver_preflight$' --output-on-failure` → exit 8，`1/1 Test #126: unit.m4.driver_preflight ***Not Run`，`0% tests passed, 1 tests failed out of 1`。
  - 两级 RED 均为「缺实现」，符合 M4-01.5 的预期失败类别。
- GREEN 证据：`本任务不产生 GREEN`。M4-01 只交付 RED 与声明；GREEN 属 M4-02。
- Debug/Release 证据：Debug 配置成功：`cmake --preset msvc --fresh` → exit 0，`The CXX compiler identification is MSVC 19.44.35228.0`。Release 已于 M4-04 之后补跑，见 [跨任务 Release 证据](#m4-42-跨任务-release-证据)。
- Spec/Plan 对齐：
  - Plan 任务 1 的 RED 矩阵四行全部覆盖（服务不存在、`SERVICE_DISABLED`、四种可用启动类型、三类 SCM I/O 失败）。
  - Plan 任务 1「不依赖测试机上实际 `phdskmnt` 服务的存在、权限或启动类型」— 已满足：`FakeScm` 为纯内存 seam，测试不触碰真实 SCM。
  - Spec §3.1「预检不读取 bundle」— 已满足：新头文件不引用 bundle、adapter handle、StoragePort、SRB QUERY_VERSION、设备或盘符。
  - 无偏差。
- 安全检查：`无 VMware/驱动/设备动作`。测试为纯内存 seam，未调用真实 `OpenSCManagerW`/`OpenServiceW`/`QueryServiceConfigW`，未运行 `myrd.exe`，未触碰服务、设备、磁盘或电源边界。
- 环境记录：`out/build/msvc` 原有 CMake 缓存指向 CLion 自带 MinGW `g++.exe`，与本仓库 `CMakeLists.txt:5` 的 `M0 requires MSVC` 冲突，导致 `cmake --preset msvc` 失败。已用 CMake 官方 `--fresh` 重建缓存为 MSVC 19.44；该目录是可重新生成的构建产物，未触碰任何源码或用户文档。
- 结论：`ON_TRACK`

### Review Record — M4-02

- Start commit: `46e739d`
- Implementation commit: `d134963`
- 允许文件与实际文件：`一致（允许集的子集）`。实际改动为 `src/run/driver_preflight.cpp`（新增）、`src/CMakeLists.txt`、`tests/unit/m4_driver_preflight_tests.cpp`。`src/run/driver_preflight.h` 在允许集内但无需再改——M4-01 已冻结其声明。未触碰 `tests/CMakeLists.txt`：测试沿用本仓库既有惯例，直接 `#include "src/run/driver_preflight.cpp"`（与 `m4_run_policy_tests.cpp` 包含 `run_loop.cpp` 一致），因此无需新增构建注册。
- RED 证据：见 M4-01（LNK2019 / LNK1120；ctest `Not Run`）。本任务开始时该 RED 仍然成立。
- GREEN 证据：
  - `cmake --build --preset debug --parallel 1 --target myrd_m4_driver_preflight_tests` → exit 0，`Linking CXX executable tests\Debug\myrd_m4_driver_preflight_tests.exe`。
  - `ctest --preset debug -R '^unit\.m4\.driver_preflight$' --output-on-failure` → exit 0，`1/1 Test #126: unit.m4.driver_preflight ... Passed`，`100% tests passed, 0 tests failed out of 1`。
  - `cmake --build --preset debug --parallel 1 --target myrd` → exit 0，确认新增源文件已进入产品目标且不破坏 `myrd` 链接。
- Debug/Release 证据：Debug focused 目标已绿（见上）。Release 已于 M4-04 之后补跑，见 [跨任务 Release 证据](#m4-42-跨任务-release-证据)。
- Spec/Plan 对齐：
  - Plan 不变量 1 — 已满足且已加测试固定：`driver_unavailable` 只代表两种已知结果。未定义启动类型归为 `available`（新增测试 `m4_driver_preflight_undefined_start_type_is_left_to_adapter_discovery`），避免该协议代表第三种未知条件。
  - M4-02.3 字节协议 — `ErrorDomain::usage`、code `1`、`driver_unavailable`、`phdskmnt is disabled or absent`，四项逐字段断言。
  - M4-02.4 — 三类 SCM 失败各自保留 `ErrorDomain::win32`、原始错误码与自身操作名，并断言渲染文本不含 `driver_unavailable`。
  - M4-02.5 handle-close — 新增 `m4_driver_preflight_closes_every_handle_it_opened`，逐场景断言精确的关闭序列（含「未打开则不关闭」）。错误优先级 — 新增 `m4_driver_preflight_stops_at_the_first_failing_scm_call` 与 `m4_driver_preflight_reads_the_open_error_before_closing_the_manager`，后者证明先读 `last_error` 再 `close_handle`（`CloseServiceHandle` 会覆盖线程错误值）。
  - M4-02.6 — `driver_preflight.cpp` 仅 include `driver_preflight.h` 与 `<vector>`；无 adapter handle、StoragePort、SRB QUERY_VERSION、设备枚举、盘符检查或 bundle 访问。
  - 无偏差。
- 安全检查：`无 VMware/驱动/设备动作`。全部测试走内存 seam；未调用真实 SCM，未运行 `myrd.exe`，未触碰服务、设备、磁盘或电源边界。
- 结论：`ON_TRACK`

### Review Record — M4-03

- Start commit: `714ec09`
- Implementation commit: `def1812`
- 允许文件与实际文件：`有一处受控扩展`。实际改动为 `src/run/main.cpp`、`tests/unit/m4_product_boundary_tests.ps1`（均在允许集内），外加 `src/run/driver_preflight.h`。该头文件不在 M4-03 允许集内，但必须改：它在 `main.cpp` 的新 include 位置先于项目头引入 `<windows.h>`，`min`/`max` 宏破坏了 `src/common/operation_context.h:13` 与 `src/storage/block_math.h:64`（`error C2589/C2059/C3615`）。修法为采用本仓库既有头文件惯例 `#define WIN32_LEAN_AND_MEAN` + `#define NOMINMAX`（与 `src/common/unique_handle.h` 完全一致），未改变任何声明或语义。`src/CMakeLists.txt` 在允许集内但无需再改——M4-02 已注册该源文件。
- RED 证据：`ctest --preset debug -R '^unit\.m4\.product_boundary$' --output-on-failure` → exit 8，`1/1 Test #120: unit.m4.product_boundary ***Failed`，失败文本 `product main must call the phdskmnt service preflight`。
- GREEN 证据：
  - `cmake --build --preset debug --parallel 1 --target myrd` → exit 0（先于 ctest 重建，确保合同不是对着旧 exe 通过）。
  - `ctest --preset debug -R '^unit\.m4\.(driver_preflight|product_boundary)$' --output-on-failure` → exit 0，`100% tests passed, 0 tests failed out of 2`（#120 product_boundary 24.84s、#126 driver_preflight 0.02s）。
- Debug/Release 证据：Debug focused 命令已绿（见上）。Release 已于 M4-04 之后补跑，见 [跨任务 Release 证据](#m4-42-跨任务-release-证据)。
- Spec/Plan 对齐：
  - Spec §3.1「预检完成前不得读取、创建、修改或挂载 bundle」— 合同以 wmain 内源码偏移逐项断言预检早于 `classify_bundle(`、`create_image(`、`recover_complete_image(`、`M3AimRuntime::create(`、`find_recovered_volume(`、`provision_new_volume(`。
  - Plan 任务 1「精确插入在 `require_elevation()` 成功之后」— 已满足并被断言；错误优先级保持：未提权时 `require_elevation` 先返回并 `return 1`，永不报告服务状态。
  - M4-03.1「Create 与 Recover 不得各自有第二个预检入口」— 以全文件 `Measure-LiteralOccurrence` 断言恰好一次调用。
  - M4-03.2 字节协议 — 断言 `L"driver_unavailable"` 与 `L"phdskmnt is disabled or absent"` 在 `driver_preflight.cpp` 中各恰好出现一次、`ErrorDomain::usage, 1`、三个 SCM 操作名各恰好一次且与 `driver_unavailable` 构造点不相邻，因此 SCM I/O 失败的 stderr 不可能含该 operation。exit 1 与 CRLF 由既有 `print_run_error` 路径保证，并已在 M4-02 的 C++ 单测逐字段冻结。
  - **未决**：Spec §3 把该协议中的 bundle 路径写为「绝对 bundle 路径」，但产品对所有 run 错误一律原样打印命令行传入的路径（相对路径不会被 canonicalize）。该行为先于本任务存在，非 M4-03 引入，已登记为 **D-1** 待用户决策；本任务的合同只断言 `print_run_error` 的字段与形状，未对路径绝对性作出任何主张。
  - 无偏差（头文件扩展已如实记录并说明原因）。
### Review Record — M4-04

- Start commit: `b949b44`
- Implementation commit: `0db6eb9`
- 允许文件与实际文件：`一致（允许集的子集，外加一处必要注册）`。实际改动为 `tests/e2e/m4_aim_query.cpp`、`tests/unit/m4_product_boundary_tests.ps1`、新增 `tests/unit/m4_aim_query_protocol_tests.cpp`（即允许集中的「对应 C++ 单元测试」）、`tests/CMakeLists.txt`（注册该测试，与 M4-01 同例）。`src/driver/adapter_discovery.cpp` 与 `src/m3/m3_windows_acceptance.cpp` 在允许集内但**未修改**——Plan 任务 2 规定「仅在必要时补可测导出/注释，不改变权威逻辑」，经审阅两者的权威逻辑与调用位置均已正确，无需改动。
- RED 证据：
  - `write_m4_query_json` 断言在重构前不可能通过（当时 query 内联拼接 JSON，无该标识符）；`ctest -R '^unit\.m4\.aim_query_protocol$'` 在测试文件存在前为 `Not Run`。
  - 更强的 RED：合同内置 10 条 self-audit 变异，每条都必须被拒绝，否则测试抛出 `adapter authority self-audit missed: <name>`。变异覆盖：StoragePort GUID 被改、`SPDRP_SERVICE` 换成 `SPDRP_HARDWAREID`、QUERY_VERSION 探针移除、多候选拒绝被短路、`SPDRP_FRIENDLYNAME` 顶替服务过滤、预检引入 PhysicalDrive 匹配、query 失败文本改名、失败输出改到 stdout、成功发射绕过受审计 formatter、provider 的 discovery 调用被删除。
  - 过程中出现一次真实 RED：初版 self-audit 用了 `&{...}` 动态调用，被本文件既有的 `Assert-SafeBoundaryInvocations` 判为 `boundary invocation safety found a dynamic or qualified command invocation`。已改为静态 hashtable 表驱动，不削弱既有安全规则。
- GREEN 证据：`ctest --preset debug -R '^unit\.m4\.(driver_preflight|aim_query_protocol|product_boundary)$' --output-on-failure` → exit 0，`100% tests passed, 0 tests failed out of 3`（#120 product_boundary 24.70s、#126 driver_preflight 0.01s、#127 aim_query_protocol 0.01s）。
- Debug/Release 证据：Debug focused 验证已绿（见上）。Release 已于 M4-04 之后补跑，见 [跨任务 Release 证据](#m4-42-跨任务-release-证据)。
- Spec/Plan 对齐：
  - Spec §3.2「`discover_aim_adapter` 是唯一设备发现权威」— 以源码合同冻结四项标记（StoragePort GUID、`SPDRP_SERVICE`、`phdskmnt`、QUERY_VERSION/版本匹配）与零/多候选拒绝。
  - Spec §3.2 与 §6「不把 FriendlyName/Class/PnP/序列号/PhysicalDrive 作为第二匹配权威」— 对 discovery、preflight、query 三处源码同时禁止。
  - Spec §3「query 失败协议 exit 1 / stdout 空 / stderr `discover_aim_adapter failed\r\n`」— 冻结文本唯一性、`std::wcerr` 去向、以及紧随其后的 `return 1`。CRLF 由 Windows 宽字符文本流对 `std::endl` 的换行转换保证。
  - Spec §4「成功探针 JSON 严格类型」— 新 C++ 单测逐字节断言：单行、无 BOM、恰好一个换行且位于末尾、`success` 为不带引号的 `true`、`api_version`/`sub_version` 为不带引号的数值、`ids` 为裸数值数组、uint32 全量程。
  - Spec §3.2 / M4-04.5「服务可用但 discovery 失败仍走既有协议且不承诺零 bundle 写入」— 合同断言 `no AIM adapter found` 文本仍在，且断言 `discover_aim_adapter` 调用仍位于 `provider_->start(` **之后**（未前移为第二道 `driver_unavailable` 闸门）；本任务未新增任何该场景的零写入主张。
  - 无偏差。
- 安全检查：`无 VMware/驱动/设备动作`。新 C++ 单测以 `MYRD_M4_AIM_QUERY_NO_MAIN` 抑制 `wmain`，只编译并调用纯 formatter，不枚举设备、不打开句柄、不发 SRB。未运行 `myrd_m4_aim_query.exe`。`unit.m4.product_boundary` 仍只运行既有白名单内的 `myrd.exe` 调用。
- 结论：`ON_TRACK`（范围已按下述更正缩小）

#### 更正 — M4-04.4 曾被过度声明

初版复盘把 M4-04.4 记为完成，但该子项要求「BOM、多 JSON、错误 CRLF、额外/错误类型均**失败**」。实际交付的只是 query 成功 JSON 的**发射端**逐字节断言；「均失败」需要一个严格**拒收方**，而当时不存在任何会拒绝畸形 query 输出的代码。

更正内容：

1. M4-04.4 改记为 `[~]` 部分完成，明确写出已完成/未完成的边界；
2. 拒收矩阵移交新增的 M4-10.7，与「控制器是 query 的消费方」这一职责映射一致；
3. 在 M4-10.7 完成前，任何复盘不得声称 query 严格协议已双向闭环。

M4-04 的其余子项（M4-04.1/.2/.3/.5/.6）未受影响，其证据与 10 条 self-audit 变异仍然有效。

- 安全检查：`无 VMware/驱动/设备动作`。`unit.m4.product_boundary` 会运行 `myrd.exe`，但仅限既有的受审计有界调用集合（`--help`、无参、`run --help`、`compact --help`、缺失 bundle 的 compact、无效 run，以及一个临时目录中的 unknown bundle）；该集合由合同自身的 `Assert-SafeBoundaryInvocations` 白名单强制，未创建 AIM 设备、未挂载盘符、未触碰服务或电源边界。本机 `phdskmnt` 处于可用状态，因此新预检放行、unknown bundle 仍按既有 `classify_bundle` 协议失败——预检未修改任何服务状态。
- 结论：`ON_TRACK`

## M4-4.2 跨任务 Release 证据

在 M4-04 完成后补跑，覆盖 M4-01 → M4-04 的全部产出。补跑动机：这四份复盘原先都记「Release 未运行，属 M4-14」，而本仓库 `myrd_enable_warnings` 将告警视为错误、Release 的告警集与 Debug 不同，因此「是否需要回头返工」在 Release 通过前无法回答。

| 命令 | 退出码 | 结果 |
|---|---|---|
| `cmake --build --preset release --parallel 1` | 0 | 全量 Release 构建，零 error、零 warning |
| `ctest --preset release -L m4-v3 --output-on-failure` | 0 | `100% tests passed, 0 tests failed out of 18` |
| `cmake --build --preset debug --parallel 1` | 0 | 全量 Debug 构建，零 error |
| `ctest --preset debug -L m4-v3 --output-on-failure` | 0 | `100% tests passed, 0 tests failed out of 18` |

结论：M4-01 → M4-04 的产出在 Debug 与 Release 双配置下均无回归，**无需返工**。本记录不替代 M4-14 的发布前全量验证——M4-14 仍须在其自身的起始 commit 上重新执行完整命令表。

### Review Record — M4-05

- Start commit: `b474157`
- Implementation commit: `8bb2410`（九案拆分）+ `b8a3162`（composition 合同回归修复）。两次实现提交，不合并、不改写历史——`8bb2410` 引入的回归由 `b8a3162` 显式修复，保留可追溯性。
- 允许文件与实际文件：**`偏差（允许集原先不完整，已更正）`**。台账原定允许集为 `Prepare-M4NegativeFixtures.ps1`、`Invoke-M4RunLifecycle.ps1`、`test-m4-guest-qualification.ps1`、`tests/CMakeLists.txt`。实际必须改动：
  - `tests/e2e/Prepare-M4NegativeFixtures.ps1`（生产方，允许集内）
  - `tests/e2e/Invoke-M4RunLifecycle.ps1`（消费方，允许集内）
  - `tests/unit/test-m4-formal12-support.ps1`（**允许集外**：端到端跑 Prepare 并断言旧 manifest 形状）
  - `tests/integration/m4_product_composition_tests.cpp`（**允许集外**：源码文本断言 `$NegativeCases` 整行）

  后两者不是范围蔓延，而是同一共享契约的必要组成：案例清单被 4 个文件断言，不同批改就是红的。允许集的遗漏已回写至 M4-05 实施前置分析第 1 条。`test-m4-guest-qualification.ps1` 与 `tests/CMakeLists.txt` 在允许集内但无需改动。
- RED 证据：
  - 负例 runner selftest（`MYRD_M4_NEGATIVE_SELFTEST=1`，Phase `Negative`）→ exit **77**，`M4 lifecycle Negative BLOCKED: negative contract case count`。断言收紧为 `Count -eq 9` 并新增 `$NegativeCases -notcontains 'missing_driver'`，旧八案与旧单一 `missing_driver` 均被拒绝。
  - 第二次真实 RED（由独立复核发现，非计划内）：`ctest --preset debug -R '^integration\.m4\.product_composition$'` → exit 8，`FAIL m4_product_composition_tests.cpp:162`，该测试仍断言旧八案整行。
- GREEN 证据：
  - 负例 runner selftest → exit 0，`M4 negative contract selftest PASS`
  - `test-m4-guest-qualification.ps1 -SourceRoot .` → exit 0，`M4 guest qualification contract PASS`
  - `ctest --preset debug -L m4-v1` → `100% tests passed, 0 tests failed out of 1`
  - `ctest --preset debug -L m4-v2` → `100% tests passed, 0 tests failed out of 3`
  - `ctest --preset debug -L m4-v3` → `100% tests passed, 0 tests failed out of 18`
  - `ctest --preset debug`（全量 143 项）→ `99% tests passed, 1 tests failed`，唯一失败为 `integration.m3.recovery`（标签 `m3-v2`）。**已用 `git stash` 在剔除本次全部改动的前提下复现该失败**，确认为先于 M4-05 存在的问题，与本任务无关，未被本任务引入也未被掩盖。
  - `git diff --check` → 干净。
- Debug/Release 证据：Debug 三个 m4 标签与全量套件如上。Release 未在本任务重跑（M4-05 只改测试/fixture 脚本与一处测试源码文本断言，无产品代码变更）；双配置全量属 M4-14。
- Spec/Plan 对齐：
  - Spec §4「负例总数由 8 增至 9」及其固定顺序 — 已满足，`$NegativeCases` 与 `$NegativeContracts` 顺序一致并有断言。
  - Spec §3「两个缺驱动场景的统一协议」— 两例期望 stderr 均为 `driver_unavailable: phdskmnt is disabled or absent`；旧 `discover_aim_adapter: no AIM adapter found` 文本已从 runner 中**完全移除**，并由 `m4_product_composition_tests.cpp` 断言其不得复现。
  - Spec §2「不得提供自动迁移或兼容分支」— schema 升为 `myrd.m4.negative-fixtures.v2`，无 v1 兼容分支。
  - Spec §4 执行顺序 — 初始 `active_cases` 为 `missing_driver_create`、`driver_lost_recover`；`-ActivateRemaining` 置其余七例。
  - clone 权威边界 — Prepare 只声明 `clone_source`，不自行 clone；副本仍由 M4-07.2 的控制器创建。
- 安全检查：`无 VMware/驱动/服务/设备/磁盘/电源动作`。本任务只改测试与 fixture 脚本，未运行 `myrd.exe`、query 工具、`sc.exe`、`pnputil`，未生成 kit，未创建资格根目录。runner 中的 `Get-Service -Name 'phdskmnt'` 为既有 fixture 代码，仅在真实 VM 的 Negative 阶段执行，本任务未触发。
- 过程记录（供后续任务参照）：本任务的实现由 subagent 完成，其自验仅运行了 `m4-v3` 标签，因而**未发现**自己引入的 `m4-v1;m4-v2` 回归。教训：**改动跨标签共享的契约时，按标签验证不充分，必须跑全量套件**。后续任务的 GREEN 证据一律以全量 `ctest --preset debug` 为准，并对任何失败项用 `git stash` 判定是否先于本任务存在。
- 结论：`ON_TRACK`

### Review Record — M4-06

- Start commit: `ee97f58`
- Implementation commit: `15e1c01`
- 允许文件与实际文件：`一致（允许集的子集）`。实际改动 `tests/e2e/Invoke-M4RunLifecycle.ps1`、`tests/unit/test-m4-guest-qualification.ps1`。`Prepare-M4NegativeFixtures.ps1` 在允许集内但无需改动（九案 manifest 已由 M4-05 落地）。**未**触碰 M4-05 暴露的两个共享契约文件，也未触碰 `src/`、`tools/`、`M4_TASKLIST.md`、`tasklist.md`。
- RED 证据：`MYRD_M4_NEGATIVE_SELFTEST=1` 负例 selftest → exit **77**，`M4 lifecycle Negative BLOCKED: driver-lost-recover success contract accepted cleanup_exit`。这是实现过程中变异矩阵抓到的**真实校验器缺口**：成功证据校验器初版只对 `cleanup.exit` 做整数类型检查，导致非零退出（清理失败）可以蒙混通过；收紧为 `exit -eq 0` 后转 GREEN。变异矩阵本身即常驻 RED 装置——任一变异被接受都会以 `... accepted <mutation>` + exit 77 暴露。
- GREEN 证据（由主控制器独立复核，非采信 subagent 自报）：
  - `cmake --build --preset debug --parallel 1` → exit 0，0 个编译错误（**先重建再测**，避免 M4-05 出现过的陈旧测试二进制假象）
  - `ctest --preset debug --output-on-failure`（全量 143 项）→ `99% tests passed, 1 tests failed out of 143`
  - 唯一失败 `integration.m3.recovery`（标签 `m3-v2`），为 M4-05 已用 `git stash` 判定的先存问题，未被修复也未被掩盖
  - `MYRD_M4_NEGATIVE_SELFTEST=1` 负例 selftest → exit 0，`M4 negative contract selftest PASS`
  - `test-m4-guest-qualification.ps1 -SourceRoot .` → exit 0，`M4 guest qualification contract PASS`
  - `test-m4-formal12-support.ps1 -SourceRoot .` → exit 0，`M4 Formal12 support contract PASS`
  - 协议 grep 复核：`Invoke-M4RunLifecycle.ps1` 中 `discover_aim_adapter: no AIM adapter found` 出现 **0** 次；`driver_unavailable: phdskmnt is disabled or absent` 出现 3 次；query 自有协议 `discover_aim_adapter failed` 出现 3 次
  - `git diff --check` 干净；两文件 UTF-8 无 BOM、CRLF、零裸 LF
- Debug/Release 证据：Debug 全量如上。Release 未在本任务重跑（改动为纯 PowerShell 测试/fixture 脚本，无 C++ 产品或测试源码变更）；双配置全量属 M4-14。
- Spec/Plan 对齐：
  - Spec §5.1 `missing_driver_create` 零写入 — 拒绝后断言 `case_root_absent`（`-not (Test-Path $path)`）、`bundle_hash='ABSENT'`（`TreeHash` 仅当受管 case 根不存在时返回 `ABSENT`，因此其下无 bundle/journal）、`letter_guid='ABSENT'`、`windows_unchanged`（稳定盘清单比对覆盖「无 AIM 盘」）。比较范围**只含受管 case 根**，不含控制器证据目录、临时文件或 Windows 事件日志。
  - Spec §5.2 `driver_lost_recover` 树不变 — `bundle_hash = TreeHash $path` 为逐文件「相对路径 | 长度 | SHA-256」全量清单（含 journal），断言拒绝前后非 `ABSENT` 且逐字节相等。
  - Spec §3「run 与 query 协议独立」— query 走独立字段与独立校验器 `Assert-DriverUnavailableQueryProtocol`（exit 1、stdout 空、`discover_aim_adapter failed\r\n`），**两条 stderr 从不做字面比较**，「一致」只按二者均 exit 1 判定。
  - Spec §5.2 / 执行规则 7「物理枚举仅作诊断」— `physical_drive`/`partition_number`/`pnp_device_id` 位于 `diagnostics` 字段，校验器只做类型检查（int-or-null / string-or-null），**不参与任何通过判据**。
  - Spec §2 无兼容分支 — 旧单一 `missing_driver` 与旧拒绝文本均已不存在。
- 安全检查：`无 VMware/驱动/服务/设备/磁盘/电源动作`。仅本地 PowerShell 测试代码改动；未运行 `myrd.exe`、query 工具、`sc.exe`、`pnputil`，未生成 kit，未创建资格根目录。新增的 `Refusal-Query` 子进程助手只存在于 VM 专用真实分支，本地从未执行。
- 未完成/待接续（如实记录，勿遗忘）：
  1. runner 新增可选 `-Query` 参数，但 `Capture-M4Child.ps1` 与 `tools/Invoke-M4GuestQualification.ps1` **尚未接线传入**。该接线属控制器侧工作，须在 **M4-09**（`missing_driver_create` 同时调用 run 与 query）落实，否则 query 证据分支在真实 VM 中不会被触发。
  2. `driver_lost_recover` 真实恢复的成功证据在本任务交付为**冻结 schema + 严格校验器 + selftest fixture**；真实产出方在控制器，属 **M4-10**。这与既有 `Assert-NegativeCaseEvidence` 的 runner/controller 镜像模式一致。
- 结论：`ON_TRACK`

## M4-0.3 环境前提（E 系列）

### E-1（验证前提，非缺陷）— 全量 ctest 必须在提权会话中运行

实测：`Elevated=False` 的会话中 `ctest --preset debug` 全量为 **136/143**；失败 7 项全部依赖管理员令牌，且已用 `git stash` 证明与代码改动无关：

`integration.shm`、`unit.lifecycle_runner`、`integration.m2.shm`、`unit.m2.windows_acceptance`、`integration.m3.windows_vhdx`、`integration.m3.provider`、`unit.m4.product_boundary`

在 `Elevated=True` 的会话中，全量基线为 **142/143**，唯一失败是已知先存问题 `integration.m3.recovery`（标签 `m3-v2`）。

规则：

1. 所有「全量 ctest」的 GREEN 证据必须注明运行会话是否提权。
2. 在未提权会话中，GREEN 证据只以不依赖提权的子集为准（`m4-v1`/`m4-v2`/`m4-v3` 标签与 runner/controller/kit selftest）。
3. M4-14 的发布前全量验收**必须**在提权会话中运行，否则不得宣称「本地验证通过」。

## M4-0.4 组件间协议漂移（D 系列续）

### D-3（待收口，属 M4-09）— 控制器与 runner 的证据文件名/拒绝协议已漂移

M4-05/M4-06 把 runner 拆成 `missing_driver_create` 与 `driver_lost_recover` 两例，证据文件名变为 `m4-negative-missing_driver_create.v1.json` 与 `m4-negative-driver_lost_recover.v1.json`，拒绝协议改为 `driver_unavailable: phdskmnt is disabled or absent`。

但 `tools/Invoke-M4GuestQualification.ps1` 仍有 **8 处**硬编码旧名 `m4-negative-missing_driver.v1.json`，以及 **7 处**旧拒绝文本 `discover_aim_adapter: no AIM adapter found`（后者大多位于 selftest 自造 fixture 内）。真实 VM 流程中控制器会去读一个 runner 再也不会生成的文件。

为何测试抓不到：控制器的 SelfTest 与失败矩阵用旧名字**自造 fixture**，因此 `M4 guest qualification SelfTest PASS` 是本地绿灯，掩盖了跨组件断裂。

收口归属：**M4-09**（编排 `missing_driver_create` 控制器路径）必须把控制器的证据文件名、拒绝协议、以及 M4-06 遗留的 runner `-Query` 接线一并对齐。D-3 未关闭前，任何复盘不得声称「控制器与 runner 已端到端一致」。


### Review Record — M4-07

- Start commit: `45050cc`
- Implementation commit: `212f27e`
- **来源说明**：本任务的实现（`tools/Invoke-M4GuestQualification.ps1` +155 行、`tests/unit/test-m4-guest-qualification.ps1`）在会话恢复后以**未提交改动**出现在工作树中，非本主控制器直接产出；内容与当初派发给被中断 subagent 的 M4-07 brief 逐项吻合，最可能是该 agent 写完但未提交。按用户决策「逐条核验后采纳」，先逐条核验，再提交；提交信息已如实注明来源。
- 允许文件与实际文件：`一致`。实际改动恰为允许集内的 `tools/Invoke-M4GuestQualification.ps1` 与 `tests/unit/test-m4-guest-qualification.ps1`。`tests/CMakeLists.txt` 无需改动。
- RED 证据：`tools/Invoke-M4GuestQualification.ps1 -SelfTest` → exit 0，`M4 guest qualification SelfTest PASS`。本任务的前向状态机与 clone 拒绝条件由**合同断言**与**失败矩阵行**冻结：合同新增 `MissingDriverCreateCaptured`/`DriverLostRecoverCaptured`/`DriverRestored`/`DriverLostRecoverRecovered`/`DriverLostRecoverClone`、`clone path escapes qualification root`、`cloned driver_lost_recover tree is not equal`、`driver_lost_recover clone already exists`、`Blocked is terminal`、`invalid transition`，并断言 `MissingDriverCaptured`（旧状态）不复存在。逐条核验见下。
- 逐条核验（不采信 SelfTest 通过即等于完成）：
  - M4-07.1 四新状态 + forward-only：`grep` 确认四状态在转换表（`:398`、`:1666`）与事件流中一致出现；`M4QualificationBlocked` 出现 101 次；`invalid transition` 与 `Blocked is terminal` 文本存在，且测试断言旧 `MissingDriverCaptured` 已移除。
  - M4-07.2 clone：`Invoke-FormalDriverLostRecoverClone`（`:1164`）复制 Formal bundle 到 `driver_lost_recover` 根，复制前后 `Get-TreeInventory` 全量清单逐字节相等（`:1184`）。
  - M4-07.3 四类 Blocked：kit/commit 经 `Assert-QualificationKit` 校验（`:1170`）；clone 路径 `StartsWith(rootFull+'\')` 防逃逸（`:1176`）；**Formal 原 bundle 保护**——clone 与 Formal 路径双向不互相嵌套（`:1177`）；clone 已存在拒绝（`:1178`）；树不等拒绝（`:1184`）。
  - M4-07.4 严格事件 schema：clone 事件经 `[ordered]` 结构写回（`:1185` 起），状态推进采用 `New-QualificationState ... (([uint64]$s.sequence)+1)`，事件追加后 `Read-QualificationState` 重读。
  - M4-07.5 durable hook 链：`Write-QualificationState` 出现 60 次、`WriteState` 40、`FlushEvent` 40、`ReReadState` 8、`VerifyEvidenceHash` 6，与既有持久化链一致。
- GREEN 证据：
  - `tools/Invoke-M4GuestQualification.ps1 -SelfTest` → exit 0，`M4 guest qualification SelfTest PASS`
  - `tests/unit/test-m4-guest-qualification.ps1 -SourceRoot .` → 通过（全量 ctest 中非失败项）
  - `cmake --build --preset debug --parallel 1` → exit 0
  - `ctest --preset debug` 全量 → 见 E-1：本会话未提权，136/143，失败 7 项均为提权依赖，已用 stash 证明与本改动无关。
- Debug/Release 证据：Debug 见上（未提权子集全绿）。Release 未重跑（改动为纯 PowerShell）。双配置全量属 M4-14，且按 E-1 必须在提权会话。
- Spec/Plan 对齐：Spec §4「Formal 原 bundle 永不作为第三阶段写入目标」→ clone 用独立根 + 双向碰撞拒绝落实；Spec §8「forward-only，Blocked 终态」→ 转换表与 `Blocked is terminal` 断言落实；Spec §7「证据写后重读」→ durable hook 链落实。无偏差。
- 安全检查：`无 VMware/驱动/服务/设备/磁盘/电源动作`。核验过程仅 grep/Read/SelfTest/ctest；未运行 `myrd.exe`、query 工具、`sc.exe`、`pnputil`，未生成 kit，未创建真实资格根目录（SelfTest 用临时目录）。
- 未完成/待接续（如实记录）：见 **D-3**——控制器与 runner 的证据文件名/拒绝协议漂移，属 M4-09 收口。M4-07 不关闭 D-3，只负责 clone 与状态机。
- 结论：`ON_TRACK`（来源已注明并逐条核验；D-3 阻断项已在台账登记并归入 M4-09）
