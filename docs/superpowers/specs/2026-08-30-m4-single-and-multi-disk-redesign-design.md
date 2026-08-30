# MyRamDisk M4 单盘核心与多盘隔离重设计

状态：待用户审阅，尚未实施

日期：2026-08-30

M3 基线：GitHub `jimmy402b/build_my_ramdisk` 的 `main` 当前提交
`df09c4f0ac310b00e5b53be594c4bfba6730aaec`。该公开仓库 README 将 M3
记录为已完成最终验收，证据冻结提交为 `4ece675`。

## 1. 设计决策

M4 重新从已验收的 M3 持久化核心开始，不继续当前复杂的 M4 qualification
控制器，也不继承 Task23 的 Windows Service/worker/IPC 管理平台作为运行前置
条件。

M4 的最小正确性单元是一个 `DiskRuntime`：

```text
一个 myrd.exe 进程
  -> 一个 M3 provider
  -> 一个 AIM 设备
  -> 一个 Windows 卷
  -> 一个 bundle 与 checkpoint 序列
```

多盘由多个相互独立的 `myrd run` 实例组成。每个实例拥有独立 bundle、盘符、
AIM 设备、writer lock、内存预算和 checkpoint generation。M4 不要求一个中心
服务统一管理所有磁盘；统一服务、命名管道和 worker 监督保留给后续阶段。

## 2. 目标

M4 必须实现：

1. 通过命令行创建一个新的 AIM-backed NTFS RAM disk。
2. 通过同一命令恢复已有 bundle。
3. 运行期间按时间或 dirty 数据量自动执行 checkpoint。
4. checkpoint 前对实际 Windows 卷执行 `FlushFileBuffers`。
5. 异常进程终止、Windows 重启或 VMware 人工断电后，恢复到最后一次成功提交的 checkpoint。
6. checkpoint 之后尚未提交的内容可以丢失。
7. 已有 bundle 恢复时不得重新分区、格式化或创建新的空文件系统。
8. Ctrl+C 按确定顺序完成 flush、最终 checkpoint、卸载卷、删除盘符和停止 AIM runtime。
9. 支持两个或更多独立 `myrd run` 实例同时工作，且一个实例失败不影响其他实例。
10. 对 bundle 的误修改进行检测并拒绝静默恢复；不额外引入签名或专用账户体系。

## 3. 非目标

本阶段不实现：

- Windows Service、LocalSystem worker 或命名管道控制平面；
- 多盘集中式 profile 数据库、自动重连和全局调度器；
- 自动安装、替换或卸载 `phdskmnt`；
- GUI、托盘程序和远程控制；
- 在线 compaction；
- 复杂的九负例 qualification kit 和三阶段发布认证；
- 以 `PhysicalDrive` 编号、PnP 实例号或显示名称作为恢复权威；
- 防御拥有管理员权限用户的恶意篡改。

`myrd compact --bundle <path>` 可以保留为离线维护命令，但不参与在线运行时
的自动调度。

## 4. 持久化与恢复语义

M4 完全继承 M3 的 durable-view 语义：

- 普通文件写入先进入 RAM，不立即产生持久化承诺；
- 只有成功发布的 manifest checkpoint 才被视为可恢复；
- 恢复只能选择一个完整旧 generation 或完整新 generation；
- 不允许把两个 generation 的块混合；
- manifest、journal、index 或 VHDX 损坏时必须失败关闭；
- 不允许把损坏块静默替换为零、旧数据或新空盘；
- ZERO/DISCARD 后不得从旧 backing 中恢复被清除的数据；
- manifest publish 已开始但结果无法确认时，必须保留 M3 的
  `CommitIndeterminate`/reconciliation 语义，不得立即当作普通失败重试；
- checkpoint 后的新写入在下一次成功 checkpoint 前均允许丢失。

用户可以依赖的唯一恢复承诺是：

```text
恢复结果 = 最后一次成功提交 checkpoint 的完整逻辑磁盘状态
```

## 5. Bundle 完整性与误修改边界

checkpoint 不是 `txt` 文件。真正的持久化数据包括：

```text
disk.rdm                 双槽 manifest
base-*.vhdx              基础 VHDX
journal-*.rdj            增量 journal
index/root               已提交 journal 索引
```

M4 沿用 M3 已有的固定布局、CRC、generation、journal digest、文件 identity、
manifest 选择和 committed-length 校验。启动恢复前必须重新验证这些内容。

验收中的 `A.txt`、`D.txt` 仅是写入 RAM 盘的测试数据，用来比较恢复内容，
不参与 manifest 选择，也不是 checkpoint 元数据。

本阶段不新增签名 seal、DPAPI 密钥或专用 ACL。假设普通管理员不会手工编辑
bundle；如果 bundle 被误删、截断或修改，系统必须检测并报错，而不是静默接受。

## 6. 命令行契约

```text
myrd --help

myrd run --bundle <path>
         --size <64M|128M|...>
         --drive-letter <R:>
         [--checkpoint-seconds <N>]
         [--checkpoint-dirty-mb <N>]
         [--ram-limit <N>]

myrd compact --bundle <path>
```

约束：

- `run` 每个进程只接受一个 bundle 和一个盘符；
- bundle 路径必须是本机 NTFS 路径，不得是设备路径或 RAM 盘路径；
- 新 bundle 创建 GPT、单分区和 NTFS；
- 已有完整 bundle 只恢复，不重新格式化；
- 请求大小必须与已恢复 manifest geometry 完全一致；
- 盘符已占用、bundle 非空但格式未知、驱动不可用或 bundle 损坏时返回失败；
- 正常输出只包含人类可读的进度与 generation 信息；错误输出到 stderr；
- 退出码 `0` 表示成功，`1` 表示运行/恢复/持久化/清理失败，`2` 表示命令行错误。

默认策略：

```text
checkpoint-seconds = 60
checkpoint-dirty-mb = 64
ram-limit = 2G
```

## 7. 单盘运行生命周期

### 7.1 创建

1. 检查提权、驱动可用性、bundle 路径和目标盘符。
2. 分类 bundle；只有不存在或为空的目录允许 Create。
3. 创建 M3 generation-zero bundle。
4. 创建 AIM 设备并启动 M3 provider。
5. 通过受控 provisioning 逻辑创建 GPT/单分区/NTFS。
6. 验证唯一目标卷和盘符归属。
7. flush Windows 卷。
8. 提交 generation 1 checkpoint。
9. 输出 `Ready: R: generation 1.`。

### 7.2 恢复

1. 检查 bundle 完整性和 requested size。
2. 由 M3 recovery engine 选择最后一个完整 generation。
3. 创建与该 generation 对应的 AIM runtime。
4. 只发现并重新挂载已有卷，不调用格式化或分区命令。
5. 验证卷 geometry、AIM 归属和盘符。
6. 输出恢复后的 generation 和 Ready 状态。

### 7.3 自动 checkpoint

每一秒检查一次维护状态。当满足任一条件时执行：

- 距离上次成功 checkpoint 达到 `checkpoint-seconds`；或
- dirty bytes 达到 `checkpoint-dirty-mb`。

执行顺序固定为：

```text
锁定/验证目标卷
  -> FlushFileBuffers
  -> M3 consistent snapshot
  -> journal prepare + flush + readback
  -> manifest publish + flush
  -> backing view/dirty acknowledgement
  -> 输出 committed generation
```

失败必须保留 M3 的结构化 disposition。不可确认提交不得被当作未提交处理。

### 7.4 正常关闭

Ctrl+C 只设置 shutdown 标志，主循环执行：

```text
lock volume
  -> verify exact volume
  -> FlushFileBuffers
  -> final checkpoint（必要时有限重试）
  -> dismount
  -> remove owned drive letter
  -> stop M3/AIM runtime
```

任何资源清理失败都返回非零，但不得删除用户 bundle。foreign volume、foreign
盘符或无法确认归属时必须拒绝删除。

## 8. 多盘隔离模型

M4 的多盘支持采用“多实例隔离”，而不是中心服务：

```text
myrd run --bundle E:\m4\disk-a --size 64M --drive-letter R:
myrd run --bundle E:\m4\disk-b --size 64M --drive-letter S:
```

每个实例必须满足：

- 只能写自己的 bundle；
- 只能卸载自己发现并验证过的 AIM 设备和卷；
- 只能使用自己的 writer lock 和 checkpoint generation；
- 自己的 checkpoint、journal 错误或异常退出不能改变其他实例；
- 失败清理不能按名称杀进程或删除未知设备；
- 两个实例可以并行读写、checkpoint 和正常关闭；
- 同一 bundle 的第二个实例必须被 writer lock 拒绝。

后续若需要 `myrd disk list/start/stop` 或 Windows Service，必须把本节的
`DiskRuntime` 作为不可变的单盘执行核心；管理层不得复制 M3 持久化逻辑。

## 9. 代码边界

建议新 M4 只增加以下运行层：

```text
src/run/
  cli.{h,cpp}
  disk_runtime.{h,cpp}
  run_loop.{h,cpp}
  windows_volume.{h,cpp}
  provision_volume.ps1
  main.cpp
```

职责：

- `cli`：纯参数解析和默认值；
- `disk_runtime`：一个 bundle、一个 M3 runtime、一个卷的生命周期；
- `run_loop`：定时 checkpoint、journal 容量和 shutdown policy；
- `windows_volume`：卷发现、provision、flush、lock、dismount、盘符管理；
- `main`：单一组合入口，不实现 journal、manifest 或 VHDX 格式。

严禁：

- 在 M4 复制 M3 checkpoint/recovery 算法；
- 让 CLI 直接改写 bundle 文件；
- 通过 PhysicalDrive/PnP 显示信息替代 M3/AIM 权威发现；
- 为多盘引入共享可变全局 checkpoint 状态；
- 在失败清理时使用宽泛的按名称 kill 或设备删除。

## 10. 本地验证标准

实现完成前必须在干净工作树执行：

```powershell
cmake --preset msvc
cmake --build --preset debug --parallel 1
cmake --build --preset release --parallel 1
ctest --preset debug --output-on-failure
ctest --preset release --output-on-failure
git diff --check
```

M3 全部正式标签必须保持通过。M4 至少需要：

- CLI 正常/错误输入测试；
- bundle 分类和 zero-write 测试；
- checkpoint 调度测试；
- commit disposition 测试；
- Windows volume identity/provisioning 测试；
- 单盘 Ctrl+C 清理测试；
- 同 bundle writer lock 冲突测试；
- 双实例独立故障和独立恢复测试。

## 11. VM 验收环境

正式 VM 验收使用固定的 Windows 11 x64 虚拟机：

- 已安装并可用的签名 `phdskmnt.sys` 和 AIM；
- Administrator PowerShell；
- bundle 位于持久化本机 NTFS 卷；
- 至少两个空闲盘符，例如 `R:` 和 `S:`；
- 验收前无目标 bundle、myrd 进程、AIM 设备和目标盘符；
- 记录 VM UUID、MAC、驱动版本、AIM 版本和 Windows build；
- VMware Reset 或 Power Off→Power On 只能由用户人工执行；
- 不使用 SSH、`vmrun`、脚本 shutdown 或自动设备删除。

验收报告必须记录每次命令、退出码、stdout/stderr、generation、测试文件
SHA-256、bundle 清单和最终残留状态。

## 12. VM 验收场景

### VM-1：单盘创建

运行：

```powershell
myrd run --bundle E:\m4\disk-a --size 64M --drive-letter R:
```

必须观察到：

- `Ready: R: generation 1.`；
- R: 是目标 AIM 设备上的 GPT/NTFS 卷；
- 写入 `R:\A.txt` 后能够读回；
- bundle 包含合法 `disk.rdm`、VHDX 和 journal；
- 没有第二个 AIM 设备或未知盘符。

### VM-2：checkpoint 后异常进程终止与恢复

1. 写入 `A.txt`；
2. 等待明确的 `Checkpoint committed generation N.`；
3. 记录 `A.txt` SHA-256；
4. 再写入 `after-checkpoint.txt`；
5. 强制终止该 `myrd.exe`；
6. 人工执行一次 VMware Reset 或 Power Off→Power On；
7. 使用完全相同的 `run` 命令恢复。

必须满足：

- `A.txt` 存在且 hash 与 checkpoint 前一致；
- `after-checkpoint.txt` 可以存在，也可以丢失；
- 恢复 generation 不小于已记录的成功 generation，且不出现混合代次；
- 恢复路径不格式化、不重新分区；
- R: 只绑定到本次恢复的 AIM 设备。

### VM-3：双盘并行与隔离

同时启动 disk-a/R: 和 disk-b/S:。分别写入不同内容并等待各自 checkpoint。

必须满足：

- R: 与 S: 分别对应各自 AIM 设备和 bundle；
- A 的 generation、journal 和错误不改变 B；
- 停止 A 不卸载 S:；
- 同一个 bundle 启动第二个实例被 writer lock 拒绝；
- 两个 bundle 的树清单互不引用。

### VM-4：双盘断电恢复

1. A、B 各完成至少一个成功 checkpoint；
2. 分别写入 checkpoint 后的新内容；
3. 人工执行一次 VMware Reset；
4. 分别使用原始命令恢复 A、B。

必须满足：

- A 恢复到 A 的最后成功 checkpoint；
- B 恢复到 B 的最后成功 checkpoint；
- A 的恢复失败不会删除、卸载或修改 B 的资源；
- 最终两个盘符和 AIM 设备均能准确归属并清理。

### VM-5：负面与完整性

逐项验证：

- 盘符已占用；
- bundle 非空但无 manifest；
- manifest 损坏；
- journal 尾部截断或 CRC 错误；
- requested size 不匹配；
- 同 bundle writer lock 已占用；
- checkpoint flush 失败（可用测试 seam）；
- 驱动或 AIM 不可用。

每项都必须返回非零、不挂载错误卷、不格式化已有 bundle、不删除用户数据，
并且不影响其他正常运行的磁盘实例。

## 13. 通过/失败分类

只有以下条件全部满足，才称为“M4 VM 验收通过”：

1. Debug/Release 构建和 M3 回归全部通过；
2. M4 本地单元/集成测试全部通过；
3. VM-1 至 VM-5 全部通过；
4. 至少一次真实 checkpoint 后的进程异常恢复通过；
5. 至少一次人工 VMware Reset/断电恢复通过；
6. 至少两个盘并行、隔离和恢复通过；
7. 所有恢复内容符合最后成功 checkpoint 语义；
8. 无错误格式化、跨盘清理、未知设备删除或残留进程。

失败必须归类为：

```text
PRODUCT_FAIL       环境满足，但产品行为违反本规范
ENVIRONMENT_BLOCKED VM、AIM、驱动、权限或盘符前提不满足
HARNESS_FAIL        验收脚本/测试工具自身错误
USER_ABORTED        人工电源或操作步骤不符合规定
```

不得通过放宽断言、重复使用旧 bundle/root 或更换验收环境来把失败改写为
通过。

## 14. 后续演进

M4 完成后，若确实需要集中式多盘管理，下一阶段可以增加：

1. `DiskSupervisor`，只管理多个 `DiskRuntime` 实例；
2. `myrd disk list/start/stop/checkpoint`；
3. 每盘 profile 和操作记录；
4. 命名管道和权限验证；
5. 服务化和 worker 监督。

这些功能必须复用已经通过 VM 验收的单盘运行核心，不能重新定义 checkpoint
或 recovery 语义。
