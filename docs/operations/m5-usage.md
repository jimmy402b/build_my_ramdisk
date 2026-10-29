# 使用说明（M5）

> 这份文档回答两个问题：**这块盘能信到什么程度**，以及**怎么用命令操作它**。
> 内部实现与验收读数在 `docs/validation/`，操作与交接在 `m5-handoff.md`。

---

## 1. 它是做什么的

MyRamDisk 用内存做一块 Windows 盘：卷由 AIM 驱动呈现在系统里，普通的读写就落在内存里，
看起来和一块真的硬盘一样（盘符、文件系统、任何程序都能用）。区别在于：

- **快**：没有磁头、没有 NAND，读写走内存；
- **电一断，内存就没了**——所以它必须持续把内容**提交（checkpoint）**到磁盘上的 bundle 目录，
  下次开机再从最近一次完整提交恢复。

## 2. 能信到什么程度（**这是这份文档最重要的一节**）

**承诺：崩溃或断电最多丢失"一个检查点间隔"的写入。**

默认值（`disk create` 不指定任何策略时）：

| 策略 | 默认 | 含义 |
|---|---|---|
| 间隔检查点 | **60 秒** | 每 60 秒把自上次提交以来的写入提交一次 |
| 脏数据上限 | **64 MiB** | 未提交的写入超过 64 MiB 就**立刻**提交，不等满 60 秒 |
| 内存压力 | 开 | 物理内存吃紧时提前提交 |
| 休眠前 | 尽力而为 | 系统进入睡眠前做一次提交 |
| 正常停止 | 必须提交 | `disk stop` 会先提交再拆设备；提交不了就拒绝停止 |

所以"最多丢一个间隔"有两种形态，取决于你先撞上哪一个：**最多 60 秒**的写入，
或者**最多 64 MiB** 的写入，谁先到算谁。

**这不等于"数据永不丢"**，请照这个前提用它：

- 掉电时**最后一次提交之后**写的东西会丢——实测过：断电前最后一次提交之后写的文件，
  恢复后确实不存在（`docs/validation/m5-t1-09-power-loss.md`）；
- 提交里的数据**逐字节一致**——同一个读数里，断电前写入的标记文件恢复后完全一致；
- 恢复后的 generation 只会**不早于**最后提交的那一代（断电瞬间的脏写会被重放并提交成下一代）。

**要更小的窗口**：`--checkpoint-interval 10s`、`--dirty-threshold 16MiB`（更频繁 = 更慢的写、
更多磁盘占用）。**要更快的写**：放宽它们，代价是丢得更多。这是策略，由配置的人承担后果。

## 3. 命令

服务是 `MyRamDiskService`（SCM 名），客户端是 `myrd-cli.exe`。所有命令只与服务对话，
客户端不碰设备、不碰文件。

```text
myrd-cli disk list                                   # 所有盘：状态、generation、内存占用
myrd-cli disk show <disk>                            # 一块盘的细节（含恢复证明字段）
myrd-cli disk create --name <name> --bundle <dir> --size <size> --mount <target>
myrd-cli disk import --name <name> --bundle <dir> --mount <target> [--owner <sid>]
myrd-cli disk start <disk>
myrd-cli disk stop <disk> [--force] [--accept-volatile-data-loss]
myrd-cli disk checkpoint <disk>                      # 立刻提交一次
myrd-cli disk compact <disk>                         # 压缩 bundle 里的 journal
myrd-cli disk config set auto_start <true|false> <disk>
myrd-cli disk forget <profile-id> --confirmation <uuid>
myrd-cli disk repair-profile <profile-id> [--from-previous]
myrd-cli disk rebind <profile-id> --bundle <dir> --expected-image <uuid>
myrd-cli operation show|wait|cancel <operation-id>
```

常用选项：`--wait`（等操作结束）、`--no-wait`（只等准入）、`--dry-run`、`--timeout <秒>`、
`--pipe <名字>`（诊断用）。

**退出码**：`0` 成功、`1` 产品失败、`2` 用法/schema、`3` 服务不可用、`4` 权限、`5` 超时或结果不确定。

**`disk list` 一行怎么读**：

```text
d75ddf8b-…  T:  ready  running  runtime=Running  activity=Idle
            persistence=Dirty  health=Healthy  generation=4  mem=1048576/1073741824
```

`runtime` 是它现在在跑还是停了；`persistence` 告诉你**内存里的东西是否都已提交**
（`Clean` 已提交 / `Dirty` 有未提交）；`generation` 是提交代数（恢复后只会不早于它）；
`mem` 是当前驻留内存/预算。

## 4. 断电之后会发生什么（不需要你做任何事）

1. 机器开机，SCM 按 `start= auto` 拉起服务；
2. 服务启动时先把"上次记着在运行、但它的 worker 已经不在了"的盘**接收过来**：
   退休那条陈旧状态，再对 `auto_start=true` 的盘下真正的启动；
3. 启动过程从 bundle 恢复最近一次完整提交，把掉电瞬间的脏写重放并提交成下一代；
4. 盘回到原来的盘符，`disk list` 里能看到它在跑，`generation` 不早于断电前最后一次提交。

想让某块盘**不要**在开机时自动回来：`disk config set auto_start false <disk>`。

## 5. 出问题时要看什么

- **服务的日志**：`%ProgramData%\MyRamDisk\logs\service.log`（追加写）。
  它存在的原因就是"服务没有控制台"——SCM 启动的进程 stderr 会被丢弃，所以诊断信息必须落在文件里。
- **盘的数据**：bundle 目录里是提交与 journal；`disk show` 会给恢复证明字段。
- **出事了别删 `ProgramData`**：profile 记录、操作记录都在里面，删了等于放弃诊断线索。

## 6. 目前不做的事（明确写出来，避免误解）

- **多盘同时运行不在支持范围，而且是硬拒绝**：M5 是单盘产品。已经有一块盘在运行时，
  `disk create` 和 `disk start` 都会在准入阶段被拒绝，返回稳定错误码 **`0x5609`**，
  文案写明 “M5 is a single-disk product”，CLI 退出码 `1`。拒绝发生在任何 worker、
  设备、bundle 或持久化记录产生之前，所以第二块盘**不会留下任何残留**。
  要换一块盘：先 `disk stop`（或 `disk forget`）当前那块。
  这条限制以前只是“没实现”，表现为第二块盘看起来能建、拆卸时却留下设备
  （`no volume carries the recorded identity` / `device_absent=0`）；那个行为已经关闭。
- `disk release-memory`（手动把内存还给系统）**尚未实现**。
- 快照/克隆、网络共享、压缩都不在这条产品线的范围内。
