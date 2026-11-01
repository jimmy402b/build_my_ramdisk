# R-16：一页支持说明与错误入口

## 1. 产出

- `docs/operations/m5-support.md`：一页，六节——支持范围、已知限制（逐条给测试或“未测”）、
  数据丢失上限、备份与导出、安装/卸载注意、出问题先看哪里。
- 入口：`README.md` 顶部新增 “M5 support” 一段，指向该页。
- 错误信息指向它：
  - 安装器的 5 条驱动相关拒绝（未安装、安装不完整、版本读不出、版本过旧、服务未运行）
    都追加 `docs/operations/m5-support.md`；
  - 产品侧 `driver_unavailable` 的消息改为
    `phdskmnt is disabled or absent; the driver requirement and the other known limits are on docs/operations/m5-support.md`。

## 2. 逐条限制与依据（文档评审用）

| 文档里的限制 | 依据（测试读数 / 明确未测） |
|---|---|
| 仅 Win11 x64 | 决策 4；Win10/Server/ARM64 **未测**且矩阵标“不支持” |
| 单盘 | `service_core.single_disk_create` 拒绝；本轮 `compatibility`/`cluster_size` 用例在第二块盘上收到同一拒绝（`the profile mount target is already registered…` / 单盘拒绝） |
| 卷标不支持、exit 2 | `integration.m5.cli_e2e`：`label.exit=2`、`label.result=refused-by-name` |
| 簇大小仅 create | `integration.m5.cluster_size`：`65536` / `restart` 仍 `65536` / 缺省 `4096` |
| 导出仅停机盘 | `integration.m5.cli_e2e`：`export exit=0`、重复 `exit=1`、generation 不变 |
| 只支持重装、无回滚 | `integration.m5.upgrade` 四条读数 |
| over-commit 拒绝 `0x560A` | `integration.m5.*` 预算拒绝读数（R-01） |
| 驱动前置，安装器不安装 | 三个显式条件 + 正路径（1.2.28.93）每次安装读数；**缺失/过旧分支客机无法实测** |
| 未签名发布 | `docs/operations/m5-release.md`；SmartScreen 后果写明 |
| 数据丢失上限 | `m5-stab-03a`：三次断电 `recovered_generation=2`、`outcome=last_committed`、`survivors_after_cut=0` |

## 3. 读数与说明

- 安装器文本改动后 `integration.m5.install_uninstall` 仍需通过（下一轮的 smoke 记录在本页
  更新；改动只涉及拒绝分支的措辞，正路径不受影响）。
- 产品侧 `driver_unavailable` 文本无法在本客机取得读数：本机驱动正常，构造“缺失/停用”需要
  卸载或停用 `phdskmnt`，`AGENTS.md` §4 禁止。唯一覆盖它的测试
  `integration.m4.product_composition` 的 `driver_unavailable_is_zero_write` 在本机有**既有**
  未捕获异常（`git stash` 对照改动前后相同），所以本轮也没有本机读数。
  **明确记为未测**，而不是“已通过”。
- 文档评审：逐条对照上表，六节齐全，每条限制都有测试或“未测”标记；与
  `m5-support-matrix.md` 无冲突（矩阵是细节版，本页是入口版）。

## 4. 复现

```powershell
# 安装/卸载路径（含新的拒绝文本，正路径）
powershell -File .\tests\e2e\test-m5-install-uninstall.ps1   # 客机
```
