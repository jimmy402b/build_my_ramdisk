# T1-07 内存可见性与 UNMAP 端到端

**结论**：`resident_bytes` / `budget_bytes` 已从存储层的自有计数器贯穿到 CLI；删除文件后常驻内存
实测下降——**UNMAP 链路的首次端到端验证**。

**客机**：`myrd-vm`，Release，硬重启 + 清空 `C:\ProgramData\MyRamDisk\{profiles,operations}`。

---

## 1. 贯穿的每一层（§7.3 的要求）

| 层 | 落点 | 数值来源 |
|---|---|---|
| storage | `DynamicBlockStoreStats::committed_bytes`（已有） | 块真正提交进 arena 时 +`kBlockSizeBytes`（`dynamic_block_store_write.cpp:929` 等） |
| persistence/M3 | `M3Provider::memory_budget_bytes()`（新增） | `store_->options().hard_limit_bytes`；store 关掉后回落到 `config_.store.hard_limit_bytes` |
| run | `DiskMaintenanceStats::resident_bytes` / `budget_bytes`（新增） | adapter 从上面两者取 |
| host | `WorkerRuntimeStatus::resident_bytes` / `budget_bytes`（新增，走私有协议 codec） | `DiskWorkerRuntime::observe()` 从 runtime 的维护统计取 |
| management | `WorkerRuntimeObservation` → `ServiceProfileProjection`（各加两个字段） | `observe_worker_runtime()` 映射；只有 `worker_alive` 时才带值 |
| CLI | `disk list` / `disk show` 行尾 `mem=<resident>/<budget>` | 直读投影 |

**为什么不是"进程工作集"**：工作集把映射文件、DLL、JIT 页全算进来，用户问的"这块盘占多少内存"
只有存储自己的计数器答得准。§7.3 也点名这是最容易在中间断掉的一类事实，所以每一层都钉了用例：

- `m5_worker_protocol`：状态里两个 u64 过线后仍相等，且**截断被报成 malformed**（而不是读成两个 0）；
- `m5_observe_worker_runtime_carries_the_memory_gauges_only_while_alive`：runtime 倒下后必须报 0，
  不能把最后一次读数当成"仍在占用"；
- `cli_wire_round_trips_a_list_response`：两个值过客户端线格式不变；
- `cli_renders_resident_memory_against_its_budget`：行里确实出现 `mem=…/…`。

## 2. UNMAP 端到端（第一次真跑）

`integration.m5.cli_e2e` 新增一步（写 16 MiB → 读 → 删 → 再读）：

| 时点 | resident | 说明 |
|---|---|---|
| 写文件前 | **2 949 120** | 系统与元数据已提交的块 |
| 写完 16 MiB | **19 660 800** | 恰好 +16 MiB |
| 删除后 | **2 949 120** | 回到写前同一数值 |
| budget | 1 073 741 824 | `--ram-limit` 默认 1 GiB |
| worker 工作集 | 22 573 056 | 与 resident 同量级（§7 的"与任务管理器同量级"） |

**第一次尝试失败过，值得记**：普通写入后 resident **完全没有变化**（2949120 → 2949120），
因为数据还在 Windows 文件缓存里。改成**写穿**（`FileOptions::WriteThrough` + `Flush(true)`）
并对"上升"做有界轮询（30 s）后才得到上面的读数。这条不是测试技巧问题：
它说明"写文件后立刻读内存"在 Windows 上量到的是缓存，不是存储。

**链路归属**：NTFS 删除 → 卷 UNMAP（`kFlagSupportsUnmap` + `kReqUnmap`）→
`request_processor` → `device_.unmap` → 存储 `Discard`（`JournalRecordType::Discard`）。
本次读数证明**整条链路是通的**，而不只是"机制已存在"。

## 3. 读数（请求 ID）

| 用途 | 请求 |
|---|---|
| 第一次单次（缓存干扰，resident 不动） | `433c1d703ebf4b5db5a2c4f81e03bdaf` |
| 写穿后单次通过（上面那张表） | `3859752bc6d44ee6899702ad7543dd64` |
| 验收 `-Repeat 20` | `541c8b4a36bc43698acea0211214fea0`：**20/20**，7 分 10 秒，干净客机 |

## 4. 未做 / 延后

- **`disk release-memory`（§7.4 手动释放）没有实现**。它需要一条完整纵切
  （storage 公开 `reclaim_to` → M3 → M4 runtime → worker 命令 → 服务动作/计划 → CLI 动词），
  而 T1-07 的验收面（可见性 + UNMAP）不含它。记录在 tasklist 的 T1-08 行里，与"内存压力"一起做，
  因为它与 `EvictionManager::reclaim_to` 是同一个消费者。
- 与任务管理器"同量级"只作为读数并列，不写成断言：两个数计的东西不同（store 已提交的 arena
  与 worker 进程的整个工作集），钉住比值会把平台观测变成项目不控制的契约。
