# STAB-02 内存耗尽行为：预算耗尽时可预期的写失败、可完成的 checkpoint、可恢复的最后提交代

**结论**：内存预算耗尽时，需要**新提交块**的写入以 I/O 错误失败（不挂死、worker 与设备不残留）；
已提交数据不受影响；`disk checkpoint` 仍能完成并推进 generation；服务重启后 `disk start` 恢复到
最后提交代，已提交字节逐段校验一致；删除文件后 UNMAP 归还内存，写入恢复。**最多丢一个 checkpoint
间隔（默认 60 s / 64 MiB，取先）**，与 T1-10 的承诺一致。

---

## 1. 策略（这是产品选择，不是实现细节）

| 项 | 行为 |
|---|---|
| 预算来源 | profile 的 `memory.hard_limit_bytes`。`automatic` = `min(物理内存/4, 虚拟容量)`；`fixed` = `--memory-hard-limit` |
| 耗尽点 | **预算**，不是物理 RAM。automatic 模式因此把每块盘限制在物理内存的 1/4 以内，正常使用不会把机器压到换页/提交失败 |
| 需要新块的写 | 拒绝。Windows 上层看到的是 **I/O 设备错误**（`ERROR_IO_DEVICE`），不是 `ERROR_NOT_ENOUGH_MEMORY`：proxy 把 `memory_hard_limit` 映射为 `ERROR_NOT_ENOUGH_MEMORY`，但 NTFS/AIM 栈把它呈现为设备错误 |
| 只改已提交块的写 | **可能成功**（小文件、元数据更新）。实测：预算满后 4 MiB 数据块写失败，而 23 字节文本文件创建成功 |
| 已提交数据 | 不受影响：失败发生后 `persistence=Clean`、`generation` 不变 |
| checkpoint | 成功；把失败前已写入的脏数据提交成新一代（实测 generation 2→4） |
| 停止 / 重启 / 启动 | 成功；恢复到最后提交代（实测 5 ≥ 4），文件长度 256 MiB、前 4 MiB 逐字节一致 |
| 归还内存 | 删除文件 → UNMAP → `mem` 从 256 MiB 回落到约 5 MiB，写恢复 |
| 最多丢多少 | 最后一次成功提交之后的写入；即默认 60 s 或 64 MiB（取先） |

**用户看到的失败长什么样**（客机原样，中文系统）：

```text
使用“1”个参数调用“Flush”时发生异常:“由于 I/O 设备错误，无法运行此项请求。”
```

`disk show` 同时给出 `mem=<budget>/<budget>`（预算已满）——这是用户可自行核对的信号。当前**没有**
把 `hard_limit_rejections` 计数暴露到 `disk show`/服务日志，这是已知的可观测性缺口（见 §6）。

## 2. 途中发现并修掉的四个产品缺陷（**都阻塞 STAB-02，且都影响正常创建**）

STAB-02 需要一块大于 64 MiB 的盘，而 512 MiB/1 GiB 的 `disk create` 在此前**必然失败**。逐层定位：

| # | 缺陷 | 证据 | 修法 |
|---|---|---|---|
| 1 | `provision_volume.ps1` 把盘符写死给 **PartitionNumber 1**；GPT 盘上的 MSR 是分区 1，数据分区是 2，盘符挂到 MSR 上，数据卷永远没有盘符 | 客机时间线：`1:1@`（MSR）在 22:09:39 获得 `V:\`；数据卷 `1:2@\\?\Volume{…}` 始终无盘符；`select_volume_for_disk` 报 `no volume matches the requested drive letter` | 用 `New-Partition` 返回对象的 `PartitionNumber`；单测改为模拟 MSR 布局并断言盘符跟随创建的分区号 |
| 2 | `verify_volume` 的 `kPartitionOverheadAllowance` 是 2 MiB，而 MSR 让数据分区比虚拟盘小约 16 MiB | `Diagnostic verify_volume: extent_bytes=520028160 expected=536870912 … guid 相同` | 上限改为 18 MiB（MSR + GPT 对齐），注释写明测量值；真正被截断的卷仍然失败 |
| 3 | worker 的 `default_recovery_options()` 把 `max_virtual_bytes` 写死 **64 MiB**，任何更大的 bundle 在 `record_bundle_identity` 被判 `slot_semantics;status=unsupported` | `slot=0/1 status=4 failed=2 ctx=slot=A;check=slot_semantics;status=unsupported` | 用 worker 自己的 `virtual_bytes` 作为上限 |
| 4 | **内存预算从未到达 worker**：launch spec 不携带 profile 的 memory policy，worker 永远用自己的默认（1 GiB `hard_limit`），`--memory-mode fixed --memory-hard-limit` 只写进 profile 不生效 | 修复前 `disk show` 的 `mem=…/1073741824` 与 256 MiB 请求不符，320 MiB 写入 0.4 s 全部成功 | launch spec 追加 `ram_limit_bytes`（可选 tag，`unit.m5.worker_private_pipe` 覆盖 0/非 0）；`make_launch_request` 从 profile 或 create intent 物化；worker → adapter → `store_options_for` |

另修：`record_bundle_identity` 现在把 bundle 拒绝的 code/step 与两个 slot 的 `safe_context` 写进错误，
下一次同类失败不需要再改代码取证（此前只有 "the bundle holds no image this worker can verify"）。

## 3. 客机读数（`myrd-vm`，Release，`Reset-M5Guest` 后干净机）

盘：1 GiB 虚拟容量，`--memory-mode fixed --memory-hard-limit 256MiB`，checkpoint 间隔 30 m（默认
dirty 阈值 64 MiB 保持开启）。实验脚本：`tests/e2e/test-m5-memory-exhaustion.ps1 -Mode budget`。

```text
baseline:  generation=2  mem=6291456/268435456       runtime=Running persistence=Clean
write 320 MiB in 4 MiB chunks:
           written_mib=256  write_failed=yes  write_seconds=14.4
           failure: I/O 设备错误 (MethodInvocationException 0x80131501; inner ERROR_IO_DEVICE)
after:     generation=2  mem=268435456/268435456     runtime=Running persistence=Clean health=Healthy
           workers_after_failure=1
small write (23 bytes, 只改已提交块): succeeded
checkpoint: exit=0 state=succeeded
after cp:  generation=4  mem=67108864/268435456      runtime=Running persistence=Dirty
stop:      succeeded
restart:   service restarted, disk start succeeded
after restart: generation=5  mem=1245184/268435456   runtime=Running persistence=Clean
           file_length_after_restart=268435456   committed_prefix_verified_bytes=4194304 (逐字节一致)
delete:    mem=5505024/268435456   write_after_unmap=ok
cleanup:   forget=ok
PASS
```

同一实验在修复 #4 之前记录了“预算未生效”的对照读数（写入 320 MiB 成功、`mem` 显示 1 GiB），
证明修复前后的差异来自产品而不是环境。

## 4. 没有覆盖的部分（明确写出来）

- **物理 RAM 真的不够**（fixed 预算大于机器提交上限）：没有做。产品默认（automatic）把预算限制在
  物理内存的 1/4，正常路径到不了物理耗尽；故意把预算设到 6–8 GiB 需要数分钟写入 + 巨量 journal，
  本轮没有测。OS 分配失败与预算拒绝走同一个错误码（`memory_hard_limit`），但**没有客机读数**。
- **journal 增长**：一次 checkpoint 的 journal 实测 ≈ 344 MiB（256 MiB 脏集 + COW/开销），约为脏集的
  1.34 倍。worker 的恢复上限据此按“2×脏集 + 64 MiB”推导（否则重启会拒绝自己的 bundle）。
  但这说明 journal 会随脏集线性增长；长时间不 checkpoint 的盘需要 STAB-03/STAB-11 的 journal 上界读数。
- **可观测性**：`hard_limit_rejections` 计数没有出现在 `disk show` 或服务日志里。用户只能从
  `mem=budget/budget` 与 I/O 错误推断。补一个计数字段需要贯通 worker 状态 → 投影 → 线格式 → CLI，
  留作 STAB-13 的一部分。

## 5. 回归

- 本机：`unit.m4.provision_script`、`unit.m5.worker_private_pipe`、`unit.m5.service_worker_control`、
  `m5.*` 模型套件、`m4.checkpoint_operation`、`m4.recovery_commands` 等 17 个套件全绿。
- 客机：`integration.m5.cli_e2e` **1/1 通过**（含 64 MiB 创建、读写、checkpoint、stop、restart、
  export、Load Only、易失盘、自动 checkpoint、STAB-01 第二块盘拒绝与 device_absent=1）。

## 6. 发布影响

产品代码与 worker 资源均变化（`service`/`worker`），release manifest 继续失效，H1 收尾统一重新冻结。
