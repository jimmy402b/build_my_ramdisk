# STAB-06 / R-07 + R-08：升级、覆盖安装、重装、脏卸载、数据格式版本 —— **PASSED**

**结论**：两条客机用例 + 一次 install_uninstall，覆盖 R-07（三处版本一致）与 R-08 的全部路径。

**客机**：`myrd-vm`，Windows 11 x64 26200，Release；每次运行前 `Reset-M5Guest`。

---

## 1. 覆盖安装 / 保留数据卸载 / 重装 / 脏卸载（`integration.m5.upgrade`，1/1，33 s）

```text
clean.exit=0                      （先 purge 卸载，保证从干净开始）
install1.exit=0                   （安装器打印 phdskmnt 检查）
profile_id=c7a0410a-…
overwrite.exit=0
overwrite.profile_still_listed=yes
after-overwrite.runtime=Running
after-overwrite.marker_intact=yes
keep.exit=0                       （卸载，保留数据）
keep.processes_left=0
keep.install_dir_exists=False
keep.data_dir_exists=True
reinstall.exit=0
after-reinstall.runtime=Running
after-reinstall.marker_intact=yes
dirty.exit=0                      （盘仍在运行时卸载）
dirty.processes_left=0
dirty.volume_gone=True
dirty.data_dir_exists=True
final.exit=0
after-dirty.runtime=Running
after-dirty.marker_intact=yes
cleanup_forget=0
PASS integration.m5.upgrade: overwrite, keep-data uninstall, reinstall, dirty uninstall
```

判定：覆盖安装后同一块盘仍注册、可启动、数据完整；保留数据卸载只移除服务与文件、保留记录，重装后
同一块盘可启动；**运行中卸载**会先把盘停掉（无进程残留、卷消失、记录保留），重装后数据仍然完整。
accepted upgrade path 是“重装”，不是版本回滚。

**这一轮修掉的安装器缺陷（R-08 的核心）**：覆盖安装时服务还在运行，`Copy-Item` 被
“文件正被使用”拒绝（`myrd-service.exe`）。安装器现在**在复制前先停掉并删除旧服务**（等 STOPPED，
最多 120 s），复制完再注册启动；这也让运行中的盘走产品自己的停止路径，而不是被换掉二进制。

## 2. 数据格式版本（`integration.m5.state_format`，1/1，30 s）

durable 记录的头部是 magic(8) + major(u16) + minor(u16)；把 `service_state.myrs` 的 major 从 1 改成
32767 后，按产品方式启动已安装的服务：

```text
state.path=C:\ProgramData\MyRamDisk\profiles\<id>\service_state.myrs
state.bytes=158
state.schema_major_before=1
state.schema_major_patched=32767
refusal.start_exit=0            （sc start 返回 0，但服务立刻退出）
refusal.service_running=False
--- service log tail:
  2026-11-01T05:56:22Z myrd-service: start failed:
    op=profile-registration.state.decode
    msg=Record schema major is unsupported native=0
refusal.names_schema=yes
--- restore
state.schema_major_restored=1
restored.service_running=True
restored.list_exit=0
PASS integration.m5.state_format
```

判定：不支持的 schema major **明确拒绝**，不是解码成“看起来可恢复”的东西；恢复原始字节后服务能起来、
记录完好。

**这一轮修掉的产品缺陷**：已安装服务在启动失败时只写 stderr（服务模式下没有 stderr），`sc query`
只能看到 1066。现在 `failure_exit_code` 把同一行追加到
`C:\ProgramData\MyRamDisk\logs\service.log`（安装器已把该文件设为 Users 可读），所以上面那行
`start failed: … Record schema major is unsupported` 是**用户能读到的**。

## 3. R-07 版本一致（`integration.m5.install_uninstall`，1/1，10 s）

```text
phdskmnt 1.2.28.93 present and running (floor 1.2.28.93)
cli: 1.0.0 / service: 1.0.0 / gui: 1.0.0
consistent version: 1.0.0 (matches product_version.h)
UNINSTALLED   service and files gone, data directory kept, no processes left
```

R-07 保持 PASSED；R-06 的驱动检查正路径也由这一次安装读数证明。

## 4. 未覆盖的分支（明确写出来）

- **版本回滚**：不支持（裁定 5），没有可测路径。
- **驱动缺失/过旧/未运行**：不可在本客机复现（不允许卸载或降级 `phdskmnt`），只验证了 1.2.28.93
  正路径；三个否定分支的判定在安装器里是显式条件。
- **跨大版本的数据迁移**：1.0.0 是首个发布格式；schema major 1 之外的记录被拒绝（§2），没有“从
  旧版本升级上来”的路径可测。
