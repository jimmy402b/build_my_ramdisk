# 盘符：指定与跨重启稳定（R-19 后续验证）

**问题**：盘符能不能指定？重启后会不会变？

**结论**：**能指定，且当前实现会在每次 start（含服务重启、VM 重启）重新使用 create 时记录的那个盘符**；
如果该字母已被**卷**占用，start 返回错误，不会静默换一个字母。

## 1. 实现路径（代码）

| 环节 | 位置 | 行为 |
|---|---|---|
| 指定 | `disk create --mount R:` | 盘符写进 profile 的 `mount_target` |
| 传给 worker | `src/host/m5_worker_main.cpp:243` | `config.drive_letter = spec.mount_target[0]` |
| 每次 start 赋给卷 | `src/run/disk_runtime.cpp:218` | `operations_.assign_drive_letter(volume, letter)` |
| 新建时占用检查 | `src/run/provision_volume.ps1:76-83` | `Get-Volume -DriveLetter` / `Get-PSDrive` 命中即 `throw 'requested drive letter is occupied'` |

## 2. 客机读数（2026-11-02，`integration.m5.drive_letter`）

**stability 模式**（同一 service，脚本 `tests/e2e/test-m5-drive-letter.ps1`）：

```text
stability.letter=T:
stability.id=06e455ba-b54d-4cba-b9ae-c0e3a6b89be6
first_start_letter=T:      first_start_volume=yes
restart_letter=T:          restart_volume=yes          (stop -> start)
service_restart_letter=T:                             (sc stop/start 服务后再 start)
```

**after-reboot 模式**（宿主机 `vmrun stop hard` + `start` 之后）：

```text
reboot.id=06e455ba-b54d-4cba-b9ae-c0e3a6b89be6
reboot.letter=T:
reboot.volume_present=yes
reboot.list_mounts=T
reboot.forget_exit=0
drive_letter.reboot_result=PASS
```

即：**create 时指定的 `T:` 在 stop/start、服务重启、VM 硬重启后都保持不变**。

## 3. 占用行为（实测记录）

- 用 `subst T: <dir>` 占住字母时，`disk start` **仍然成功**且卷拿到 `T:`（subst 不是卷挂载点，
  不参与挂载管理器冲突）；这条**不是**“被卷占用”的场景。
- 真正被**卷**占用的分支只在**新建**路径上有检查（`requested drive letter is occupied`，
  `provision_volume.ps1`）；`start` 路径没有单独的冲突检查，冲突会以挂载/赋字母失败的形式报错。
  该分支**未做破坏性实测**（需要第二块卷长期占住同一字母），如实标注。

## 4. 限制与注意

- 换字母：`disk forget` 后 `disk import --name … --bundle … --mount <新字母>` 重新登记（**未实测**）。
- 没有 `disk start --mount` 这种临时改字母的参数；`disk config set` 只支持 `auto_start`。
- 这条用例是 **host 驱动**（`tools/m5vm/Test-M5DriveLetter.ps1`，因为重启半步不能在 guest 用例里完成），
  因此**不在** 163 例发布门禁内；门禁口径见 `docs/operations/m5-release-rehearsal.md` §9。

## 5. 复现

```powershell
# 稳定性 + 重启两半，一次跑完
tools\m5vm\Test-M5DriveLetter.ps1
```

（本期实测时该驱动的两半步分别手动执行过一次：stability 于 11:51Z、after-reboot 于 11:59Z，读数见 §2。）