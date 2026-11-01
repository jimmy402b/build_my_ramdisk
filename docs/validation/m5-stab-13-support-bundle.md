# R-15：支持包（support bundle）

## 1. 产出

`tools/New-M5SupportBundle.ps1`——随发布物提供的一条命令：

```powershell
# 以管理员运行；默认在 zip 里脱敏
powershell -ExecutionPolicy Bypass -File tools\New-M5SupportBundle.ps1 `
    -Output .\MyRamDisk-support.zip
```

参数：`-Output`（已存在则拒绝覆盖）、`-InstallDir`（默认 `C:\Program Files\MyRamDisk`）、
`-DataRoot`（默认 `C:\ProgramData\MyRamDisk`）、`-CliPath`（测试/便携场景）、
`-LogTailKb`（默认 256）、`-RecentOperations`（默认 20）、`-IncludePaths`（**显式**保留绝对路径，
默认脱敏）。

## 2. 包里有什么

| 文件 | 内容 |
|---|---|
| `environment.json` | 采集时间、工具版本、PowerShell 版本、是否管理员、OS 名称/版本/build、架构、计算机名、用户名 |
| `versions.json` | 四个二进制的 FileVersion/ProductVersion/大小/SHA-256 + `myrd-cli --version` |
| `driver.json` | `sc query phdskmnt` 全文、驱动文件版本与 SHA-256 |
| `service.json` | `MyRamDisk*` 服务状态 |
| `commands/disk-list.txt`、`commands/disk-show-<id>.txt` | CLI 的只读视图（**只用真实存在的动词**；CLI 没有 `status`/`driver status`/`operation list`） |
| `operations/<id>.txt` | 最近 N 个操作的 `operation show` 投影（id 来自 `operations/*.myop`） |
| `failure.json` | **最近一个失败操作**的 id 与完整 show 输出 |
| `profiles.json` + `manifests/<id>.disk.rdm` | profile 目录清单与每个 `disk.rdm` 原样拷贝 |
| `logs/service.log`、`logs/worker.log` | 每个日志的最后 256 KiB（缺失时写 `(absent: …)`） |
| `manifest.json` | 每个文件的相对路径/字节数/SHA-256 + `failure` + `redacted` |

## 3. 脱敏

默认把 **计算机名** → `%COMPUTERNAME%`、**用户 profile 路径** → `%USERPROFILE%`、
其余 `X:\...` 绝对路径 → `<path>`。脱敏发生在写盘之前，`manifest.json` 的哈希是对**已脱敏**
内容算的。`-IncludePaths` 才保留绝对路径（给维护者明确索取时用）。

## 4. 客机读数（`integration.m5.support_bundle` 1/1，74 s）

用例先制造一次**真实失败**（bundle 目录里已有一个外来文件，create-bundle 失败，
操作进入 `failed`），再让“陌生用户”跑一条命令：

```text
READING: failure.cli_exit=0
READING: failure.operation_state=failed
READING: tool.exit=0
READING: archive.bytes=3870
READING: manifest.files=10
READING: manifest.redacted=True
READING: manifest.failure.operation=3e9a9b8a-d0e5-45ec-8d9a-7eb70ccb19c8
READING: logs.service_bytes=3337
READING: redaction.offenders=0
PASS integration.m5.support_bundle: one command produced a redacted, self-explaining archive
```

断言内容：`manifest.json` 可解析且 `failure.operation_id` 等于失败操作；`operations/<id>.txt`
在包里且写明 `failed`；`environment/versions/driver/service/profiles/logs/disk-list` 全在；
包里任何文本文件都不含 `%USERPROFILE%` 原串或计算机名。

## 5. 调试记录（两条真实断路）

1. **CLI 没有 `status`/`driver status`/`operation list`**：第一版工具按直觉调用了它们，
   客机直接 `unknown command 'status'`。现在只用 `disk list`/`disk show`/`operation show`/`--version`，
   操作 id 从 `operations/*.myop` 文件名枚举。
2. **原生 stderr 在 `$ErrorActionPreference='Stop'` 下是终止错误**：CLI 把拒绝写到 stderr，
   工具正是要记录这些拒绝，于是把偏好临时降为 `Continue` 并显式取 `$LASTEXITCODE`。

## 6. 未测

- 非管理员运行：日志目录是 management-only（B4），标准用户读不到；工具会照常生成包但日志
  标 `(absent: …)`。这条路径**未在客机实测**（没有标准用户会话），文档与用例都按“管理员运行”写。
- `-IncludePaths` 路径：功能存在，本用例只测默认脱敏路径。
