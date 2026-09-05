# MyRamDisk M5 Supervisor 与 Windows Service 设计规范（Draft）

状态：设计草案，供评审；不构成已实现功能的承诺  
日期：2026-09-06  
前置版本：M4 单盘/多实例 CLI 已通过 VM 资格验收，`myrd.exe` 仍依赖已签名的
Arsenal Image Mounter `phdskmnt` 适配器。

## 1. 目标与设计结论

M5 将 M4 的 `DiskRuntime` 包装为一个本机 Windows 服务管理的多盘系统。用户
通过 profile 声明“哪些磁盘应在开机时恢复”，服务为每个 profile 建立一个相互
独立的 runtime，并提供本机 CLI 查询和控制。

M5 的最小可交付结果是：

1. 在受保护的 `ProgramData` 目录保存、校验和原子更新磁盘 profile。
2. 由 `DiskSupervisor` 管理多个互不共享可变状态的 `DiskRuntime`。
3. 提供 `disk list/start/stop/checkpoint` 以及 profile 注册/启停命令。
4. 安装 `MyRamDiskService`，在 Windows 启动后读取启用 profile 并逐盘恢复。
5. 使用受 ACL 保护的本机命名管道提供 CLI 到服务的 IPC；变更操作需要管理员。
6. 在服务停止、系统关机和服务异常时，按 M4 语义尽力完成 checkpoint 和清理，
   一个磁盘失败不得停止或清理其他磁盘。
7. 对 worker 退出、profile 损坏、盘符冲突、驱动不可用等情况进行可诊断的、
   fail-closed 的处理，并在 VMware Windows 11 虚拟机上重复验收。

M5 不改变 M3/M4 的 bundle 格式、checkpoint 算法、恢复选择规则或 AIM 适配器
边界；它只增加配置、编排、服务和运维接口。

## 2. 范围

### 2.1 必须实现

- `DiskProfile` v1 的 JSON 持久化、严格校验、原子更新和 ACL。
- `DiskSupervisor` 的 profile 加载、状态机、生命周期并发控制和故障隔离。
- 本机 `myrd disk` CLI；旧的前台 `myrd run` 命令保持兼容。
- `MyRamDiskService` Windows Service，以及启动/停止控制。
- 开机自动恢复 `auto_start=true` 的 profile。
- `SERVICE_CONTROL_PRESHUTDOWN`/停止顺序、最终 checkpoint 和有界超时。
- 本机命名管道 IPC、调用者鉴权、请求序列号、错误分类和超时。
- 每盘 worker 退出检测、资源归属清理、状态持久化/日志和显式重启。
- M4 bundle 的只读导入/注册和 M5 profile 的撤销；不迁移 bundle 内容。
- 面向上述行为的单元、集成、服务和 VMware 验收证据。

### 2.2 非目标

M5 不实现以下功能：

- GUI、托盘程序、远程管理、跨主机或云同步。
- 替换、重新编译、分发或自动安装 `phdskmnt`/AIM 驱动。
- 加密 profile/bundle、签名 seal、密钥管理或复制到其他设备。
- VSS、应用级事务一致性、数据库备份或逐写入同步持久化。
- 在线 compaction、deduplication、compression、parent VHDX 或复制。
- 跨用户共享同一磁盘、登录会话感知或网络用户授权。
- 无限自动重试、无人值守的 bundle 重建、格式化或“修复为零”。
- 复杂安装器、自动升级服务和跨版本在线迁移；安装步骤可由管理员手工执行。

## 3. 继承的 M3/M4 契约

### 3.1 持久化语义

M5 必须调用已经验收的 `DiskRuntime`/M3 provider，不得在 supervisor 中复制
manifest、journal 或 VHDX 逻辑。对一个 profile：

```text
恢复结果 = 最后一次成功提交 checkpoint 的完整逻辑磁盘状态
```

checkpoint 之后的写入允许丢失，也允许在故障时仍存在；两者都不是 M5 失败。
恢复只能选择完整的旧 generation 或完整的新 generation，严禁混合 generation、
把损坏块当零、重新分区、重新格式化或静默创建空 bundle。

M5 在 checkpoint 前必须依赖 M4 的卷 flush barrier；`Committed`、
`NotCommitted`、`CommitIndeterminate` 和 `CommittedNeedsReconciliation` 的
处理规则不变。后两种未解决状态不得被服务当成普通成功；必须阻止下一次持久化
操作并显示可诊断错误。

### 3.2 资源归属

每个 runtime 独占以下资源：一个 profile id、一个 bundle writer lock、一个 AIM
设备、一个 NTFS volume、一个目标盘符、一个 worker/`DiskRuntime` 和自己的日志
上下文。Supervisor 不按进程名、PhysicalDrive 编号或显示名称进行清理，只按
runtime 保存并再次验证的资源身份清理。

`phdskmnt` 是外部运行前置条件。服务只检查驱动/适配器是否可用，不安装、重启、
卸载或替换驱动。前置条件不满足时，相关 profile 进入 `Blocked`，其他 profile
仍继续启动。

## 4. 总体架构

```text
myrd.exe disk ...
        │  length-prefixed UTF-8 JSON over `\\.\pipe\myrd-supervisor-v1`
        ▼
MyRamDiskService (LocalSystem, one process)
        │
        ├── ProfileStore (ProgramData, strict JSON, ACL, atomic replace)
        ├── DiskSupervisor (one state machine per profile)
        │       ├── Worker A -> M4 DiskRuntime -> M3 provider -> AIM/phdskmnt -> R:
        │       └── Worker B -> M4 DiskRuntime -> M3 provider -> AIM/phdskmnt -> S:
        └── Structured event/log sink
```

只有 service 可以创建或销毁 AIM 设备、打开 bundle 或改变 profile runtime。CLI
不直接操作 bundle、Windows volume 或设备；它只发送请求并展示 service 的响应。
旧 `myrd run` 仍作为 M4 兼容入口保留，用于前台、诊断和回滚；它不自动加入
Supervisor 的 profile 集合。

## 5. Profile v1

### 5.1 文件位置和保护

profile 位于：

```text
C:\ProgramData\MyRamDisk\profiles\<profile-id>.json
```

`<profile-id>` 为小写 UUID，文件名必须与 JSON 中的 `profile_id` 完全一致。目录
和文件使用以下 ACL：SYSTEM 和本机 Administrators 拥有完全控制；普通用户没有
写入、删除或替换权限。服务以 LocalSystem 运行。CLI 不依赖“管理员不会修改
文件”的假设：任何读取都重新验证 schema、路径、几何和字段约束；无效 profile
进入 `Blocked`，不得被静默修正。

profile 更新采用：写入同目录随机临时文件 → 写满并 flush → 重新读取并校验 →
使用原子 replace 替换目标文件。临时文件不是有效 profile，服务启动时忽略并
记录事件；不得直接原地覆盖正在使用的 JSON。每次修改递增 `revision`。

M5 不提供密码或加密保护；管理员仍被视为本机受信任的配置拥有者。保护目标是
防止普通用户、半写入文件和服务重启时的部分更新，而非抵抗本机管理员。

### 5.2 JSON schema（严格 v1）

以下字段是唯一允许的字段；未知字段、重复键、错误类型、非规范 UUID、超出范围
的数值和尾随非空字符都必须拒绝：

```json
{
  "schema": "myrd.profile.v1",
  "profile_id": "4d7c0c5a-0e0e-4be3-8eb3-1bb2a4b5a7d1",
  "bundle_path": "C:\\RamDiskData\\disk-a",
  "size_bytes": 67108864,
  "drive_letter": "R:",
  "auto_start": true,
  "checkpoint_seconds": 60,
  "checkpoint_dirty_bytes": 67108864,
  "revision": 1,
  "created_utc": "2026-09-06T00:00:00Z",
  "updated_utc": "2026-09-06T00:00:00Z"
}
```

字段约束：

| 字段 | 约束 |
| --- | --- |
| `schema` | 精确为 `myrd.profile.v1` |
| `profile_id` | 小写 UUID v4；不可在更新中改变 |
| `bundle_path` | 绝对本地 NTFS 路径；规范化后不得是设备路径、UNC、reparse point、RAM disk、profile 目录或任一其他 profile 的 bundle |
| `size_bytes` | 64 MiB 至 1 TiB，按 M4 支持的逻辑扇区/几何对齐 |
| `drive_letter` | `A:` 到 `Z:` 的大写单字母加冒号；每个 profile 唯一 |
| `auto_start` | 布尔值；决定开机是否尝试恢复 |
| `checkpoint_seconds` | 5 至 86400 的整数；与 M4 CLI 的最小值一致 |
| `checkpoint_dirty_bytes` | 1 MiB 至 1 TiB 的整数，并不超过 `size_bytes`；与 M4 CLI 上限一致 |
| `revision` | 从 1 开始的严格递增整数 |
| 时间字段 | UTC RFC3339，服务只写入，不用于恢复选择 |

配置验证还必须检查：bundle 所在卷是持久化 NTFS；bundle 路径不是目标盘符；
同一 drive letter、bundle canonical path 或 profile id 不重复；目标盘符在当前
启动时未被外部卷占用。占用检查只阻止该 profile，不改变外部卷。

### 5.3 状态不是恢复权威

profile 只表达期望配置，绝不保存 `generation`、AIM disk number、PhysicalDrive
编号、volume serial 或“上次挂载成功”的布尔值作为恢复权威。每次启动都从 bundle
manifest 和当前设备重新发现/校验身份；generation 只显示为观测结果。服务状态
可以写到日志或短生命周期状态文件，但删除它不得影响恢复。

## 6. CLI 与服务命令契约

### 6.1 命令

```text
myrd disk list [--json]
myrd disk add --id <uuid> --bundle <path> --size <size> --drive-letter <L:> [--auto-start] [--create]
myrd disk remove <id> [--keep-bundle]
myrd disk enable <id>
myrd disk disable <id>
myrd disk start <id>
myrd disk stop <id>
myrd disk checkpoint <id>
myrd service status [--json]
```

- `disk add` 默认只注册一个已有且通过只读验证的 bundle；创建新 bundle 必须显式
  使用 `disk add --create`（该开关仅允许目录不存在或为空，禁止格式化非空无效目录）。
  创建/注册成功后才原子发布 profile；任何失败都不留下可自动启动的半 profile。
- `remove` 只删除 profile，默认保留 bundle；运行中的 profile 必须先 stop。
  删除 bundle 永远不是 M5 CLI 的隐含行为。
- `enable/disable` 只改变 `auto_start`，不能停止正在运行的盘；`disable` 后下次
  启动不再恢复，但当前实例保持到显式 `stop`。
- `start`、`stop`、`checkpoint` 对同一 profile 串行；对不同 profile 可并行。
- `stop` 使用 M4 正常 shutdown policy（flush、final checkpoint、dismount、删除
  自有盘符、停止 runtime）；没有“强制删除 bundle”选项。
- `list` 返回 profile 配置和实时状态；实时状态不可用时返回 `Unknown/ServiceUnavailable`
  而不是猜测。

保留的 M4 命令：

```text
myrd run --bundle <path> --size <size> --drive-letter <L:> ...
```

它仍是前台兼容/诊断接口，不读写 M5 profile，不受 service 的 start/stop 请求控制。

### 6.2 返回值和错误分类

CLI 进程返回：`0` 成功；`1` 产品/运行失败；`2` 参数或 schema 错误；`3` IPC
不可用/服务未运行；`4` 权限不足；`5` 超时或结果未确定。stderr 必须包含稳定
错误类别和 profile id（不输出秘密）。`CommitIndeterminate`、服务断开或请求超时
不得被 CLI 改写成成功。

## 7. IPC 与权限

### 7.1 通道

服务监听：

```text
\\.\pipe\myrd-supervisor-v1
```

每条消息为 4 字节 little-endian 长度加 UTF-8 JSON，最大 64 KiB；单请求必须有
`request_id`、`operation`、`profile_id`（`list/status` 可为空）和 `schema`。
服务逐请求回复成功或失败对象，客户端断开不等于操作取消。单请求默认 30 秒，
checkpoint/stop 允许 120 秒；超时只表示结果未知，服务继续完成或清理该操作并
通过日志/后续 `list` 报告最终状态。

### 7.2 鉴权

推荐使用两个本机管道：控制管道 DACL 仅允许 SYSTEM 和本机 Administrators，查询管道
允许已认证的本机用户读取。若实现只使用一个管道，则 DACL 可允许已认证用户连接，
但服务必须 impersonate 客户端并逐请求拒绝非管理员的写操作；不能仅依赖客户端自报身份。
服务在每个变更请求上 impersonate 客户端并检查本机 Administrators 成员资格及
已提升 token；仅 `list`、`service status` 对普通本机用户可用。服务不信任客户端
提供的路径身份、PID、AIM 编号或盘符归属，所有值重新从 profile/runtime 校验。

拒绝变更请求时不触碰 bundle、volume 或 profile；拒绝本身写入审计事件（调用者
SID、操作、profile id、原因）。IPC 只在本机工作，不监听 TCP/命名网络管道。

## 8. DiskSupervisor 状态机

每个 profile 有独立的状态和互斥操作锁。状态定义：

```text
Disabled       profile 存在但 auto_start=false，未运行
Configured     profile 有效，尚未启动
Starting       正在做驱动、盘符、bundle 和 M4 runtime 前置检查
Recovering     正在从已有 bundle 选择完整 generation 并挂载
Provisioning   仅显式 add --create 的首次创建路径
Running        volume、AIM identity、worker 均通过验证
Checkpointing  正在执行一次受策略触发的 checkpoint
Stopping       正在执行 M4 正常 shutdown
Stopped        曾正常停止，bundle 保留
Blocked        profile/驱动/盘符/权限前置条件失败，未拥有运行资源
Failed         worker 或生命周期失败；资源清理已完成或结果待确认
```

合法主要转移：

```text
Configured/Stopped -> Starting -> (Provisioning|Recovering) -> Running
Running -> Checkpointing -> Running
Running -> Stopping -> Stopped
Starting/Recovering/Checkpointing -> Blocked 或 Failed
Failed/Blocked/Stopped -> Starting（仅显式 start 或下一次 boot policy）
Disabled -> Disabled（enable 后为 Configured）
```

状态转换具有以下不变式：

- 同一 profile 同时最多一个 start/stop/checkpoint；重复请求返回当前操作状态，
  不启动第二个 runtime。
- 一个 profile 的错误只释放该 profile 已验证拥有的资源；绝不按名称杀进程、
  删除未知 AIM 设备或卸载其他 profile 的盘符。
- `Running` 只有在 volume identity、AIM identity、drive letter、bundle writer
  lock 和 worker heartbeat 全部验证后才能发布。
- `Failed`/`Blocked` 不能隐藏在 `Stopped`；`list` 必须显示稳定错误类别和最后
  generation（如果可安全读出）。
- Service 重启后状态全部重新从 profile、bundle 和实际设备建立；旧状态文件不
  能让服务跳过验证。

## 9. Service 生命周期与启动顺序

### 9.1 启动

`MyRamDiskService` 采用 `AUTO_START`（必要时使用 delayed-auto-start），运行账户
为 LocalSystem，依赖已存在的 `phdskmnt` 服务/适配器检查。服务不安装或改变该驱动。
启动阶段顺序固定为：

1. 打开受保护日志和 IPC；记录服务版本、OS build、驱动查询结果。
2. 读取并逐个验证全部 profile；同一坏 profile 不阻塞其他 profile。
3. 做一次只读环境预检（权限、持久化 NTFS、驱动可查询、profile 盘符唯一）。
4. 将 `auto_start=true` 的 profile 按小写 `profile_id` 排序提交独立启动 worker；
   每盘启动超时 60 秒，超时只标记该盘失败并开始其资源清理。
5. 所有 worker 都完成“成功、Blocked 或 Failed”判定后，服务进入 `Running` 或
   `Degraded`。`Degraded` 表示服务和 IPC 正常但至少一个 profile 未运行；不能被
   报告为全盘成功。

启动期间不自动创建缺失 profile、不格式化目标卷、不修复损坏 bundle、不重试无限
次。驱动不可用时所有相关 profile 为 `Blocked`，仍保留 `list/status` 和日志。

### 9.2 停止、注销和系统关机

服务接受 `SERVICE_CONTROL_PRESHUTDOWN`。顺序固定为：

1. 停止接收新的变更 IPC；查询请求可继续直到响应。
2. 标记 supervisor 为 stopping，并为每个 `Running` runtime 发出一次 M4 clean
   shutdown：阻止新 mutation、FlushFileBuffers、final checkpoint、解决提交状态、
   dismount、删除自有盘符、停止 AIM/runtime。
3. 每个盘有 120 秒有界预算；一个盘超时或失败不阻塞其他盘的清理。
4. 关闭 worker、写最终状态事件、关闭 IPC 和日志。

正常关机时 checkpoint 成功则该 generation 成为新的恢复点；checkpoint 未完成时
   不伪造成功，下一次启动仍只能恢复先前完整 generation。服务不会删除 bundle。
   若系统强制终止服务，按 M4 异常恢复语义处理，不把未完成 shutdown 记为已提交。

## 10. Worker 监督与故障隔离

每个 worker 对应一个 profile，向 supervisor 报告启动阶段、generation、设备身份、
checkpoint disposition、清理结果和心跳。心跳仅用于检测线程/进程异常，不是持久化
权威。

- worker 意外退出：service 立即记录 exit reason，验证并清理它仍拥有的 volume/盘符/AIM
  资源；状态为 `Failed`。不自动无限重启、不删除 bundle；管理员可执行 `disk start`
  触发一次新的 M4 recovery。
- worker 启动尚未发布 `Running` 就失败：不得留下可见盘符或 AIM 设备；失败清理
  失败也必须列出残留身份并保持 `Failed`，不能跨盘清理。
- A 的 checkpoint、bundle 损坏、盘符占用或 stop 失败不得改变 B 的状态、generation、
  journal、盘符或 AIM identity。
- supervisor 自身崩溃后，Windows SCM 可按服务恢复策略重新启动一次；新实例仍按
  profile/bundle 重新校验。M5 不以自动重启 worker 作为数据正确性的依据。

## 11. 观察性、日志和诊断

日志目录：

```text
C:\ProgramData\MyRamDisk\logs\service.jsonl
C:\ProgramData\MyRamDisk\logs\audit.jsonl
```

每条事件至少包含 UTC 时间、服务版本、事件类型、profile id、状态前后值、
generation（如已知）、drive letter、错误类别、请求 id 和结果。AIM/PnP 编号只
作为观测字段，不能作为恢复/清理的唯一身份。日志单文件达到 10 MiB 时保留最近
5 个滚动文件；日志写满不应改变 checkpoint 结果，但必须发出一次 `LogWriteFailed`
事件或 Windows Event Log 记录。

`disk list --json` 输出 schema `myrd.disk.list.v1`，每项包含 profile、状态、
last_committed_generation（可选）、runtime pid/volume identity（可选）、last_error
和 `observed_at_utc`。字段缺失表示未知，不得以零或旧值冒充当前状态。

## 12. M4 Bundle 注册与迁移

M5 不修改 bundle 文件。把已有 M4 bundle 纳入服务的唯一受支持流程是：

1. 管理员执行 `disk add`/`disk import`，指定唯一 profile id、bundle、size 和盘符。
2. Service 在离线状态取得 writer lock，执行 M3/M4 只读完整性和 geometry 验证；
   若 manifest、journal、VHDX 或 geometry 不合法，拒绝发布 profile。
3. 将已验证的 profile 通过原子 replace 写入 `ProgramData`；bundle 保持原路径和字节。
4. 管理员显式 `disk start` 验证一次 recovery，再决定是否 `enable`。

M4 的 foreground command、bundle 文件名、manifest generation 和 checkpoint 语义
   不变；没有 profile 的旧 bundle 不会被开机自动扫描或“猜测挂载”。profile 删除
   只撤销管理关系，不删除 bundle，因此可以回退到 M4 foreground 命令。

Profile schema 升级只接受服务明确支持的版本。未知版本进入 `Blocked`，保留原文件；
安装新服务前应复制整个 profiles 目录。M5 v1 不提供在线 schema rewrite 或自动
   降级；回滚服务时删除/禁用 profile 不影响 bundle。

## 13. VM 验收前置条件和证据

使用与 M4 相同的 Windows 11 x64 VMware 虚拟机，并在独立持久化 NTFS 路径保存
证据。验收前必须：

- 只安装已批准的、签名且可查询的 `phdskmnt`；
- 管理员 PowerShell、空闲目标盘符（R:/S:）和无残留 `myrd`/AIM 设备；
- 记录 commit、Release SHA-256、Windows build、VM UUID、驱动服务状态和 profile
  目录 ACL；
- 每个案例保存精确命令、stdout、stderr、退出码、service JSON 状态、日志片段、
  bundle tree/hash、volume/AIM identity 和最终清理观察；
- 人工 Power Off→Power On 只能由验收人员执行，脚本不得替代电源边界。

沿用 M4 的 `PASS`、`PRODUCT_FAIL`、`ENVIRONMENT_BLOCKED`、`HARNESS_FAIL`、
`USER_ABORTED` 分类。任何 profile 失败不能被改写为 service 全局 PASS。

## 14. VM-可执行验收矩阵

以下每项都使用全新的 profile/evidence 根目录；除标明“人工断电”的项目外，
产品命令由管理员 PowerShell 执行。每项通过条件必须同时满足命令结果、资源身份、
日志和清理断言。

### M5-1：服务安装、profile 注册和列表

1. 确认 `phdskmnt` 可查询，安装/启动 `MyRamDiskService`，记录服务状态。
2. 用 `myrd disk add --id <A> --bundle C:\M5\disk-a --size 64M --drive-letter R: --auto-start`
   注册一个已由 M4 创建并验证的 bundle。
3. 执行 `myrd disk list --json` 和 `myrd service status --json`。

通过条件：profile JSON 位于正确目录且 ACL 正确；list 显示 `Configured`、正确
路径/容量/盘符/auto_start；没有创建 AIM 设备或占用 R:；重复 id、重复 bundle、
重复盘符、未知字段和普通用户写入均被拒绝且没有副作用。

### M5-2：显式 start/checkpoint/stop

1. 执行 `myrd disk start <A>`，在 R: 写入 marker，等待 service 报告 committed generation。
2. 执行 `myrd disk checkpoint <A>`，记录 generation 和 marker SHA-256。
3. 执行 `myrd disk stop <A>`。

通过条件：状态按 `Starting/Recovering/Running/Checkpointing/Stopping/Stopped`
   转移；R: 与正确 AIM identity 绑定；stop 完成最终 flush/checkpoint、卸载并删除
   自有盘符；bundle 的 committed marker 和 generation 可由 M4 foreground recovery
   验证；其他外部盘符/设备不受影响。

### M5-3：开机自动恢复

1. 将 A 和 B profile 均 `enable`，执行 `disk start` 写入不同 committed markers，
   记录 generation/hash 后正常重启 Windows。
2. 登录后只执行 `myrd disk list --json` 和 `service status`，不手动 `disk start`。

通过条件：service 自动启动；A/B 均在其原盘符恢复到各自最后 committed generation；
   两个 bundle、AIM identity 和 volume identity 不交叉；日志明确记录启动顺序和结果；
   服务状态为 `Running`。

### M5-4：双盘启动故障隔离

1. A、B 均设为 auto-start；在启动前让 B 的目标盘符被外部卷占用，或使用一个
   经过批准的 disposable 损坏 B profile。
2. 重启 Windows，观察 service 和 A/B。
3. 解除 B 故障后显式 `disk start B`。

通过条件：A 自动进入 `Running` 并可读写；B 进入 `Blocked` 或 `Failed`，错误分类
   明确且没有格式化/删除 B bundle；B 的失败不停止 service、不卸载 A、不改变 A 的
   generation；修复前置条件后显式 start B 可恢复。该项必须记录外部卷未被修改。

### M5-5：IPC 与权限边界

1. 使用管理员和普通本机用户分别执行 `list/status`、`start/stop/checkpoint`。
2. 发送错误 schema、未知 operation、未知 profile、超长消息、重复 request id 和
   请求超时场景。

通过条件：普通用户可查询但所有变更返回权限错误；管理员变更成功；非法请求返回
   参数/IPC 错误且不触碰 bundle/设备；服务日志记录 SID、request id 和结果；客户端
   断开后操作不会被当成成功或静默取消。

### M5-6：正常停止和系统关机协调

1. A/B 同时运行并产生 dirty data。
2. 执行 `disk stop A`，同时对 B 发起一次 checkpoint；另做一次 Windows 正常重启。

通过条件：每盘各自完成 flush/final checkpoint/卸载；一个盘慢或失败不跨盘清理；
   正常重启后 auto-start 盘恢复最新已成功 generation；停止期间拒绝新的变更请求；
   无 `myrd`、目标盘符或 AIM 设备残留，或证据明确列出且归属于失败盘。

### M5-7：worker 异常退出与显式恢复

1. 运行 A/B，记录各自 worker PID 和资源 identity。
2. 只终止 A worker，不触碰服务或 B。
3. 查询 list/status，确认 A 失败后执行 `disk start A`。

通过条件：service 在有界时间内将 A 标为 `Failed`，只清理 A 已验证资源；B 保持
   `Running`、盘符可读写且 generation 不变；A 的 bundle 不被删除/格式化；显式
   start A 从最近完整 checkpoint 恢复；日志含退出原因、清理结果和新 generation。

### M5-8：人工强制断电后的多盘恢复

1. A/B 都有 committed marker，各自再写一个未提交 marker。
2. 记录 service/worker 状态和 generation。
3. 由操作员执行一次 VMware `Power Off → Power On`，不得使用 guest shutdown、
   `vmrun` 或脚本代替。
4. 登录后等待 service 自动启动，执行 `disk list --json`。

通过条件：A/B 自动恢复到各自最后完整 committed generation；未提交 marker 可丢失
   或保留；不出现混合 generation；无 A/B identity 交换；一个盘 bundle 损坏时另一
   盘仍独立恢复；最终 orderly stop 后无归属不明的进程、盘符或 AIM 设备。

### M5-9：profile/bundle 完整性和迁移回退

1. 复制 A bundle 和 profile 到 disposable 路径，分别篡改 schema、revision、
   drive letter、manifest 或 journal 尾部。
2. 重启 service/执行 `disk start`；记录所有原始文件哈希。
3. 从 M5 profile remove（保留 bundle），用原 M4 `run` 命令 recovery；重新 import
   为新 profile。

通过条件：每个坏 profile/bundle fail-closed，状态为 `Blocked`/`Failed`，不创建 AIM
   设备、不格式化、不删除原文件；有效 bundle 可被 import；remove 后 bundle 字节和
   M4 recovery 不变；重新 import 生成新的 profile revision，不需要改写 bundle。

## 15. 通过条件与证据格式

M5 资格通过必须同时满足：

1. Debug/Release 构建、M4 回归和 M5 单元/集成测试通过。
2. M5-1 至 M5-9 全部为 `PASS`；人工断电项目必须有操作员记录。
3. 每个 profile 的失败隔离、权限、日志、identity 和最终清理断言均有原始证据。
4. `qualification.json` 固定记录 source commit、两个 artifact SHA-256、VM 元数据、
   driver preflight、各 case 分类和证据文件哈希；离线 verifier 只读验证证据，不启动
   service、不查询或修改设备。
5. 最终 VM 状态没有未解释的 `myrd` 进程、R:/S: 目标盘符或 AIM 设备；若某盘的失败
   资源无法清理，必须将该 case 标为 `PRODUCT_FAIL`，不得以“重启后消失”掩盖。

推荐 case 证据结构：

```text
<evidence-root>/
  qualification.json
  artifacts/myrd-release.exe.sha256
  artifacts/myrd-debug.exe.sha256
  VM-M5-1/{preflight.json,commands/,service.json,logs/,profiles/,result.json}
  VM-M5-2/{...}
  ...
  driver-preflight.json
```

## 16. 未决但有界的实现选择

以下选择在实现前必须固定在 plan/代码评审中，但不能扩大 M5 范围：

- 服务使用 LocalSystem 还是等效的受限虚拟服务账户：本草案选择 LocalSystem，理由
  是当前已签名 `phdskmnt`/volume 操作和现有 M4 管理员模型；若改为受限账户，必须
  重新证明驱动、bundle ACL 和 shutdown 权限，不得默默降级。
- profile 原子 replace 的 Windows 目录 flush 细节：必须保证重启后读取到完整旧文件
  或完整新文件；不能接受截断 JSON。可保留 `.bak` 作为回滚副本，但 `.bak` 不是
  service 自动恢复输入，除非主文件验证失败且副本自身通过 schema/ACL/时间规则。
- `AUTO_START` 与 delayed-auto-start 的具体 SCM 标志：应以 `phdskmnt` 可查询、
  启动不阻塞系统和 M5-3 通过为准；不能通过重启 guest 来规避服务排序问题。
- 失败 worker 是否由 SCM 重启 service：允许一次有界 service restart，但不允许
  无限 worker 自动重启；worker recovery 的权威入口仍是显式 `disk start`。

这些实现选择不会改变 M4 的 checkpoint/recovery 语义，也不会增加 GUI、远程管理、
驱动替换或应用一致性功能。
