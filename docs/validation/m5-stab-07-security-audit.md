# STAB-07 / R-09：安全边界对抗性审计 —— **审计完成；阻断项修复中（PARTIAL）**

**方法**：对 `feature/m5-lifecycle` 做**只读静态审计**（管道、路径/重解析点、worker 权限/job、
二进制/配置 ACL、日志泄露五个面）。没有做运行期利用；因此本文件的“断言”全部是代码路径结论，
运行期需要验证的条目单列在 §4。审计由子代理执行、主代理逐条对照代码复核。

**结论口径**：按 release gate 8，**阻断项必须修完**、接受风险必须写清楚。当前阻断项已列在 §2，
修复完成前 STAB-07 保持 PARTIAL。

---

## 1. 已经成立的控制（有代码依据）

| 面 | 控制 | 位置 |
|---|---|---|
| 私有 worker 管道 | 本地 only（`PIPE_REJECT_REMOTE_CLIENTS`）、单实例、DACL 只含创建者/SYSTEM/Administrators、首帧 32 字节随机秘密握手 | `src/host/m5_worker_private_pipe.cpp:67-96,265-282,532-538,588-621` |
| 公有控制管道 | 固定名 + 拒绝远程、DACL deny AN/NU、BA/AU 只给受限掩码、调用者身份取自连接（impersonation）而非消息、1 MiB 上限 | `src/control/authorization.cpp:281-330,453-461`、`src/control/public_pipe_server.cpp:18-20,319-327`、`src/control/ipc_frame.h:16` |
| 路径/重解析点 | profile 目录逐组件 `FILE_FLAG_OPEN_REPARSE_POINT` + 类型/身份校验 + ACL 校验；bundle/持久化层拒绝 reparse/hardlink | `src/profile/profile_store.cpp:140-206,252-320,1171-1298`、`src/management/profile_registration_store.cpp:164-272`、`src/persistence/image_paths.cpp:543-580` |
| worker/job | 创建即入 job（`PROC_THREAD_ATTRIBUTE_JOB_LIST`）、继承列表只放启动管道与 job 句柄、身份=pid+创建时间 | `src/host/m5_worker_private_pipe.cpp:865-890,937-946` |
| ACL | 安装器为 data root/profiles/operations 设受保护 DACL；运行期同样校验 | `tools/install/Install-MyRamDisk.ps1:124-157`、`src/management/worker_binding_store.cpp:26-115` |
| 日志 | 事件只带 kind/UUID/command/phase/message，无秘密/nonce/管道名 | `src/management/service_core.h:887-897` |

## 2. 阻断项（必须修完才能算 R-09 PASSED）

| # | 问题 | 影响 | 修复方向 |
|---|---|---|---|
| B1 | **worker 不认证服务端**：worker 连到 spec 里的名字并先发秘密，`handshake` 只校验回显；没有 `GetNamedPipeServerProcessId`/服务端 SID 校验 | 服务在“创建管道后、worker 连接前”死亡时，任意本地用户可占用可预测的管道名，拿到秘密后驱动仍在运行的 SYSTEM worker（`start`/`export_bundle`/`remove_and_stop`） | worker 连接后校验服务端进程 SID ∈ {LocalSystem, Administrators}；不匹配立即退出 |
| B2 | **owner 未校验**：`validate_management_acl` 只取 DACL；owner 隐含 WRITE_DAC | 标准用户可预创建 `C:\ProgramData\MyRamDisk`（本机 ProgramData 给 Users 创建子项 + CREATOR OWNER 全权），安装器只设 DACL 不设 owner；owner 随后可改 DACL → 接管持久化状态 | 安装器显式设 owner（SYSTEM/Administrators）；产品校验 owner SID |
| B3 | **export 目的地**：`disk_export` 由 owner 发起，SYSTEM worker 用 `CREATE_NEW` 在任意绝对路径创建文件；没有父目录 reparse/可达性校验 | profile owner（非管理员）可让 SYSTEM 在 `C:\Windows\System32` 等目录创建文件（不可覆盖，但仍是任意创建） | 目标父目录：逐组件拒绝 reparse；用调用者 token 对父目录做 `FILE_ADD_FILE` 的 `AccessCheck`，不允许就拒绝 |
| B4 | **日志泄露**：`logs\service.log` 对 Users 可读，且失败消息里带完整 bundle 路径与 PID | 任意本地用户可读到其他用户的 bundle 路径（通常含用户名）与服务 PID | 安装器把 `logs` 改成 management-only；保留“管理员可读”的可操作性 |
| B5 | **job 句柄权限**：`DuplicateHandle(..., 0, TRUE, DUPLICATE_SAME_ACCESS)`，`DUPLICATE_SAME_ACCESS` 忽略期望权限，worker 拿到 `JOB_OBJECT_ALL_ACCESS`，与注释“0 权限”不符 | worker 可 `TerminateJobObject`/`SetInformationJobObject` 影响所有 profile 的 job（同一 SYSTEM 信任域内，但违背最小权限声明） | 用显式 `JOB_OBJECT_QUERY` 复制；同步修注释 |
| B6 | **launch spec 读上限**：编码允许 128 KiB，`read_launch_spec` 单次 `ReadFile` 到 4096 字节并按短读当整帧 | 合法但较长的 bundle 路径导致 worker 启动解码失败（fail-closed 可用性缺陷） | 按帧长循环读满，或把 spec 上限与编码对齐到 4096 |
| B7 | **公有管道可被抢注**：固定名；标准用户可先创建同名管道，服务首个实例 `FILE_FLAG_FIRST_PIPE_INSTANCE` 失败，CLI/GUI 连到假服务 | 假服务可返回任意响应、收集客户端请求里的目标/路径（不能驱动真服务） | 客户端连接后校验服务端进程 SID ∈ {LocalSystem, Administrators}；不匹配拒绝 |

## 3. 接受的风险（写明理由，不修）

| # | 风险 | 为什么接受 |
|---|---|---|
| A1 | worker 以 SYSTEM 运行（需要创建卷/挂载），服务↔worker 同信任域；被攻破的 worker 可发管理命令 | 架构使然：两者必须同一特权级才能完成 AIM/卷操作；真正的边界是“非管理员 → 服务”，由公有管道 DACL + 每命令授权负责 |
| A2 | job 允许 `BREAKAWAY_OK` | 当前代码没有任何 worker 子进程以 `CREATE_BREAKAWAY_FROM_JOB` 启动；保留权限是为将来的 provisioning helper。sweep 对 worker 自身的覆盖不受影响 |
| A3 | sweep 的 pid+创建时间绑定比较不可达（`observe_orphans` 不填 `profile_id`），实际按“在 job 内即证据”强制终止 | R-04.B 的行为与读数就是按这条路径验证的；job 成员身份是比 pid 更强的证据（pid 会复用）。已记录为已知简化 |
| A4 | 服务在 `service_worker_control.cpp:865-878` 无条件置 `writer_lock_proved`/`device_match_proved`/`bundle_identity_proved` | 服务与 worker 同信任域，这三个标志是协议内的“服务已经检查过”的传递，不是独立证明；跨信任域时才是问题 |
| A5 | 控制通道 DoS：8 个实例、每请求 30 s、所有认证用户可连 | 只影响本地可用性，不越权；第 9 条运行期检查会量化 |
| A6 | profile 校验按句柄、后续 I/O 按路径重开（TOCTOU） | 需要父目录写权限（管理员），不构成非管理员边界穿越 |
| A7 | `validate_directory_identity` 比较 `FILE_NAME_OPENED` 名字而非 file id | 逐组件 reparse 拒绝才是实际边界；名字比较是第二道检查 |
| A8 | 产品路径排除硬编码 `c:`，ProgramData 在其他盘时不生效 | 只有管理员能发 create；影响限于“允许把 bundle 放进产品自己的根”，不越权 |

## 4. 运行期检查清单（尚未执行，修复后逐条测）

1. 标准用户预创建 `C:\ProgramData\MyRamDisk`（及 `profiles\workers`）→ 安装 → `Get-Acl`：owner 是否仍为该用户、能否改 DACL/重命名；对照干净安装的 owner。
2. 安装后 `icacls "C:\Program Files\MyRamDisk"` 与四个二进制；标准用户尝试替换 `myrd-m5-worker.exe`；再用自定义 `-InstallDirectory` 重复。
3. `sc qc MyRamDiskService`：`BINARY_PATH_NAME` 是否带引号；前缀是否用户可写。
4. 标准用户抢注 `\\.\pipe\MyRamDisk.Control.v1` → 服务是否失败、CLI/GUI 是否拒连（B7 修复后应拒绝）。
5. 私有管道劫持：在服务创建管道后、worker 连接前杀服务；标准用户抢注名字并用秘密完成握手 → worker 是否拒绝（B1 修复后应拒绝）。
6. 标准用户 `OpenJobObject(JOB_OBJECT_QUERY|TERMINATE)` 打开 `Global\MyRamDisk.M5.Workers`；记录 job DACL。
7. worker 进程内 dump token（`whoami /priv`、完整性、组）与继承的 job 句柄权限（B5 修复后应为 QUERY）。
8. 非管理员 owner 用 junction 父目录 `disk export` 到 `C:\Windows\System32` → 应被拒绝（B3 修复后）。
9. 用带用户名的 bundle 路径触发注册失败 → 另一标准用户读 `service.log`：应读不到（B4 修复后 logs 为 management-only）。
10. 8 KB / 100 KB 的 bundle 路径 create/import → 应正常启动或明确拒绝（B6 修复后）。
11. 一个标准用户占 8 条空闲连接，测第 9 条命令的延迟（A5 量化）。

## 5. 状态

- 审计：**完成**（静态）。
- §2 阻断项：**未修**（下一步逐条修 + 对应运行期检查）。
- §3 接受风险：已在此记录。
- §4 运行期检查：**未执行**。
