# MyRamDisk M5 支持说明（一页）

> 这份文档是用户能看到的最短版本：支持什么、不支持什么、最坏会丢什么、怎么备份、装之前要
> 什么、卸载注意什么。每一条“限制”都给出对应的测试或明确写“未测”。
> 细节：`m5-support-matrix.md`、`docs/validation/`。

## 1. 支持范围

| 项 | 值 |
|---|---|
| 操作系统 | **Windows 11 x64**（决策 4）。Windows 10 / Server / ARM64 不支持，也不做冒烟 |
| 磁盘 | **单盘**：同一时间只允许一块 MyRamDisk 盘（第二块 create/start 被明确拒绝） |
| 挂载 | 盘符在 create 时指定并记录；每次 start（含服务重启、VM 重启）都用该字母。若字母已被**卷**占用，start 返回错误而不是换字母（见 `docs/validation/m5-drive-letter.md`） |
| 文件系统 | NTFS（默认 4 KiB 簇，可在 create 时选 4/8/16/32/64 KiB） |
| 驱动 | `phdskmnt` **≥ 1.2.28.93**，安装器**只检查、不安装** |
| 权限 | 安装/创建/修改需要管理员；非管理员只能做只读查询（见 §6） |

不支持：卷标（`--volume-label` 在入口被拒，exit 2）、in-place 版本回滚（只能重装）、
over-commit（固定预算超 commit limit 在 create 被拒）、BitLocker/SMB 上的 bundle（未测）。

## 2. 已知限制（每条有测试或“未测”）

| 限制 | 依据 |
|---|---|
| 单盘：第二块盘 create/start 被拒 | `integration.m5.*` 的单盘拒绝读数；`service_core.single_disk_create` |
| 卷标不支持，入口报 `--volume-label is not supported`、exit 2 | `integration.m5.cli_e2e`：`label.exit=2`、`label.result=refused-by-name` |
| 簇大小只在 create 时可选，之后不可改 | `integration.m5.cluster_size`：64 KiB → `65536`，重启后仍 `65536`，缺省 `4096` |
| 导出只支持**已停止**的盘 | `integration.m5.cli_e2e`：停止盘 `export exit=0`、第二次 `exit=1`（拒绝覆盖）、generation 不变 |
| 升级只有覆盖安装/重装，没有版本回滚（决策 5） | `integration.m5.upgrade`：覆盖安装/保留数据卸载/重装/脏卸载四条读数 |
| over-commit 被拒，错误码 `0x560A`（决策 1） | `integration.m5.*` 的预算拒绝读数 |
| **≥8 GiB 的盘 create 会卡死**（1.0.0 已知缺陷；4 GiB 已验证可用） | `docs/operations/m5-known-issue-2026-11-04-large-disk-create.md` |
| 驱动缺失/过旧时安装器拒绝 | 三个显式条件在 `tools/install/Install-MyRamDisk.ps1`；**客机无法卸载驱动，这三个分支未实测** |
| 没有签名证书 → 未签名发布（决策 2） | 发布说明必须写 SmartScreen 后果；`docs/operations/m5-release.md` |

## 3. 数据丢失上限

- **已提交（一次 checkpoint 成功完成）的数据在断电后保留。** 三次独立硬断电读数：
  `recovered_generation=2`、`outcome=last_committed`、`survivors_after_cut=0`、盘可启动、
  无残留（`m5-stab-03a-p4-p5-power-cut.md`）。
- **未提交的写入可能丢失**，最多是最后一次成功 checkpoint 之后写入的内容。
- 停止时的易失数据需要 `--accept-volatile-data-loss` 明确确认，否则 stop 拒绝；
  脏盘强停会丢弃未提交写入并在日志里写明（`dirty writes discarded`）。
- 硬断电、worker 崩溃、service 崩溃、bundle 损坏、磁盘满、权限错误六类故障都不挂死、
  不残留设备（`m5-stab-03b`、`03c`）。

## 4. 备份与导出

- **停机导出单文件**：`myrd-cli disk export <disk> --output <file.vhdx>`（盘必须先 stop；
  目标已存在会拒绝覆盖；导出不改变 generation）。
- **整目录备份**：服务停止后复制 bundle 目录（`disk.rdm` + `base-*.vhdx` + `journal-*.rdj`）
  即可；恢复时把目录放回并 `disk import`。
- 卷内容是 NTFS；在盘运行期间用普通文件复制备份，只能保证 checkpoint 边界一致。

## 5. 安装与卸载注意

- 安装：管理员运行 `Install-MyRamDisk.ps1`；先检查 `phdskmnt` 版本与运行状态，再复制二进制，
  最后注册服务并创建 `%ProgramData%\MyRamDisk`（owner = Administrators，仅管理面可读写）。
- 卸载：**保留数据**（profile、bundle、日志都不删）；重装后盘仍可用（`integration.m5.upgrade`）。
- 卸载/升级前先 `disk stop`；运行中卸载会先停服务并清理 worker。
- 卸载不会卸载/降级 `phdskmnt`（本产品从不安装或替换该驱动）。

## 6. 出问题先看哪里

1. `%ProgramData%\MyRamDisk\logs\service.log`：服务启动失败的原因（含 native 错误码）。
2. `%ProgramData%\MyRamDisk\logs\worker.log`：worker 启动/命令失败的原因。
3. `myrd-cli disk list` / `operation show <id>`：当前状态与操作终态。
   注意 `operation show` 对失败操作返回 exit 1，这是结果而不是命令错误。
4. 支持包：管理员运行 `powershell -ExecutionPolicy Bypass -File tools\New-M5SupportBundle.ps1 -Output .\MyRamDisk-support.zip`；默认脱敏（计算机名/用户路径/绝对路径），包里的 `failure.json` 直接指向最近一次失败操作。
