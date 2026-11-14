# MyRamDisk

一个 Windows 11 的内存盘：**数据在 RAM，checkpoint 落盘，断电后回到最后一个已提交代。**

不是"又一个 ramdisk"。区别只在断电那一刻。

---

## 为什么还要做一个

Windows 上已经有几个能造内存盘的东西了。先说清楚区别：

| | ImDisk / AIM Toolkit | Primo Ramdisk | **MyRamDisk** |
|---|---|---|---|
| RAM 盘 | ✅ | ✅ | ✅ |
| 命令行 / 可自动化 | ✅ / 部分 | ❌ | ✅ |
| **断电后** | 数据全丢 | 数据全丢 | **回到最后一个已提交 checkpoint** |
| **恢复可验证** | — | — | **`generation` + `last_committed`** |
| 在线整理（compaction） | ❌ | ❌ | ✅（1.0.3 起） |
| 镜像文件挂载 | ✅ | ✅ | ❌（见"不支持"） |
| 许可 | AGPL / 免费 | 商业 | **AGPL-3.0** |

**如果你只是要一块快的内存盘，请直接用 ImDisk 或 AIM Toolkit**——它们更成熟、有更多用户、还免费。

这个项目只回答一个问题：

> **断电之后，数据还在不在；以及，你能不能证明它在。**

如果你不需要这个保证，本项目的所有复杂度对你都是净负担。

## 架构

本仓库**不包含内核驱动**。块设备由已签名的第三方驱动 `phdskmnt`（Arsenal Image Mounter）提供，本项目实现它的 **Shared Memory Proxy 后端**：

```
  用户进程
     │  myrd-cli / myrd-gui
     ▼
  myrd-service  (LocalSystem)  ── 生命周期、profile、operation
     │
     ▼
  myrd-m5-worker (SYSTEM)      ── 内存块存储 + journal + checkpoint
     │  IMDPROXY over 共享内存
     ▼
  phdskmnt.sys  (已签名，第三方)  ── 虚拟 SCSI 盘 + NTFS 卷
     │
     ▼
  Windows 文件系统
```

关键点：**每一次写都经过 worker 进程**，所以它才能记 journal。这是本项目与"内核直接持有内存"的内存盘（ImDisk / Primo）最根本的差别——也是速度上让一步的原因。

## 版本

最新版本见 **[Releases](https://github.com/jimmy402b/build_my_ramdisk/releases)** 与 tag `v1.0.x`。

> 本仓库 `main` 分支对应 **1.0.2**；`v1.0.3`、`v1.0.4` 以 tag 形式发布，代码在 `feature/m5-lifecycle` 分支上。
> 想用最新版请取 tag，不要只看 `main` 的文件树。

发布包**未签名**，附 SHA-256 校验和。

## 环境要求

| | |
|---|---|
| OS | Windows 11 x64（实测 build 26200；其他 Win11 版本未逐个验证） |
| 驱动 | `phdskmnt` **>= 1.2.28.93**，已安装且**正在运行**。安装程序只检查，不安装/替换/降级驱动 |
| 磁盘 | 单盘；NTFS；簇大小 4/8/16/32/64 KiB（默认 4 KiB） |
| 内存 | `automatic` = `min(物理内存/4, 容量)`；`fixed` 必须 ≤ 容量且 ≤ 机器提交上限（超出报 `0x560A`） |

> `phdskmnt` 由 Arsenal Image Mounter / AIM Toolkit 的安装包提供，本项目不分发它。

## 数据丢失边界

**只有已完成的 checkpoint 里的数据能存活。**

断电后磁盘回到最后一个已提交的代；该 checkpoint 之后的写入丢失——最多一个 checkpoint 间隔，默认 **60 秒**（`--checkpoint-interval`，或用 `off` 得到纯易失盘）。

## 不支持

Windows 10 / Server / ARM64 · 多盘（已延后）· 卷标 · 就地版本回滚（重装是唯一受支持的升级路径）· 超出提交上限的超配 · BitLocker 或 SMB 上的 bundle（未测）· 4Kn 介质（未测）· 镜像文件挂载 · 镜像压缩。

## 文档

| 主题 | 位置 |
|---|---|
| 支持范围、驱动要求、卸载与日志 | [`docs/operations/m5-support.md`](docs/operations/m5-support.md) |
| 支持矩阵 | [`docs/operations/m5-support-matrix.md`](docs/operations/m5-support-matrix.md) |
| 版本历史 | [`CHANGELOG.md`](CHANGELOG.md) |
| 1.0.2 / 1.0.1 / 1.0.0 发布说明 | [`docs/operations/m5-release-1.0.2.md`](docs/operations/m5-release-1.0.2.md)、[`1.0.1`](docs/operations/m5-release-1.0.1.md)、[`1.0.0`](docs/operations/m5-release-1.0.0.md) |
| ≥8 GiB 创建卡死与修复记录 | [`docs/operations/m5-known-issue-2026-11-04-large-disk-create.md`](docs/operations/m5-known-issue-2026-11-04-large-disk-create.md) |
| 1.0.3 / 1.0.4 说明 | 在 `v1.0.3` / `v1.0.4` tag 上 |

## 构建

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset release
ctest --preset release --output-on-failure
```

> 改过任何头文件后，测量前必须 `--clean-first`——增量构建不会重编所有包含该头文件的 TU。

## 许可

**GNU Affero General Public License v3.0**，见 [`LICENSE`](LICENSE)。

AIM 协议声明参考自 Arsenal Image Mounter（AGPL-3.0），固定提交 `d379caa027d21902e45fce8c9daf02fd116c2dda`；IMDPROXY 协议声明参考自 Olof Lagerkvist 的 `imdproxy.h`（MIT-style）。完整第三方声明见 [`THIRD_PARTY.md`](THIRD_PARTY.md)。
