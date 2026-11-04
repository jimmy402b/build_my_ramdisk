# MyRamDisk 1.0.1 发布说明（2026-11-04）

**产品**：Windows 11 x64 单盘 RAM disk（service + SYSTEM worker + AIM 虚拟盘 + NTFS 卷）。
**版本**：`1.0.1`（四个二进制自报 `1.0.1`）。
**分支/提交**：`feature/m5-lifecycle`；构建对应的产品提交见 §7。
**状态**：发布候选；**未签名**。
**范围**：只修 ≥8 GiB create 与「脏集 > 2 GiB 的 checkpoint」，不改变已经工作的盘的行为。

## 1. 交付物

```text
out/release/MyRamDisk-1.0.1-win11-x64/
  bin/      myrd-service.exe  myrd-cli.exe  myrd-gui.exe  myrd-m5-worker.exe
  install/  Install-MyRamDisk.ps1  Uninstall-MyRamDisk.ps1
  docs/     m5-support.md  m5-support-matrix.md
  README.txt
  release-manifest.json
  SHA256SUMS
```

二进制哈希（与冻结 manifest 一致）：

```text
myrd-service.exe    3C4A0B289B47CB46D47838AE4BF2AF8EC193C5912668DFD8B1CFD9077D8005C1
myrd-cli.exe        78C1C89CB9D1672208B1BC495878220692006736E06272BC9AA2B164FAE136AE
myrd-gui.exe        57C90A9C123BD2307D832E46A8A61478D0D4ABA70A37456B21670A6504421B30
myrd-m5-worker.exe  8B38FF285CAEF6217BBCA2A8A1662431574FD6616F311B84421230421FBE6601
```

**可重复构建**：同一 commit 两次 `cmake --build out/build/msvc --config Release --clean-first`
产出四个二进制 SHA-256 **完全一致**（2026-11-04 实测，两次构建都记录在上面的哈希里）。

## 2. 修了什么（每个都有客机读数）

1. **≥8 GiB `disk create` 死锁**（`7a1d4d1`）：automatic 预算未按块对齐、create 的
   generation-zero checkpoint 被 120 s 普通命令上限打断、服务 `operation wait` 30 s 与 CLI
   30 s 交换预算对撞。修复后 8 GiB automatic 125 s、16 GiB fixed:12GiB 314 s、16 GiB automatic
   327 s，全部 `disk create --wait` exit 0。
2. **已提交的 checkpoint 被记成 dirty**：非 stop checkpoint 的干净判据比较
   `live_dirty_epoch`（worker 从不设置，恒 0）与 cut 代次；每个 checkpoint 都 `succeeded_dirty`，
   `persistence=Dirty` 永不清、维护 checkpoint 每 60 s 重来（旧读数：generation 2→15、
   journal 9,112,297,472 B）。现在两条路径都用 worker 的块级事实 `post_commit_dirty`。
3. **恢复读端固定 2 GiB**：`b80a6e5` 只缩放了 M3 provider；`disk start` 先走的
   `M4PersistenceAdapter` 仍用 2 GiB，于是 9.1 GiB 的已提交 journal 被自己的盘拒绝
   （`no fully valid manifest generation exists`）。`recovery_persistence_limits()` 现在是
   M3 provider / M4 adapter / worker 默认恢复选项共用的唯一规则。
4. **断电落在 start 的 `Starting/Recovering` 窗口会永久卡住**：启动扫描只 retire `running`，
   非终止的 `disk.start` 一直占着 profile gate，之后所有 start 都被 `0x5605` 拒绝而
   `disk show` 仍说 Healthy。现在 sweep 也 retire `starting`，并把中断的 start 终止、释放门。

## 3. 验收读数（16 GiB / fixed:12GiB，7 GiB write-through，同一用例）

| 项 | 读数 |
|---|---|
| create | `disk create --wait` exit 0；卷 17,162,039,296 B NTFS；checkpoint generation 2 |
| 写入 | 12 s；7,516,192,768 B；SHA-256 `AF76B0B8A76FDAB57DA646151D07E028D89D594B98756D0A44EBAF143E528554` |
| checkpoint | gen 3 `post_commit_dirty=1`（写在 cut 之后仍脏）→ gen 4 `post_commit_dirty=0` → gen 6 `Idle/Clean`；journal 9,111,715,840 B 已提交 |
| 硬断电后 | 自动恢复为 `already-running`（generation 6）；文件长度 7,516,192,768 B、digest 一致；第二块盘仍被 `0x5609` 拒绝 |

## 4. 全量门禁与证据

（待本轮门禁完成后填写：A1/A2 各 158、B 5、fault 6、manifest `frozen_at`、validator 结果、
发布包复算。）

## 5. 支持与限制（照 `docs/operations/m5-support.md`）

与 1.0.0 相同：Windows 11 x64、单盘、NTFS（4–64 KiB，create-only）、`phdskmnt ≥ 1.2.28.93`
（安装器只检查）；不支持 Win10/Server/ARM64、第二块盘、卷标、in-place 回滚、over-commit、
BitLocker/SMB 上的 bundle、运行中 export、改簇大小。未签名，以 SHA-256 作为完整性校验。

## 6. 已知限制（不隐藏）

1. `unit.m3.vhdx_reader`（`current_cache_entries == 0`）偶发失败，单独跑通过——与 1.0.0 相同，
   定性安排在发布后。
2. fault 注入后 Windows WMI/PowerShell 会被拖住，fault 组必须每例一次硬重启；环境限制，
   不是数据路径失败。
3. 断电打断一次正在执行的 start 时，该 start 会在下次启动被标记为失败（`restart: an interrupted
   start was terminalized...`）并自动重来；**正在进行的操作不会续跑**，只会重试。这是本轮
   修复的语义，发布说明里明确写出。
4. 驱动缺失/旧驱动分支、start 时字母被卷占用的分支未做破坏性实测（约束/环境原因）。
5. `disk show` 的 `mem=` 在 checkpoint 期间可能读到 `0/0`（worker 忙时不回答状态），
   完成后恢复；不影响数据。

## 7. 构建与提交

（待填：最终 commit、manifest `frozen_at`、validator 输出、打包复算。）
