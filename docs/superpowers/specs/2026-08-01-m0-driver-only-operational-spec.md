# MyRamDisk M0 Driver-only PoC Operational SPEC

> 日期：2026-08-01
>
> 状态：已批准
>
> 上位规格：`docs/superpowers/specs/2026-07-31-aim-driver-only-clean-slate-design.md`
>
> 实施计划：`docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md`
>
> AIM 源码基线：`d379caa027d21902e45fce8c9daf02fd116c2dda`

## 0. 本文怎么用

本文是 M0 的规范性契约，不是背景介绍。执行 Agent 不得依赖聊天记录补全本文没有写出的行为。

关键词含义：

- **MUST / 必须**：不满足即 M0 失败。
- **MUST NOT / 禁止**：违反即停止执行并先清理现场。
- **SHOULD / 应当**：若不采用，必须在验收报告解释理由。
- **MAY / 可以**：不影响 M0 结论的实现选择。

发生冲突时，优先级如下：

1. 本文中的安全不变量。
2. 固定提交中的 AIM 官方协议定义。
3. 本文其他要求。
4. 实施计划中的示例代码和命令。

如果固定提交中的协议事实与本文不一致，执行 Agent 必须停止在“协议来源门”，记录差异并修改本文和计划；禁止选择一个看起来能工作的魔法数字继续尝试。

## 1. M0 唯一目标

M0 只回答一个问题：

> 在保留当前已安装、已签名的 `phdskmnt.sys` 的前提下，我们能否完全不调用 AIM Toolkit 用户态组件，用自己的 C++20 程序实现 Shared Memory Proxy、创建真实 Windows 磁盘、完成原始块读写并精确删除设备？

M0 的成功产物是可重复的技术证据，不是可供日常使用的 RAM Disk 产品。

## 2. 明确不属于 M0

下列内容禁止借 M0 顺手实现：

- 卸载 AIM Toolkit 或独立安装驱动。
- VHD/VHDX、镜像加载或镜像保存。
- 动态页提交、TRIM、ZERO、UNMAP。
- 定时 checkpoint、journal、崩溃恢复。
- Windows Service、CLI 管理面、GUI、盘符分配。
- 分区、格式化、NTFS 文件测试。
- 多磁盘、多 Provider、多会话并行。
- 性能优化或吞吐基准。
- 自研内核驱动。

任何上述能力只能在 M0 得出 GO 后进入后续里程碑。

## 3. 运行时依赖边界

M0 运行时允许：

- Windows 系统 DLL。
- 我们自己编译的 `myrd-m0-poc.exe`。
- 已加载的官方签名 `phdskmnt.sys`。
- Windows 内核、PnP、SetupAPI、Configuration Manager、CryptoAPI、BCrypt。

M0 运行时禁止：

- 启动 `aim_ll.exe`、`RamDyn.exe`、`AIMtk-Dlg.exe`、`AIMtk-svc.exe`、`aim_cli.exe`、`MountImg.exe` 或 `RamDiskUI.exe`。
- 加载 `aimapi.dll`。
- 调用 AIM Toolkit GUI、CLI、COM、服务 API 或导出函数。
- 用 WMI、DiskPart、PowerShell 或第三方工具创建设备。
- 让 `deviosvc` 为本次共享内存会话提供服务。

PowerShell 只允许用于构建、环境观察、停止和恢复 `deviosvc`、启动我们的程序以及解析报告。

`query-driver` 允许观察到测试前已经运行的 `deviosvc`，但必须证明它没有被本程序启动。`run` 的 E2E 报告必须记录 `deviosvc` 在整个 Provider 生命周期中为 Stopped。

## 4. 当前机器基线与可移植要求

在 2026-08-01 的当前机器上已只读观察到：

- CMake `4.2.2`。
- Ninja `1.13.2`。
- Visual Studio 2022 Build Tools。
- Windows SDK `10.0.26100.0`。
- `phdskmnt` 驱动服务处于 Running/Automatic。
- `deviosvc` 处于 Running/Manual。
- `C:\Windows\System32\drivers\phdskmnt.sys` 文件版本 `1.2.28.93`。

这些是环境证据，不是硬编码产品要求。实现必须在运行时重新采集，不得假设状态不变。

构建约束：

- C++20。
- MSVC x64。
- CMake + `Ninja Multi-Config`。
- `/W4 /WX /permissive- /Zc:__cplusplus /EHsc /utf-8`。
- Debug 和 Release 都必须成功构建。
- 真实驱动 E2E 只允许在提升权限的 x64 PowerShell 中运行。

## 5. 不可违反的安全不变量

### S1：不卸载

M0 禁止卸载 Toolkit、驱动、设备节点、服务或驱动包。

### S2：不使用全设备删除值

`IMSCSI_ALL_DEVICES` / 自动分配哨兵值 `0x00FFFFFF` 不得传给删除操作。删除 API 在调用 transport 前必须拒绝该值。

### S3：只删除本进程创建的精确设备号

只有成功 CREATE 响应返回并通过校验的 `DeviceId` 才能进入 owned slot。清理逻辑只能读取这个 slot。

### S4：不能靠容量、盘符或“最新磁盘”猜设备

PhysicalDrive 必须由 AIM `DeviceId` 中的 PathId/TargetId/Lun 与 Windows 磁盘接口的 SCSI 地址精确匹配，再通过额外身份检查。

### S5：不写 sector 0

唯一真实写入位置固定为 8 MiB，长度固定为 4096 bytes。偏移和长度都必须按 512 bytes 对齐。

### S6：写入前至少完成四重身份证明

在打开写权限前必须全部满足：

1. SCSI PathId/TargetId/Lun 与本次 CREATE 返回的 `DeviceId` 相等。
2. PhysicalDrive 编号不在 create 前的磁盘快照中。
3. `IOCTL_DISK_GET_LENGTH_INFO` 返回精确 64 MiB。
4. `IOCTL_STORAGE_GET_DEVICE_NUMBER` 返回的设备号与路径中的十进制编号相等。

任意一项不满足，禁止写入并立即进入清理。

### S7：`deviosvc` 必须恢复

E2E 脚本只允许改变 `deviosvc` 的运行状态，不得改变 StartMode。无论成功、程序异常还是 PowerShell 异常，都必须在 `finally` 中恢复原运行状态并验证 StartMode 未变化。

### S8：失败也必须留下报告

从 preflight 之后发生的任何失败都必须尽力写出 JSON 报告。报告必须包含最后成功阶段、原生错误码、是否拥有 DeviceId、是否尝试精确删除、最终设备列表及命名对象检查结果。

### S9：无法证明归属就不删除

如果 CREATE 的系统调用结果不确定，而且查询结果无法把某个设备与当前 session object path 唯一关联，禁止猜测删除。

### S10：真实测试串行

同一机器同一时间最多运行一个 M0 `run`。必须用进程外命名 mutex 拒绝并行运行。

## 6. 协议来源门

### 6.1 唯一来源

下列 AIM 官方文件在固定提交中的内容是唯一协议依据：

- `Unmanaged Source/phdskmnt/inc/common.h`
- `Unmanaged Source/phdskmnt/inc/imdproxy.h`
- `Unmanaged Source/phdskmnt/proxy.cpp`
- `Managed Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs`

执行 Agent 必须先取得该提交，并运行：

```powershell
git rev-parse HEAD
```

输出必须严格等于：

```text
d379caa027d21902e45fce8c9daf02fd116c2dda
```

网络不可用、提交不可取得或源码路径变化时，M0 状态是 `BLOCKED_PROTOCOL_SOURCE`，不得根据旧计划里的数字继续实现。

### 6.2 本地协议清单

仓库必须生成并提交 `docs/protocol/aim-m0-contract.md`，逐项记录：

- 上游提交号。
- 每项常量的上游符号名、值、文件和行号。
- 每个结构的大小、对齐、字段顺序和字段偏移。
- CREATE/QUERY/REMOVE 的输入和输出有效长度。
- 文件名长度的单位、是否含 NUL、字符编码。
- SRB `ReturnCode` 和 payload 内状态字段的解释。
- Shared Memory 对象名和 native object path 的转换。
- INFO/READ/WRITE/CLOSE 的请求与响应布局。

生产源码中的每个协议常量必须能在该清单找到一一对应项。

### 6.3 ABI 验证

所有本地结构必须：

- 使用固定宽度整数。
- 显式指定必要 packing。
- 用 `static_assert(sizeof(...))` 验证大小。
- 用 `static_assert(offsetof(...))` 验证关键字段。
- 禁止把共享内存中的未对齐地址直接 `reinterpret_cast` 为结构引用。
- 读取和写入协议头统一使用 `std::memcpy`。

## 7. 固定 M0 参数

| 参数 | 固定值 |
|---|---:|
| 逻辑磁盘容量 | 64 MiB |
| 逻辑扇区 | 512 bytes |
| Shared Memory header | 4096 bytes |
| Shared Memory data area | 8 MiB |
| Shared Memory 总大小 | 8 MiB + 4096 bytes |
| 真实写入偏移 | 8 MiB |
| 真实写入长度 | 4096 bytes |
| PhysicalDrive 枚举超时 | 15 seconds |
| 枚举重试间隔 | 250 ms |
| 单次驱动控制默认超时 | 30 seconds |
| Provider 停止等待 | 5 seconds |
| M0 最终重复次数 | 20 |

这些值必须集中定义，不得在多个 `.cpp` 中复制裸数字。

## 8. 模块和所有权边界

```text
myrd-m0-poc
├── common
│   ├── Error / Result
│   ├── UniqueHandle / UniqueView / ScopeExit
│   ├── checked integer conversion
│   └── SHA-256 / Authenticode / process snapshot
├── storage
│   └── MemoryBlockDevice(64 MiB)
├── proxy
│   ├── protocol layouts
│   ├── RequestProcessor
│   ├── SharedMemoryChannel
│   └── ShmProxyServer
├── driver
│   ├── AIM protocol layouts
│   ├── SrbCodec
│   ├── SrbTransport
│   ├── DirectAimAdapter
│   ├── AdapterDiscovery
│   └── PhysicalDriveLocator
└── poc
    ├── LifecycleRunner
    ├── RawRoundTrip
    └── PocReport
```

边界要求：

- `storage` 不包含 AIM/IMDPROXY 常量。
- `RequestProcessor` 不创建 Win32 对象和线程。
- `SharedMemoryChannel` 不解释块数据。
- `SrbCodec` 不调用 Win32 API。
- `DirectAimAdapter` 依赖可注入 `ISrbTransport`。
- `LifecycleRunner` 是唯一拥有“创建后自动清理”状态机的模块。
- `main.cpp` 只做参数解析、权限检查、调用 runner、写报告和映射退出码。

## 9. 通用错误契约

内部错误至少包含：

```cpp
enum class ErrorDomain : std::uint8_t {
    usage,
    preflight,
    protocol,
    win32,
    ntstatus,
    storage,
    cleanup,
    evidence,
    internal
};

struct Error {
    ErrorDomain domain;
    std::uint32_t code;
    std::wstring operation;
    std::wstring message;
};
```

要求：

- 普通错误使用 `Result<T>` 返回，不用异常控制正常分支。
- 构造函数和 STL 分配失败可以抛异常，但 `main` 必须捕获并生成报告。
- Win32 失败必须在调用其他 Win32 API 前保存 `GetLastError()`。
- 驱动返回的 `NTSTATUS`/SRB ReturnCode 必须原样记录，不得只转成“失败”。
- 每个错误必须标明 operation，例如 `CreateFileW(adapter)` 或 `SMP_IMSCSI_CREATE_DEVICE`。

## 10. 内存块设备契约

`MemoryBlockDevice`：

- 构造后精确拥有 64 MiB 零初始化字节。
- `size_bytes()` 返回 64 MiB。
- `sector_size()` 返回 512。
- READ/WRITE 拒绝零长度。
- offset 和 length 必须同时按 512 对齐。
- 必须使用减法形式检查范围，避免 `offset + length` 溢出。
- 成功时返回完整长度；M0 不允许部分完成。
- 同一 Provider 工作线程串行调用，因此 M0 不要求后端内部并发。

至少覆盖：零初始化、首扇区、末扇区、8 MiB/4096 round trip、未对齐 offset、未对齐 length、越界、整数溢出、零长度。

## 11. Shared Memory Provider 契约

### 11.1 对象命名

每次 run 使用 CSPRNG 生成 128-bit session id，编码为 32 个小写十六进制字符。对象名：

```text
Global\myrd-m0-<session-id>
Global\myrd-m0-<session-id>_Request
Global\myrd-m0-<session-id>_Response
Global\myrd-m0-<session-id>_Server
```

传给驱动的 native path：

```text
\BaseNamedObjects\Global\myrd-m0-<session-id>
```

session id 只允许 `[0-9a-f]{32}`；任何其他输入必须拒绝，禁止把任意用户字符串拼接到 kernel object path。

### 11.2 对象语义

- Mapping 大小精确为 8 MiB + 4096 bytes。
- Request event 是 AutoReset、初始 nonsignaled。
- Response event 是 AutoReset、初始 nonsignaled。
- `_Server` 对象按上游实现要求创建并持有；重复 session 名必须失败。
- 命名对象使用显式安全描述符，只允许 LocalSystem 和 Built-in Administrators 完全访问。
- 映射 view、mapping handle、events、mutex 和线程全部使用 RAII。

### 11.3 请求处理

M0 只实现：

- INFO
- READ
- WRITE
- CLOSE

INFO 的 flags 必须是 `0`，因此禁止宣告 CONNECT、UNMAP、ZERO、SCSI 或 SHARED。

READ/WRITE：

- 请求长度不得超过 8 MiB data area。
- offset/length 必须符合块设备对齐和范围规则。
- READ 先把数据写入 data area，再写 response header，最后 signal Response。
- WRITE 先复制 data area 到进程私有缓冲区，再调用 backend，避免共享内存被异步修改。
- 有定义响应格式的请求发生可恢复错误时，返回非零 error 和 0 length，并 signal Response。
- 未知 request code 是 fatal protocol error：记录首 64 bytes 的十六进制协议头摘要，禁止记录 data area 内容，然后进入清理。

### 11.4 Server 状态

```text
constructed -> ready -> running -> close_seen -> stopped
                         |             |
                         +-> failed <--+
```

- `start()` 只有在线程启动、对象可打开、ready 状态发布后才成功。
- worker 同时等待 Request event 和私有 Stop event，不允许用 1 秒轮询模拟停止。
- 每个收到的请求只 signal 一次 Response。
- CLOSE 增加计数后结束循环，不再 signal 下一轮。
- `stop()` 幂等，可在任意部分初始化状态调用。
- 析构不得抛异常。

## 12. AIM 控制面契约

### 12.1 Adapter discovery

- 使用 `GUID_DEVINTERFACE_STORAGEPORT` 枚举 present interface。
- 获取设备 service property，仅保留 `phdskmnt`，大小写不敏感。
- 每个候选都必须实际发送 QUERY_VERSION。
- 恰好一个候选通过协议校验才成功。
- 0 个返回 not found；多个返回 ambiguous，禁止任取第一个。

### 12.2 SRB transport

- 单个连续 buffer：`SRB_IO_CONTROL` + payload + response capacity。
- input/output 指向同一个 buffer。
- 调用 `DeviceIoControl(IOCTL_SCSI_MINIPORT)`。
- 校验 `bytes_returned >= sizeof(SRB_IO_CONTROL)`。
- 校验 signature、HeaderLength、ControlCode、Length 和总边界。
- 同时保存 Win32 error、SRB ReturnCode 和 payload status。
- 所有 `size_t -> DWORD/ULONG` 转换都先检查上限。

### 12.3 操作

必须支持并独立测试：

- QUERY_VERSION
- QUERY_ADAPTER/list devices
- CREATE_DEVICE
- QUERY_DEVICE
- REMOVE_DEVICE

CREATE：

- device number 输入为上游定义的自动分配值。
- 类型为硬盘 + proxy + shared memory。
- disk size 64 MiB，sector 512，image offset 0。
- filename 是 native object path，长度语义严格按固定上游提交。
- 成功响应中的精确 DeviceId 立即写入 owned slot。
- 随后 QUERY_DEVICE 必须确认 size、flags 和 session native path 与请求一致。

为保证“CREATE 成功但 QUERY_DEVICE 失败”仍可精确清理，M0 的底层接口必须把 CREATE 和确认拆开：CREATE 解码成功后先把 `DeviceId` 返回给生命周期，生命周期保存 owned slot，再单独调用 QUERY_DEVICE。后续产品接口可以封装两步，但不得隐藏中间 DeviceId。

REMOVE：

- 参数类型只接受强类型 `DeviceId`。
- 自动分配/全部设备哨兵值在 transport 前被拒绝。
- 删除后轮询 QUERY_ADAPTER，直到该精确 DeviceId 消失或 15 秒超时。

## 13. PhysicalDrive 身份解析

create 前采集：

- AIM device id 列表。
- 所有当前 `GUID_DEVINTERFACE_DISK` 的 PhysicalDrive number。

create 后每 250 ms 枚举磁盘接口：

- `IOCTL_SCSI_GET_ADDRESS` 得到 PathId/TargetId/Lun。
- `IOCTL_STORAGE_GET_DEVICE_NUMBER` 得到 PhysicalDrive number。
- `IOCTL_DISK_GET_LENGTH_INFO` 得到长度。
- 只保留地址精确匹配本次 DeviceId 的候选。

成功条件：

- 恰好一个候选。
- 候选不在 create 前 PhysicalDrive 集合。
- 长度 64 MiB。
- 设备号与 `\\.\PhysicalDriveN` 中 N 相同。
- QUERY_DEVICE 已确认 session native path。

0 个候选继续重试；多个候选或任一身份冲突立即失败，不继续重试。

## 14. 生命周期状态机

`LifecycleRunner` 必须显式记录以下阶段：

| 序号 | 阶段 | 成功后拥有的资源 |
|---:|---|---|
| 0 | `started` | 无 |
| 1 | `preflight_passed` | run mutex |
| 2 | `baseline_captured` | baseline snapshots |
| 3 | `provider_ready` | mapping/events/mutex/thread |
| 4 | `create_returned` | optional exact DeviceId |
| 5 | `device_confirmed` | verified DeviceId/session identity |
| 6 | `physical_drive_confirmed` | verified PhysicalDrive path |
| 7 | `round_trip_verified` | matching hashes |
| 8 | `device_removed` | no owned kernel device |
| 9 | `provider_stopped` | no provider thread |
| 10 | `cleanup_verified` | no named objects/device delta |
| 11 | `report_written` | final JSON |

清理顺序必须固定：

1. 如果 owned DeviceId 存在，尝试删除该精确 id。
2. 等待该 id 从 QUERY_ADAPTER 消失。
3. 停止 Provider，并 join worker。
4. 释放 view 和所有命名对象 handle。
5. 尝试重新打开 session mapping/events/mutex，必须全部得到 not found。
6. 重新采集 AIM device list 和 PhysicalDrive list。
7. 写最终报告。

清理错误不能覆盖原始错误；报告同时保存 `primary_error` 和 `cleanup_errors[]`。

## 15. 原始块 round trip

- 只打开已经通过第 13 节全部身份检查的路径。
- 访问权限为 GENERIC_READ | GENERIC_WRITE。
- share mode 为 FILE_SHARE_READ | FILE_SHARE_WRITE。
- 使用 `SetFilePointerEx` 定位到 8 MiB。
- 使用 `BCryptGenRandom` 生成 4096 bytes 测试数据。
- 写入必须完整完成，然后 `FlushFileBuffers`。
- 重新 seek 后完整读回 4096 bytes。
- 分别计算大写十六进制 SHA-256。
- 两个 digest 必须相等；报告只记录 digest，不记录原始内容。
- 任意 short read/write 均为失败。

## 16. CLI 和退出码

只支持：

```text
myrd-m0-poc.exe query-driver --report <absolute-or-relative-json-path>
myrd-m0-poc.exe run --report <absolute-or-relative-json-path>
```

Debug 构建为 V5 清理验证额外支持：

```text
myrd-m0-poc.exe run --report <path> --inject-failure-after <stage>
```

`stage` 只允许 `create_returned`、`device_confirmed`、`physical_drive_confirmed` 或 `round_trip_verified`。Release 构建和 `query-driver` 必须拒绝该参数。故障注入只制造主流程失败，之后必须执行与真实故障完全相同的精确清理。

- 未知命令、重复参数、缺失值都退出 2。
- `query-driver` 禁止创建 mapping、event、mutex 或设备。
- `run` 必须要求管理员权限。
- report parent 不存在时创建目录；不能覆盖已有报告，除非显式使用新的唯一文件名。

退出码：

| 值 | 含义 |
|---:|---|
| 0 | 本命令所有要求成功 |
| 2 | CLI usage |
| 3 | preflight/environment |
| 4 | protocol/source contract |
| 5 | driver discovery/query |
| 6 | provider startup/protocol |
| 7 | create/query device |
| 8 | physical drive identity |
| 9 | raw round trip |
| 10 | exact remove/cleanup |
| 11 | evidence/report write |
| 12 | unexpected exception/internal invariant |

## 17. JSON 证据模式

每份报告是 UTF-8、单个 JSON object，顶层必须包含：

```json
{
  "schema_version": 1,
  "mode": "query-driver|run",
  "run_id": "32 lowercase hex chars",
  "started_utc": "ISO-8601 UTC",
  "finished_utc": "ISO-8601 UTC",
  "success": false,
  "exit_code": 0,
  "last_stage": "report_written",
  "environment": {},
  "dependency_audit": {},
  "driver": {},
  "baseline": {},
  "provider": {},
  "created_device": null,
  "raw_round_trip": null,
  "cleanup": {},
  "primary_error": null,
  "cleanup_errors": []
}
```

必需子字段：

### `environment`

- `windows_version`
- `windows_build`
- `architecture`
- `elevated`
- `executable_path`
- `executable_sha256`

### `dependency_audit`

- `imported_modules[]`
- `loaded_modules[]`
- `forbidden_loaded_modules[]`
- `forbidden_processes[]`
- `deviosvc_state_during_run`

### `driver`

- `service_name`
- `service_state`
- `file_path`
- `file_version`
- `sha256`
- `authenticode_status`
- `signer_subject`
- `adapter_interface_path`
- `query_version_raw`

### `baseline`

- `aim_device_ids_before[]`
- `physical_drive_numbers_before[]`
- `aim_device_ids_after[]`
- `physical_drive_numbers_after[]`

### `provider`

- `session_id`
- `native_object_path`
- `mapping_size`
- `data_area_size`
- `info_count`
- `read_count`
- `write_count`
- `close_count`
- `fatal_protocol_error_count`

### `created_device`

- `device_id_raw`
- `path_id`
- `target_id`
- `lun`
- `create_flags`
- `confirmed_by_query_device`
- `physical_drive_number`
- `physical_drive_path`
- `disk_length`

### `raw_round_trip`

- `offset`
- `length`
- `written_sha256`
- `read_sha256`
- `flush_succeeded`

### `cleanup`

- `exact_remove_attempted`
- `exact_remove_succeeded`
- `removed_device_id`
- `device_disappeared`
- `provider_stopped`
- `named_objects_released`
- `aim_device_lists_match`
- `physical_drive_lists_match`
- `deviosvc_restored`
- `deviosvc_start_mode_unchanged`

JSON 字符串必须正确转义控制字符、反斜杠和引号。测试必须用 PowerShell `ConvertFrom-Json` 进行独立解析。

## 18. 自验证层级

| 层级 | 是否管理员 | 是否接触真实驱动 | 目标 |
|---|---:|---:|---|
| V0 | 否 | 否 | 文档、格式、协议来源、禁止依赖扫描 |
| V1 | 否 | 否 | 编译、单元测试、静态 ABI |
| V2 | 否 | 否 | 本地 mapping/event/server integration |
| V3 | 是 | 只查询 | query-driver，无状态变化 |
| V4 | 是 | 是 | 单次 create/read/write/remove |
| V5 | 是 | 是 | 故障注入与清理证明 |
| V6 | 是 | 是 | 连续 20 次最终验收 |

不得跳级。V3 前禁止创建真实设备；V4 单次通过前禁止运行 20 次。

## 19. 必测负面场景

至少包括：

- Result success/error 和错误域保持。
- HANDLE、mapping view、mutex 的 move/reset/析构。
- checked integer conversion overflow。
- 协议结构大小/偏移。
- 截断 SRB header、错误 signature、错误 control code、超长 Length。
- CREATE 空路径、路径超长、容量未对齐、sector 非 512。
- REMOVE 自动分配/全部设备哨兵，且 fake transport 调用次数为 0。
- 内存块零长度、未对齐、越界和溢出。
- Proxy mapping 太小、request length 大于 data area、未知 request code。
- 重复 session 名创建失败。
- server stop 在未 start、running、close_seen、failed 状态均安全。
- adapter discovery 0 个和多个匹配。
- PhysicalDrive 0 个、多个、旧盘、容量不符、设备号不符。
- JSON 转义和独立解析。
- query-driver 不创建任何新设备或命名对象。
- CREATE 后、定位前失败时精确删除。
- round trip 后、remove 前失败时精确删除。
- report 写入失败时仍执行精确清理。
- `deviosvc` 原本 running 和原本 stopped 两种恢复路径。

## 20. GO 条件

只有以下全部成立，M0 才是 GO：

1. V0-V6 全部通过。
2. Release import table 不含 `aimapi.dll` 或其他 AIM 用户态 DLL。
3. 运行时 loaded modules 不含 AIM 用户态 DLL。
4. `deviosvc` 停止期间，20/20 run 成功。
5. 每轮 INFO、READ、WRITE 至少出现一次。
6. 每轮写入和读回 SHA-256 相等。
7. 每轮只删除 CREATE 返回的精确 DeviceId。
8. 每轮 AIM device list 和 PhysicalDrive list 最终恢复到 baseline。
9. 每轮所有 session 命名对象释放。
10. `deviosvc` 状态和 StartMode 恢复。
11. 没有随机失败、残留设备或无法解释的协议请求。

任意条件失败即 NO-GO。不得以“多数成功”“手工看起来正常”或“后续会修”宣布通过。

## 21. BLOCKED 与 NO-GO 的区别

- `BLOCKED`：在接触真实驱动前，因上游源码、管理员权限、构建工具或测试机条件缺失而无法继续。记录证据，不代表 Driver-only 技术路线失败。
- `NO-GO`：已满足执行前提，但协议、驱动、Provider、身份映射、读写或清理无法满足本 SPEC。必须提交复现报告和根因边界。

## 22. M0 完成后的唯一下一步

- GO：编写 M1 独立 SPEC 和计划。
- NO-GO：只允许做根因修复并重新执行 M0。
- BLOCKED：补齐外部条件后从失败层级重跑。

无论结果为何，M0 都不授权在当前物理机器卸载 AIM Toolkit。
