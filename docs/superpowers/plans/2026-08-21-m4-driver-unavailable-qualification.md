# M4 驱动不可用与断电恢复资格验收实施计划

> **执行说明：** 本计划以 [2026-08-20 M4 驱动不可用与断电恢复验收重设计 Spec](../specs/2026-08-20-m4-driver-unavailable-qualification-design.md) 为唯一设计依据。实施时按任务顺序执行；每项先让新增测试出现预期 RED，再写生产实现到 GREEN。旧 kit、旧资格根目录和旧协议不得迁移或兼容。

**目标：** 在不修改 Windows 内核驱动、也不自动控制 VMware 电源的前提下，发布新的 M4 qualification protocol：`phdskmnt` 服务缺失/禁用时在任何 bundle 操作前以 `driver_unavailable` 拒绝；第三阶段拆分为零写入的 `missing_driver_create` 与克隆 Formal bundle 的 `driver_lost_recover`；仅以 `S:` 与 checkpoint 内容验证恢复；最终 verifier 离线复算全部证据。

**架构：** 原生 `myrd run` 在 `Create`/`Recover` 的 bundle 分类、创建、打开之前做 SCM 服务配置预检；正常 provider 路径仍由既有 `discover_aim_adapter` 负责 AIM 发现。PowerShell runner 生成九个负例的严格证据，控制器以 forward-only 状态机在 Reset3 后编排两个驱动缺失分支、恢复服务、真实恢复和其余七例。verifier 只读取冻结的 kit 与资格根目录，重新计算证据/目录哈希，不调用 `myrd`、不查询当前设备、也不触发电源边界。

**技术栈：** C++20、Win32 SCM (`OpenSCManagerW`/`OpenServiceW`/`QueryServiceConfigW`)、现有 `Result`/错误协议、PowerShell 5.1、CMake/CTest、PowerShell AST 合同测试、MSVC Debug/Release、VMware 人工 Reset 或一次 Power Off→Power On。

---

## 不变量与安全门

1. `driver_unavailable` 只代表两种已知结果：`OpenServiceW(phdskmnt)` 返回 `ERROR_SERVICE_DOES_NOT_EXIST`，或 `QueryServiceConfigW` 读出启动类型 `SERVICE_DISABLED`；它发生在读取、创建、修改或挂载 bundle 之前。`OpenSCManagerW` 失败、`OpenServiceW` 的 `ACCESS_DENIED` 或其他非“不存在”错误、以及 `QueryServiceConfigW` 失败，必须作为带具体操作名的 `ErrorDomain::win32` 错误返回，绝不误报为 `driver_unavailable`。
2. 服务存在但 adapter discovery 失败继续走既有 `discover_aim_adapter: no AIM adapter found` 协议；该路径不承诺零 bundle 写入。
3. AIM 发现只以 `discover_aim_adapter`（StoragePort 接口 + `SPDRP_SERVICE=phdskmnt` + 既有 QUERY_VERSION/唯一性规则）为权威；不新增 `FriendlyName`、Class、PnP 实例号、序列号或 PhysicalDrive 匹配。
4. 成功恢复标准是本轮 AIM 所拥有的 `S:` 出现，且 `A.txt`、`D.txt` 与 Formal 来源 bundle 的最后 checkpoint 内容哈希一致。PhysicalDrive/分区编号、PnP 实例号和显示字符串只写诊断，不参与通过/失败。
5. 所有证据为 UTF-8 无 BOM、单一 JSON、确定属性顺序、CRLF 结尾；写后立即重读、严格解析、重算 SHA-256。`Blocked` 是终态，不自动重试。
6. 全流程恰好三个 VMware 电源边界：Formal、Live Power-Cut、Missing-driver。每段选 Reset 或一次 Power Off→Power On，不能两者都做，不能正常关机。
7. 任一子进程/验证失败只清理本次进程创建且可精确识别的 PID、AIM 设备或盘符；清理错误与主错误合并并持久化。

## 文件与职责映射

| 区域 | 主要文件 | 本次职责 |
|---|---|---|
| 原生预检 | `src/run/main.cpp`、新增 `src/run/driver_preflight.h/.cpp`、`src/CMakeLists.txt` | bundle 操作前的 `phdskmnt` 服务配置预检与固定错误协议 |
| AIM 权威 | `src/driver/adapter_discovery.cpp`、`src/m3/m3_windows_acceptance.cpp` | 不改匹配权威；仅回归确认 discovery 仍留在 provider 启动路径 |
| 第三阶段 runner | `tests/e2e/Prepare-M4NegativeFixtures.ps1`、`tests/e2e/Invoke-M4RunLifecycle.ps1` | 九负例清单、两个驱动缺失场景、严格原始证据与目录清单 |
| 来宾控制器 | `tools/Invoke-M4GuestQualification.ps1` | Reset3 状态机、服务恢复、query 探针、独立阶段证据、严格无重试 |
| 最终复算 | `tools/Invoke-M4Verification.ps1` | 离线复算两种新负例及其目录/内容/哈希绑定 |
| 发布边界 | `tools/New-M4GuestQualificationKit.ps1`、`tests/unit/test-m4-qualification-kit.ps1` | 协议版本与冻结 manifest/文件集合更新 |
| 测试与说明 | `tests/unit/*`、`tests/CMakeLists.txt`、`README.md`、`docs/validation/*`、`tasklist.md` | RED/GREEN、CTest、人工三边界运行指引与结果记录 |

## 实施任务

### 1. 建立原生服务预检的可测策略，并冻结 `driver_unavailable` 协议

**文件：**
- 新增：`src/run/driver_preflight.h`
- 新增：`src/run/driver_preflight.cpp`
- 修改：`src/run/main.cpp`
- 修改：`src/CMakeLists.txt`
- 新增：`tests/unit/m4_driver_preflight_tests.cpp`
- 修改：`tests/CMakeLists.txt`
- 修改：`tests/unit/m4_product_boundary_tests.ps1`

- [ ] 先在 `m4_driver_preflight_tests.cpp` 编写 RED 测试矩阵，并且不通过真实 SCM 服务做断言：

  | 模拟输入 | 预期 |
  |---|---|
  | 服务不存在（`ERROR_SERVICE_DOES_NOT_EXIST`） | `driver_unavailable` |
  | 服务存在，`SERVICE_DISABLED` | `driver_unavailable` |
  | 服务存在，`SERVICE_BOOT_START`、`SERVICE_SYSTEM_START`、`SERVICE_DEMAND_START` 或 `SERVICE_AUTO_START` | 放行 |
  | `OpenSCManagerW`、`OpenServiceW(ACCESS_DENIED)` 或 `QueryServiceConfigW` 失败 | `ErrorDomain::win32`，保留具体操作名，stderr 不包含 `driver_unavailable` |

  测试分别覆盖纯函数分类与可注入 SCM I/O 包装的错误映射；不依赖测试机上实际 `phdskmnt` 服务的存在、权限或启动类型。
- [ ] 定义最小、无 bundle 依赖的接口，例如：

  ```cpp
  enum class driver_preflight_result { available, unavailable };
  driver_preflight_result classify_phdskmnt_service(bool service_found,
                                                    DWORD start_type) noexcept;
  Result<void> require_phdskmnt_service_available();
  ```

  `classify_phdskmnt_service()` 必须是纯函数：只将“服务缺失或 `SERVICE_DISABLED`”归为 `unavailable`，并将 `SERVICE_BOOT_START`、`SERVICE_SYSTEM_START`、`SERVICE_DEMAND_START`、`SERVICE_AUTO_START` 归为 `available`。`require_phdskmnt_service_available()` 只使用可替换的 SCM I/O 包装调用该纯函数：仅 `ERROR_SERVICE_DOES_NOT_EXIST` 映射到“服务缺失”；其他 SCM 失败原样生成 `ErrorDomain::win32`、具体 Win32 error code 和操作名（例如 `OpenSCManagerW`、`OpenServiceW`、`QueryServiceConfigW`）。不要把 adapter handle、StoragePort 枚举、SRB QUERY_VERSION、设备枚举、盘符检查或 bundle 操作移入该模块。
- [ ] 在 `main.cpp` 将 `require_phdskmnt_service_available()` 精确插入在 `require_elevation()` 成功之后、`classify_bundle()` 之前；Create 与 Recover 必须共享同一入口。这保持“提权失败优先于业务错误”的既有错误优先级。
- [ ] 对已知不可用结果，预检必须返回：

  ```cpp
  myrd::Error{
      myrd::ErrorDomain::usage, 1,
      L"driver_unavailable",
      L"phdskmnt is disabled or absent"
  }
  ```

  现有 `print_run_error` 只渲染 `error.operation` 与 `error.message`，所以它必须精确输出：

  ```text
  Error: run failed for <absolute bundle path>: driver_unavailable: phdskmnt is disabled or absent\r\n
  ```

  stdout 必须为空、退出码必须为 1、stderr 必须以 CRLF 结尾。SCM I/O 错误则保留自己的 `ErrorDomain::win32` operation/message，测试必须断言其 stderr 不含 `driver_unavailable`。
- [ ] 在产品边界合同中添加源码顺序断言：预检调用必须位于 `require_elevation()` 成功分支之后并早于 `classify_bundle`、`create_image` 和所有 provider 启动入口；另加协议断言，禁止用 `FriendlyName`/`Class` 作为该预检的设备判断。
- [ ] 运行并记录：

  ```powershell
  cmake --preset msvc
  cmake --build --preset debug --parallel 1 --target myrd_m4_driver_preflight_tests
  ctest --preset debug -R '^unit\.m4\.(driver_preflight|product_boundary)$' --output-on-failure
  ```

**完成条件：** 单元测试覆盖两类已知不可用、四类可用启动类型及 SCM I/O 失败；仅已知不可用映射为固定 `driver_unavailable` 字节协议。产品入口在 `require_elevation()` 之后、任何 bundle 操作之前调用预检；现有 `discover_aim_adapter` 的实现和 provider 中的位置不改变。

### 2. 固化 adapter discovery 的唯一权威与 query 协议

**文件：**
- 修改：`src/driver/adapter_discovery.cpp`（仅在必要时补可测导出/注释，不改变权威逻辑）
- 修改：`src/m3/m3_windows_acceptance.cpp`（仅在必要时补回归钩点）
- 修改：`tests/e2e/m4_aim_query.cpp`
- 修改：`tests/unit/m4_product_boundary_tests.ps1`
- 新增或修改：对应 C++ unit test

- [ ] 先加 RED 合同，明确 `discover_aim_adapter` 仍枚举 `GUID_DEVICEINTERFACE_STORAGEPORT` 并只按 `SPDRP_SERVICE == phdskmnt` 筛选；零或多候选仍由该函数拒绝。
- [ ] 保持 query 的两类固定字节协议：

  ```text
  # driver/AIM discovery 失败
  exit 1; stdout 空; stderr = "discover_aim_adapter failed\r\n"

  # 成功
  exit 0; stderr 空; stdout = 单个合法 JSON，success=true，
  api_version/sub_version 为数值，ids 为数值数组
  ```

- [ ] 在控制器可消费 query 之前添加针对 UTF-8、单 JSON、字段类型、空 stderr、exit code 的 runner/query 合同测试；拒绝数组、额外 JSON、BOM、错误换行、非数值版本和非数值 `ids`。
- [ ] 明确测试服务可用但 provider discovery 失败时仍保持已有 `discover_aim_adapter: no AIM adapter found`，且测试不主张 bundle 零写入。

**完成条件：** 没有并行 PnP 匹配策略；query 的成功/失败输出可由控制器以严格字节和类型规则消费。

### 3. 先改 runner/fixture 为九负例和生产形状证据

**文件：**
- 修改：`tests/e2e/Prepare-M4NegativeFixtures.ps1`
- 修改：`tests/e2e/Invoke-M4RunLifecycle.ps1`
- 修改：`tests/CMakeLists.txt`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`

- [ ] 先让负例 runner 自测 RED：旧的单一 `missing_driver` 记录不能满足新 manifest 的九 case 集合。
- [ ] 将 fixture manifest 固定为恰好九项：

  ```text
  unknown_nonempty, size_mismatch, occupied_letter, gen0, writer_lock,
  missing_driver_create, driver_lost_recover, busy_volume, compact_lock
  ```

  每项带固定 schema、绝对 case 路径、相对树清单和所需输入常量；拒绝缺项、额外项、非字符串数组元素、非规范 UTF-8 或非 CRLF JSON。
- [ ] `missing_driver_create` 必须从不存在的 case 根开始；运行失败后继续断言根目录不存在，且没有该 case 的 bundle、journal、AIM 设备或盘符记录。
- [ ] `driver_lost_recover` 的输入必须是控制器从已验证 Formal bundle 复制出的专用副本。runner 只接受并核验完整相对路径/长度/SHA-256 目录清单；拒绝前后树必须逐项相等，包含 journal。
- [ ] 为 `driver_lost_recover` 的正常恢复产出生产形状的 evidence：本次 owned AIM/`S:` 绑定、`A.txt`/`D.txt` 哈希、来源 Formal checkpoint 哈希、run exit/stderr、清理结果和诊断性设备信息。PhysicalDrive/分区/PnP 文本只能放诊断字段，不能进入成功 predicate。
- [ ] 为服务预检拒绝写出两个独立负例记录：`missing_driver_create` 的零写入证明与 `driver_lost_recover` 的不变树证明；固定 `driver_unavailable` stderr，不把 query stderr 与 run stderr 字面比较。
- [ ] 新增 runner 自测的正例与 mutation 矩阵：截断 evidence、非规范 CRLF/BOM、路径逃逸、目录清单成对伪造、错误 source hash、错误 stdout/stderr、PID/退出码/数组元素类型、清理失败与重复执行。

**完成条件：** 真实 runner 和其 selftest 生产同一套严格 schema；两种驱动缺失拒绝不会被旧 `missing_driver` 假记录掩盖。

### 4. 将来宾控制器升级为 Reset3 的 forward-only 状态机

**文件：**
- 修改：`tools/Invoke-M4GuestQualification.ps1`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`
- 修改：`tests/CMakeLists.txt`（需要新的 CTest/超时才修改）

- [ ] 先为每个新状态、事件和合法转换写 RED：至少表达“Formal 副本就绪 → 等待 Missing-driver Reset → Reset 已观测 → `missing_driver_create` 已捕获 → `driver_lost_recover` 拒绝已捕获 → 驱动已恢复 → 副本已真实恢复 → 七个其余负例已完成 → NegativeComplete”。任何未定义转换、相同 root 重试或 `Blocked` 恢复必须被 typed `M4QualificationBlocked` 拒绝。
- [ ] 在 Formal 成功后创建 `driver_lost_recover` 专用副本，重算完整树清单并将 Formal source bundle、克隆清单与哈希写入 durable event；禁止第三阶段写 Formal 原 bundle。
- [ ] Reset3 的入口先只读取 boot/event/服务负状态，same-boot 必须以规定 exit 返回且不调用 query、runner、driver、fixture 或写入状态。确认 new boot 后按 Spec 的固定顺序执行两个缺驱动 case、恢复服务、query 探针、真实恢复、剩余七例。
- [ ] `missing_driver_create` 阶段同时调用 run 与 query，严格确认 query exit 1/run exit 1；任一 exit 0、其他退出码、超时或字节协议不符立即独立持久化 `Blocked`。
- [ ] `driver_lost_recover` 阶段验证副本树在拒绝前后相等。恢复时只执行：

  ```powershell
  sc.exe config phdskmnt start= demand
  sc.exe start phdskmnt
  ```

  严格解析/验证 `DEMAND_START`、`RUNNING` 与 query 成功 JSON。不得加入 `pnputil.exe /restart-device`；先在真实 VM 前置实验中证明 `sc start` 足够。
- [ ] 真实恢复后验证 `S:` 是本轮 owned AIM 挂载、`A.txt`/`D.txt` 与 Formal 来源 bundle 的最后 checkpoint 哈希一致；不再比较 PhysicalDrive/分区编号。
- [ ] 每一子步骤调用现有 durable hook（写状态、flush event、重读、验 hash）后才进入下一步；任一失败的 evidence 中写明确子步骤/主错误/清理错误，并封存 root 为 `Blocked`。
- [ ] 扩展控制器 SelfTest，使用 fresh root、fake kit、fake service/query/runner/observer，在生产 `Invoke-QualificationRun` 路径执行：成功完整 Reset3、same-boot 零写入、服务/查询不一致、每个拒绝证据 mutation、SC 配置失败、SC 启动失败、query 不合规、真实恢复内容不匹配、七负例中任一失败、清理失败、Blocked 重调零副作用。
- [ ] 记录并断言每个失败点的精确 seam 向量和完整调用顺序，而不是仅断言“调用次数大于零”。

**完成条件：** 控制器在同一个资格根目录内只能前进或封存；每一个 Reset3 子步骤都有独立可重算 evidence，且不再把物理盘号当验收条件。

### 5. 扩展最终离线 verifier，禁止再次触碰产品与 VM

**文件：**
- 修改：`tools/Invoke-M4Verification.ps1`
- 修改：`tests/unit/test-m4-guest-qualification.ps1` 或新增 `tests/unit/test-m4-verification.ps1`
- 修改：`tests/CMakeLists.txt`

- [ ] 先编写 RED：缺少任一新增 driver-unavailable evidence、将 `driver_lost_recover` 的目录清单成对伪造、将来源 checkpoint 哈希替换、将 `S:` ownership 或 A/D 哈希替换时，verifier 必须失败。
- [ ] 添加纯读取验证函数，严格复算并绑定：kit manifest/source commit、Formal clone source、两个缺驱动 evidence、拒绝前后树、恢复后 `S:`/A/D 哈希、全部九个 case、final lifecycle evidence 与所有状态事件哈希。
- [ ] verifier 不得调用 `myrd.exe`、`myrd_m4_aim_query.exe`、SCM、`Get-PnpDevice`、`Get-Disk`、`Get-WinEvent`、VMware、Reset 或任何会改变当前来宾的操作。用 AST/命令 allowlist 合同测试固定该只读边界。
- [ ] 为 raw bytes、额外字段、错误类型、错误数组元素、路径不在资格根内、重复/缺失事件、错误 CRLF、错误 hash 和旧 protocol version 添加 mutation 测试。

**完成条件：** 完整资格结论可在离线副本上复算；verifier 的通过不依赖当前 VM 的设备枚举或运行中 `myrd`。

### 6. 发布新 kit 协议并更新本地验证链

**文件：**
- 修改：`tools/New-M4GuestQualificationKit.ps1`
- 修改：`tests/unit/test-m4-qualification-kit.ps1`
- 修改：`tools/Invoke-M4GuestQualification.ps1`
- 修改：`tests/CMakeLists.txt`

- [ ] 先让 kit 合同 RED：旧 protocol/旧固定文件列表不能装载含新 runner、controller、verifier 的版本。
- [ ] 升级 manifest schema 版本并固定精确文件集合、顺序、相对路径、大小、SHA-256、source commit、artifact 与 query 哈希；控制器启动时拒绝旧 schema、缺项、额外项、大小/哈希偏差和同名预存在输出目录。
- [ ] 用隔离临时目录测试 packager 的成功输出与每种篡改：脚本、`myrd.exe`、query、manifest 字段、文件顺序、BOM、长度、hash、source commit 和额外文件。
- [ ] 更新 CTest 标签，使新的原生、runner、controller、verifier、kit 合同均纳入 `m4-v3`；设置足以覆盖完整控制器矩阵的显式超时，而不是降低测试覆盖。

**完成条件：** 新 kit 与旧 kit 不能混用；复制进 VM 前后可逐文件验证相同 manifest 与哈希。

### 7. 完整本地验证、生成 kit，并执行受控 VM 三边界验收

**文件：**
- 修改：`README.md`
- 修改：`docs/validation/*`（新增本次验收记录）
- 修改：`tasklist.md`

- [ ] 完成所有代码任务后，先运行 AST、直接合同和双配置测试：

  ```powershell
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-guest-qualification.ps1 -SourceRoot .
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-qualification-kit.ps1 -SourceRoot .
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\Invoke-M4GuestQualification.ps1 -SelfTest
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\Invoke-M4Verification.ps1 -SelfTest
  cmake --preset msvc
  cmake --build --preset debug --parallel 1
  cmake --build --preset release --parallel 1
  ctest --preset debug -L m4-v3 --output-on-failure
  ctest --preset release -L m4-v3 --output-on-failure
  git diff --check
  git status --short
  ```

- [ ] 在宿主机生成一个以 commit + GUID 命名、此前不存在的 kit 目录；使用真实 Release artifact/query 与四个 mandatory 参数，重新读取 manifest 并逐文件比 hash。不要覆盖 VM 上的旧 kit。
- [ ] 在全新 VM 上先做一次独立前置实验：将 `phdskmnt` 从 DISABLED 改为 demand 并 `sc start`，然后运行 query，记录它是否足以恢复 query 成功。若不够，停止发布和 VM 正式资格流程，回到新的设计决策；不要加入 `pnputil` 作为临时补丁。
- [ ] 仅在前置实验成功后，把整个新 kit 复制到 VM 的新目录，创建新的 qualification root，并按手册执行恰好三段：

  1. Formal：控制器输出 armed 后执行一次 Reset（或仅一次 Power Off→Power On），再次运行同一命令；
  2. Live Power-Cut：同样只做一个人工电源边界；
  3. Missing-driver：控制器输出 armed 后只做一个人工电源边界，随后由控制器自动完成两个缺驱动 case、服务恢复、真实恢复和七个其余负例。

- [ ] 每一步保存命令、退出码、控制器输出、资格根目录 evidence hash 与 verifier 输出。Windows “正在诊断/自动修复/继续使用 Windows”不自动判失败；只以 Spec 的新 boot、事件、服务/查询、证据和恢复内容判断。
- [ ] 仅在三边界、所有本地测试、新 kit 完整性和离线 verifier 全部通过后，记录“VM 已资格验证”；任一项未完成只能记录“本地验证通过”或“VM 验收未完成”。

**完成条件：** 有可复现的本地与 VM 证据链；从未将旧资格根、旧 kit 或物理磁盘编号误作成功依据。

## 建议提交切分

1. `test(m4): specify driver unavailable preflight contract` — 原生 RED 测试与边界合同。
2. `fix(m4): reject disabled phdskmnt before bundle access` — 原生预检实现。
3. `test(m4): model split driver-unavailable negative fixtures` — runner/fixture strict schema 与测试。
4. `feat(m4): orchestrate missing-driver qualification recovery` — 控制器状态机、服务恢复和完整 fake flow。
5. `feat(m4): verify split driver-unavailable evidence offline` — verifier 复算。
6. `fix(m4): freeze driver-unavailable qualification kit` — kit schema/contract。
7. `docs(m4): document three-boundary driver-unavailable qualification` — 人工验收说明与结果账本。

## 最终验收清单

- [ ] 原生服务预检先于所有 bundle/AIM/目标盘操作，且服务缺失/禁用返回固定 `driver_unavailable`。
- [ ] `discover_aim_adapter` 仍是唯一 adapter 发现权威；没有 `FriendlyName`/Class/PnP/PhysicalDrive 的第二匹配器。
- [ ] 九负例完整、两种 driver-unavailable 场景各有独立严格证据。
- [ ] `missing_driver_create` 证明 case 根不存在；`driver_lost_recover` 证明拒绝前后完整树不变、恢复内容与 Formal 来源 checkpoint 一致。
- [ ] 控制器严格执行 Reset3 顺序、每个子步骤可定位地 Blocked、`Blocked` 无重试。
- [ ] 恢复仅用 `sc config`/`sc start`，并已由单独 VM 前置实验验证 query 恢复；未调用 `pnputil`。
- [ ] verifier 纯离线、严格重算新旧所有证据和哈希。
- [ ] 新 kit 协议冻结且与旧 kit/root 不兼容。
- [ ] Debug/Release 全部 `m4-v3` 通过；VM 使用全新 kit/root 完成恰好三个人工电源边界。
