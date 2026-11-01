# R-17a：卷簇大小（create-only）

发布决策 6：**簇大小通过 `disk create --cluster-size` 提供，只作用于 create，不进 profile
记录**。理由是簇大小是“格式化”的属性：卷只格式化一次，重启/恢复是 adopt 已有卷，没有需要
重新施加或迁移的东西。

## 1. 语义与接受集合

| 项 | 值 |
|---|---|
| 选项 | `disk create --cluster-size <size>` |
| 写法 | `64KiB` / `8192`（裸数字按字节） |
| 接受集合 | 4096、8192、16384、32768、65536（2 的幂，4 KiB–64 KiB） |
| 缺省 | 不传 = Windows 默认（4 KiB） |
| 非法值 | exit 2，`--cluster-size must be a power of two between 4KiB and 64KiB` |
| 记录 | **不写入** profile record；只在 create 的 operation intent 里 |

四道闸门，任一都拒绝非法值：CLI（`cli_parser.cpp`）→ wire 解码（`ipc_codec.cpp` 字段 13）→
worker 启动前的 helper 参数检查（`windows_volume.cpp`）→ provisioning 脚本自身
（`provision_volume.ps1`，真正调用 `Format-Volume` 的进程）。

## 2. 值实际走的路径

```
CLI --cluster-size
  → control::DiskCreateCommand::cluster_size_bytes        (wire 字段 13，可选)
  → LifecycleOperationIntent::cluster_size_bytes          (只在 create 的 intent 里)
  → WorkerLaunchRequest::cluster_size_bytes               (intent 版 + bind_worker 的 record 版)
  → WorkerLaunchSpec::cluster_size_bytes                  (私有管道 spec，追加字段)
  → WorkerRuntimeConfig::cluster_size_bytes
  → DiskRuntimeOptions::cluster_size_bytes
  → provision_new_volume(bytes, letter, cluster_size)
  → provision_and_adopt_volume(...)
  → run_provisioning_helper → powershell -File provision_volume.ps1 ... -ClusterSize <n>
  → Format-Volume -AllocationUnitSize <n>
```

## 3. 调试过程中发现并修掉的四个缺陷

这一条看起来只是“加个参数”，实际暴露了四处真实的断路，全部已修：

1. **durable intent 不往返**：`write_lifecycle_intent` 没写 `volume_label` /
   `compaction_checkpoint_threshold` / `cluster_size_bytes`，执行时“内存 intent vs 持久 intent”
   比较失败（`code=22022`，`the in-memory lifecycle plan does not match durable intent`）。
   现在三个字段都追加写入，读取端用“是否还有剩余字节”做版本兼容。
2. **嵌入的 provisioning 脚本不随源文件重建**：CMake 只跟踪 `.rc`，改了 `.ps1` 后 worker 里还是
   旧脚本，helper 报 `NamedParameterNotFound`。现在 `src/CMakeLists.txt` 给
   `run/run_resources.rc` 声明了 `OBJECT_DEPENDS .../provision_volume.ps1`。
3. **cluster size 到不了 provisioning**：create 先注册 profile，再绑 worker；绑定用的是
   record 版 launch request，而 record 里没有簇大小。现在 `bind_worker` 在 record 版请求上补
   `request.lifecycle_intent->cluster_size_bytes`。
4. **worker 没有把 spec 值交给 runtime**：`m5_worker_main.cpp` 少了
   `config.cluster_size_bytes = spec.value().cluster_size_bytes;`，值到 worker 就停了
   （实测：spec 8192，卷仍是 4096）。已补。

另外两个测试自身的问题也一并修了：

- `Format-Volume` 的参数名是 **`-AllocationUnitSize`**，不是 `-ClusterSize`（写错时 helper
  抛 `NamedParameterNotFound`）；
- 三个 e2e 的 `Wait-ForTerminal` 用 `': (succeeded|completed|failed|cancelled)'` 匹配，会把
  `disposition: completed` 当成成功状态；现在只匹配 `^operation <id>: <state>`。

## 4. 客机读数（`integration.m5.cluster_size` 1/1，83 s）

```text
READING: mount=T:
READING: invalid.exit=2
READING: invalid.output=myrd-cli: --cluster-size must be a power of two between 4KiB and 64KiB
READING: create.state=succeeded
READING: volume.filesystem=NTFS
READING: volume.allocation_unit_bytes=65536
READING: volume.cluster_size_match=yes
READING: cleanup_forget=0
PASS integration.m5.cluster_size: the create formats with the requested cluster size
```

同一用例用 8 KiB 也验证过（`volume.allocation_unit_bytes=8192`），证明不是“恰好等于默认值”。

另有独立探针：在客机上对 64 MiB 的临时 VHD 直接
`Format-Volume -Partition $p -FileSystem NTFS -AllocationUnitSize 8192`，读回 `au=8192`，
确认 Windows 侧参数行为本身正常。

## 5. 回归

provisioning 脚本改动影响**所有** create，因此修复后重跑了三支烟测（各 1/1）：

- `integration.m5.performance`（console create/start/checkpoint/stop，容量 64 MiB）；
- `integration.m5.install_uninstall`（安装 + 服务 + CLI + ACL 读回）；
- `integration.m5.cli_e2e`（完整 CLI 契约，含 `label.exit=2`）。

局部单测：`m5.cli`（含 `--cluster-size` 的正反例）、`unit.m4.provision_script`（参数个数/类型/
默认值，`Format-Volume` 的参数名由脚本自身校验）。
