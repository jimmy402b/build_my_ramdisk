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

## 许可证

MyRamDisk 使用 GNU Affero General Public License v3.0。固定 AIM 协议参考来自 Arsenal Image Mounter 的 AGPL-3.0 源码；AIM 必须独立获取和安装。
