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
