# MyRamDisk

MyRamDisk 是一个前台运行的、持久化的 Windows RAM disk 产品。它使用已单独安装的 Arsenal Image Mounter (AIM)；本项目遵循 GNU Affero General Public License v3.0，详情见 [LICENSE](LICENSE)。

## 支持范围

v1 支持 Windows 11 x64 上的一个 AIM-backed NTFS RAM disk。必须在提升权限的 Administrator PowerShell 中运行，并且 AIM 已安装且可用；本发行包不包含 AIM 或驱动安装程序。bundle 目录必须位于本机 NTFS 卷；创建时指定的盘符必须当前空闲。产品不承诺广泛 Windows 认证，也不提供服务、GUI、开机自启、多磁盘、驱动安装或在线压缩。

## 快速开始

```powershell
.\myrd.exe run --bundle "D:\MyRamDisk\work" --size 4G --drive-letter R:
```

`--size` 接受 64 MiB 到 1 TiB 的 `K`、`M`、`G` 或 `T` 后缀。bundle 目录必须位于本机 NTFS 卷，且创建时指定的盘符必须当前空闲。默认 checkpoint 间隔为 600 秒，脏数据阈值为 256 MiB，RAM 上限为 2 GiB；可分别用 `--checkpoint-seconds`、`--checkpoint-dirty-mb` 和 `--ram-limit` 调整。

## 创建与恢复

不存在或为空的 bundle 会创建新的镜像和 NTFS 卷。包含完整 `disk.rdm` 的 bundle 会恢复既有镜像；恢复路径绝不会格式化卷。恢复必须使用与原来相同的 `run` 命令（特别是相同的 bundle、大小和盘符）。

## Checkpoint 与 journal

运行中会按间隔或脏数据阈值发布 checkpoint；只有已发布的 checkpoint 才能在非正常停止后恢复。journal 到 80% 时会给出一次警告。journal 写满时，产品停止后续 checkpoint 并在安全清理后以退出码 1 结束；此时先离线压缩，再重新运行原来的 `run` 命令。

## 离线压缩

仅在 `myrd run` 已停止、且 bundle 写入锁可独占取得时执行：

```powershell
.\myrd.exe compact --bundle "D:\MyRamDisk\work"
```

离线压缩不启动或发现 AIM，只需要 bundle 文件系统访问和独占写入锁。v1 没有在线压缩；相关未来工作见 [docs/deferred/online-compaction.md](docs/deferred/online-compaction.md)。

## 安全关闭与强制终止

先关闭使用该盘的应用，再按 Ctrl+C。MyRamDisk 按锁定卷、验证 extent、刷新、最终 checkpoint、卸载、移除盘符、停止运行时的顺序清理；若卷被占用，请关闭 Defender 或 Windows Search 对该盘的扫描/索引后重试 Ctrl+C。

普通关闭后，等待 `myrd` 退出，运行相同 bundle 的 `compact`，再运行原来的 `run` 命令。若 journal 已满，也按 Ctrl+C 等待安全清理（退出码 1），离线压缩后再重启。若产品进程被强制终止，只能终止该产品进程；在恢复前先对 VMware 或 Windows 执行硬重置，然后重新运行完全相同的 `run` 命令。强制终止不承诺清理或身份后续处理。

## 退出码

`0` 表示成功；`1` 表示运行、checkpoint、压缩或清理失败；`2` 表示命令行用法错误。

## 已知限制

只支持一个前台磁盘。没有服务、GUI、自动恢复、自动压缩、在线压缩、多磁盘或驱动安装。不要在产品运行时执行离线压缩。

## 构建与验证

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset debug --target myrd
ctest --preset debug --output-on-failure
```

## v1.0 来宾资格测试（高级/发布维护者）

本节是发布维护者在已批准、隔离的 Windows 来宾中执行的资格流程，不是普通用户安装步骤。资格 controller、helper 和 kit 只用于验证；资格脚本不进入 v1 六文件 release ZIP。先在 host 生成并冻结 qualification kit，再把**整个** kit 复制到 approved isolated guest 的本机 NTFS 目录。不要从网络共享、可移动介质或不完整的 kit 运行。

在 host 的仓库根目录、完成 `release` 构建后，以下命令生成一个新的、带当前 commit 限定符和随机后缀的 kit 目录。先按你的本机 NTFS 路径设置 `$KitParent`（不使用任何特定用户目录）；脚本拒绝覆盖已存在的输出目录。`Test-Path` 和 SHA-256 输出用于在复制前确认两个 build 输入确实存在且已记录：

```powershell
$SourceRoot = (Resolve-Path .).Path
$Artifact = Join-Path $SourceRoot 'out\build\msvc\src\Release\myrd.exe'
$Query = Join-Path $SourceRoot 'out\build\msvc\tests\Release\myrd_m4_aim_query.exe'
$KitParent = 'D:\M4QualificationKits'
$Commit = (git -C $SourceRoot rev-parse --short=12 HEAD).Trim()
$OutputDirectory = Join-Path $KitParent ('qualification-kit-' + $Commit + '-' + [Guid]::NewGuid().ToString('N'))

Test-Path -LiteralPath $SourceRoot,$Artifact,$Query,$KitParent
Get-FileHash -LiteralPath $Artifact,$Query -Algorithm SHA256
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\New-M4GuestQualificationKit.ps1 -SourceRoot $SourceRoot -Artifact $Artifact -Query $Query -OutputDirectory $OutputDirectory
```

确认命令输出的 `$OutputDirectory` 后，冻结该目录内容并完整复制整个 kit 到 approved isolated guest；不要向既有 kit 目录重跑，也不要只复制其中若干文件。

在来宾的管理员 PowerShell 中，进入已复制的 kit 目录后，仅运行下面这一个命令；省略 `-Status` 即为开始或恢复，必须始终使用同一个随机 QualificationRoot（本机 NTFS 绝对路径）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Invoke-M4GuestQualification.ps1 -QualificationRoot <本机NTFS绝对路径>
```

每次脚本返回后，只有在它已明确打印相应的 armed 标题及其全部提示行时，才可执行一次人工 UI 动作。不得使用 Windows 正常关机、脚本 shutdown，也不得额外 reset；断电/重置后启动 Windows，再以同一命令和同一随机 QualificationRoot 恢复。

| 唯一人工 UI 动作 | 必须先等待的脚本提示 | 允许的动作与恢复 |
| --- | --- | --- |
| Reset 1 | `CRASH RESET ARMED`（Formal CreateKill 后） | VMware UI 中恰好一次 Reset 或 Power Off/On；回来后运行同一命令。 |
| Reset 2 | `LIVE POWER-CUT ARMED`（live power-cut armed 后） | VMware UI 中恰好一次 Reset，或 Power Off/On，以模拟突然断电；回来后运行同一命令。 |
| Reset 3 | `MISSING-DRIVER RESET ARMED`（`phdskmnt` Disabled missing-driver 后） | VMware UI 中恰好一次 Reset 或 Power Off/On；回来后运行同一命令。 |

三次且仅三次 UI 动作是资格证据的一部分。每一轮都必须等待完整的 armed 提示，不能提前操作。QualificationRoot 与其中的 evidence 是证据链：成功后保留，不得删除或复用；失败 root 永久保留以便诊断，不得重试或新建替代 root 来掩盖失败。

需要只读查看状态时，使用同一个 root：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Invoke-M4GuestQualification.ps1 -QualificationRoot <本机NTFS绝对路径> -Status
```

退出码 10 表示仍需执行已 armed 的 reset，且该等待路径不写入状态；退出码 77 表示 `Blocked` 或安全拒绝；退出码 0 表示当前阶段成功，最终完成时输出唯一成功行 `M4 v1 QUALIFICATION PASS`。除该唯一 PASS 行外，不应把中间阶段成功视为 v1 资格通过。

本流程的静态安全契约检查 PowerShell AST 中的直接调用、常见删除别名和 literal call-operator；它不声称能够证明任意运行时拼接命令的行为。

## 许可证

MyRamDisk 使用 GNU Affero General Public License v3.0。固定 AIM 协议参考来自 Arsenal Image Mounter 的 AGPL-3.0 源码；AIM 必须独立获取和安装。
