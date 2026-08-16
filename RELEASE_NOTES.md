# MyRamDisk v1.0.0 Release Notes

Release date: 2026-08-15

MyRamDisk v1.0.0 supports one persistent, AIM-backed NTFS RAM disk on Windows 11 x64. Run it from an elevated Administrator PowerShell session with Arsenal Image Mounter (AIM) already installed and available. AIM is a separate prerequisite and is not included in this release archive. bundle 目录必须位于本机 NTFS 卷；创建时指定的盘符必须当前空闲。

Public commands:

```powershell
.\myrd.exe run --bundle "D:\MyRamDisk\work" --size 4G --drive-letter R:
.\myrd.exe compact --bundle "D:\MyRamDisk\work"
```

`run` creates an empty bundle, or recovers an existing complete bundle without formatting it. While it runs, MyRamDisk publishes checkpoints and can recover the last successfully published checkpoint after an unclean stop. `compact` is an offline operation for a stopped bundle and reduces persisted generations.

Fixed v1 limits: one disk, foreground process only, no GUI, service, autostart, driver installation, online compaction, or broad Windows certification claim. Close applications before Ctrl+C so the volume can be locked safely. A forced termination requires a VMware or Windows hard reset before recovery, then rerunning the identical `run` command.

## 来宾资格测试边界

资格 controller/helper/kit 仅为验证工具，不进入 v1 六文件 ZIP，也不是终端用户功能。v1 的支持边界仍是 Windows 11 x64 上的单一 AIM-backed NTFS RAM disk；资格 kit 只可由发布维护者在已批准、隔离的来宾本机 NTFS 路径中使用。

三次真实 UI 测试尚待用户明确启动。在获得该明确启动前，不会启动真实 VM、SSH、driver、device、reset 或断电操作；本版本说明不把本地静态契约或 controller 自测表述为真实来宾资格 PASS。
