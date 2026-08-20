# M4 驱动不可用与断电恢复验收重设计 Spec

日期：2026-08-20
状态：待用户审核
范围：M4 产品运行协议、Windows 来宾验收控制器、第三阶段证据与最终离线 verifier

## 1. 目标与非目标

本设计解决真实 VMware 来宾中第三阶段反复出现的 `missing_driver` 失败。根因是旧流程在 AIM/`phdskmnt` 不可用时仍先进入 Create 路径，导致 bundle 产生 `base-0000.vhdx`、`disk.rdm`、journal 等副作用，随后才报告 adapter discovery 失败；旧验收又把易变的物理磁盘枚举当作恢复成功条件。

目标：

1. 驱动或 AIM 不可用时，在任何 bundle/AIM/目标盘操作前确定性拒绝，并保证零 bundle 写入。
2. 将旧 `missing_driver` 拆成两个可观测场景：空 bundle 的创建拒绝，以及完整 checkpoint bundle 的驱动丢失拒绝与恢复。
3. 断电后的真正成功条件是 RAM 盘稳定映射回目标盘符 `S:` 且 checkpoint 内容一致，不依赖 VMware 物理磁盘编号、PnP 实例号等易变枚举。字面上的 PhysicalDrive 编号不属于验收条件。
4. 保持 Formal 与 Live Power-Cut 的既有成功目标，仅替换第三阶段不可靠的判定。
5. 让最终 verifier 只读、离线、可重复复算，不重新运行产品、不触发电源边界。

非目标：本 Spec 不改变 Windows 内核驱动实现本身，不引入自动 VM 电源控制，不迁移旧资格根目录或旧 kit。

## 2. 协议版本与发布边界

本设计发布新的 M4 qualification protocol 版本。旧 kit、旧证据和旧资格根目录只可用于诊断，不能被新 verifier 接受；不得提供自动迁移或兼容分支。任何源码、artifact、query 或脚本输入变化都必须生成新 kit 和新的资格根目录。

kit 在生成时冻结 source commit、artifact/query 以及全部脚本的 SHA-256。复制到 VM 后先验证 manifest 与每个文件哈希；运行期间不得覆盖 kit。发布 kit 与实际运行 kit 必须逐文件哈希相等。

## 3. 运行前驱动/AIM 预检

### 3.1 已知驱动不可用的廉价服务判定

`myrd run` 的 Create 与 Recover 都必须先执行同一项廉价服务预检，且预检完成前不得读取、创建、修改或挂载 bundle：

1. 通过 `QueryServiceConfig(phdskmnt)` 检查服务是否存在以及是否被配置为 `DISABLED`。服务缺失或 `DISABLED` 时，立即使用 `driver_unavailable` 协议。
2. 此判定只解决已知的服务缺失/禁用情形；它不以服务当前是否 `RUNNING` 代替产品启动语义，也不读取 bundle。

### 3.2 既有 adapter discovery 的保留位置

服务预检通过后，继续沿用产品已有的 `discover_aim_adapter` 作为唯一设备发现权威。该函数枚举 `GUID_DEVICEINTERFACE_STORAGEPORT` 接口，并以 `SPDRP_SERVICE=phdskmnt`、QUERY_VERSION 及现有的零个/多个候选拒绝逻辑为准；Spec 不再定义第二套基于 `Class`、`FriendlyName`、固定 PnP 实例号或其他显示字段的匹配规则。

全量 `discover_aim_adapter` 保持在 provider 启动流程中的既有位置，用于覆盖“服务在但 adapter 不在、QUERY_VERSION 失败或版本不匹配”等更宽失效模式，并沿用既有 `discover_aim_adapter: no AIM adapter found` 协议。除非另有产品负责人决定，不把完整 discovery、SRB QUERY_VERSION 或 `AdapterHandle` 生命周期前置为 `driver_unavailable` 的判定条件。

已知驱动不可用（服务缺失或禁用）统一协议：

```text
exit 1
stdout: 空
stderr: Error: run failed for <绝对 bundle 路径>: driver_unavailable: phdskmnt is disabled or absent\r\n
```

此协议对 `missing_driver_create` 与 `driver_lost_recover` 完全相同。发生该错误时不读取 bundle，也不创建 AIM 设备、目标盘符或任何 bundle 文件。服务看似可用但完整 adapter discovery 失败时，使用既有协议 `discover_aim_adapter: no AIM adapter found`，同样要求在 bundle 操作前失败。

查询工具 `myrd_m4_aim_query.exe` 保持独立只读协议：exit 1、stdout 为空、stderr 严格为 `discover_aim_adapter failed\r\n`。第三阶段必须运行该交叉检查；若与 `driver_unavailable` 结果不一致，控制器立即 `Blocked`。

服务预检通过后若 provider 内部 discovery 或后续挂载因竞态失效，允许保留已有 bundle，但必须清理本次创建的 AIM 设备和目标盘符；该情况是普通运行时失败，不得伪装为已知 `driver_unavailable`。

## 4. 第三阶段场景与执行顺序

负例总数由 8 个增至 9 个：

1. `unknown_nonempty`
2. `size_mismatch`
3. `occupied_letter`
4. `gen0`
5. `writer_lock`
6. `missing_driver_create`
7. `driver_lost_recover`
8. `busy_volume`
9. `compact_lock`

Reset3 后，在同一 Windows 启动内严格按以下顺序执行：

1. 验证新启动、事件和驱动/PnP 负状态。
2. 执行 `missing_driver_create`。
3. 执行 `driver_lost_recover`。
4. 恢复 `phdskmnt`，并确认既有 adapter discovery 已再次可用。
5. 对 `driver_lost_recover` 副本执行真实恢复并验证 checkpoint 内容。
6. 继续其余七个负例。

这是全状态机中复杂度最高、最需要真实 VM 验证的编排段。每个子步骤（启动事实、两个缺驱动拒绝、驱动恢复、真实恢复、七个剩余负例）必须各自写入独立的阶段证据；链式流程中的任一步失败，都必须先写出该子步骤的明确 `Blocked` 记录，再封存整个资格根目录，不得只留下一个无法定位的链式错误。

任一步失败即持久化 `Blocked`，不自动重试、不跳过、不继续。资格根目录进入 `Blocked` 后永久封存；后续运行只报告该状态，重新测试必须使用新的资格根目录。

Formal 成功后，立即将已验证的 Formal bundle 完整复制为 `driver_lost_recover` 专用副本，并记录创建前后完整树清单（相对路径、文件长度、SHA-256）。Formal 原 bundle 永不作为第三阶段写入目标。

## 5. 两个缺驱动负例的证据

### 5.1 `missing_driver_create`

初始 case 根目录必须不存在。驱动预检失败后，case 根目录仍不存在；没有 bundle 文件、AIM 设备、目标盘符或 journal。验证只覆盖受管 case 根目录，不把控制器证据目录、临时文件或 Windows 事件日志纳入零写入比较。

### 5.2 `driver_lost_recover`

初始 case 是 Formal 副本。驱动预检失败后，完整 bundle 树的相对路径、文件长度和 SHA-256 必须完全不变；本次拒绝产生的 journal 也不得改变。驱动恢复后运行该副本，成功条件为：

- `myrd run` exit 0，stderr 为空；成功提示文字不冻结；
- `S:` 出现并属于本次 AIM 挂载；
- `A.txt`、`D.txt` SHA-256 与 Formal 副本来源 bundle 的最后 checkpoint 内容完全一致；该场景的唯一内容比较基准就是这份来源 bundle；
- 其余全局磁盘编号、分区编号、PnP 实例号和日志文字只保存为诊断，不作为通过条件。

## 6. 驱动恢复与 Windows 边界

人工只操作 VMware 电源边界，控制器负责恢复驱动。恢复顺序首先只执行并验证服务动作：

```text
sc.exe config phdskmnt start= demand
sc.exe start phdskmnt
```

随后必须验证 `DEMAND_START`、`RUNNING` 以及既有 `discover_aim_adapter` 已再次可用。实施前必须在干净 VM 上做一项独立前置实验，确认 `sc start phdskmnt` 足以恢复 adapter discovery；在该实验证实前，资格控制器不得调用 `pnputil.exe /restart-device`，以免引入设备实例匹配和输出解析的第二条失败链。若实验表明 `sc start` 不足，此设计须先增补单独的设备恢复决策与测试，再开始实现；本 Spec 当前不授权该命令进入资格运行路径。

设备身份只能来自产品 `discover_aim_adapter` 的 STORAGEPORT 接口枚举与 `SPDRP_SERVICE=phdskmnt` 过滤。PNPDeviceID、FriendlyName、Class、序列号、PhysicalDrive 编号、分区编号及无关盘符只作诊断，不能成为第二套匹配权威。

Windows 在 Reset3 后出现“正在诊断”“自动修复”或“继续使用 Windows”不自动判失败；只要最终可登录，并满足新启动、事件 41/6008、`phdskmnt` 服务缺失或 `DISABLED`、query 拒绝、无 AIM 盘、`S:` 不存在，即可继续。显示名称、PnP 实例及其错误代码只作诊断，不是判定前提。

全流程恰好三个 VMware 电源边界：Formal 一次、Live Power-Cut 一次、Missing-driver 一次。每阶段优先 VMware Reset；可用 Power Off→Power On 替代同一边界，但不可同阶段两者都做，也不可正常关机 Windows。

## 7. 状态、证据与清理

所有阶段性证据使用 UTF-8 无 BOM、单个 JSON、确定属性顺序和 CRLF 结尾。写入后立即重新读取、解析并计算 SHA-256；状态事件保存证据绝对路径与哈希。后续阶段及最终 verifier 必须重新计算并完全匹配。缺失、额外字段、错误类型、非规范字节或哈希不一致均 `Blocked`。

任何阶段的子进程超时、格式错误或验证失败，控制器只针对自己启动的精确 PID 做有界终止并等待；若创建了 AIM 设备或 `S:`，只清理本次拥有的资源。清理失败必须与原始错误合并写入 `Blocked` 证据，不得静默忽略、按名称杀进程或删除未知设备。

## 8. 状态机与 verifier

状态机为 forward-only；`Blocked` 为终态，不允许从同一资格根目录恢复或重试。每次状态转换必须写入、flush、重读并验证事件哈希后才能继续。

最终 verifier 为纯证据复算器：只读资格根目录中的证据与哈希；复算拒绝协议、受管 bundle 树、驱动/PnP 状态记录、恢复后的 `S:` 绑定和 `A.txt`/`D.txt` 哈希。不运行 `myrd`，不查询当前设备，不触发 Reset，不依赖当前 VM 的可变枚举。

## 9. 端到端人工验收门槛

只有同时满足以下条件，才称为“VM 已资格验证”：

1. 本地单元、自测以及 Debug/Release 测试全部通过；
2. 生成并独立验证新 kit；
3. 干净 VM 使用新 kit、新资格根目录完成恰好三个电源边界；
4. 三阶段证据与恢复内容满足本 Spec；
5. 最终离线 verifier 通过。

否则只能报告“本地验证通过”或“VM 验收未完成”，不得宣称 M4 已通过。

## 10. 实施范围与验证策略

实施需要同步更新：产品 run/recover 的 AIM 预检与错误协议、第三阶段 runner/fixture、来宾控制器状态机与证据校验、最终 verifier、kit manifest/版本，以及对应单元和集成自测。实现遵循先写失败测试再修复的 TDD 顺序；每个新边界至少有正例、类型/字节篡改负例、清理失败负例和无重试断言。

实施完成后先进行本地 AST/契约/Debug/Release 验证，再生成新 kit，最后在全新 VM 上执行人工三边界测试。旧资格目录和旧 kit 不得混入结果。
