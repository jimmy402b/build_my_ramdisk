# STAB-10 / R-12：性能基线 —— **PASSED（64 MiB 口径）**

**客机**：`myrd-vm`，Windows 11 x64 26200，Release；每次运行前 `Reset-M5Guest`。
**用例**：`integration.m5.performance`（1/1，**64 MiB 逻辑盘、写 32 MiB**）。

## 1. 读数（`integration.m5.performance`，请求 `d57163e8…`）

| 项 | 读数 | 说明 |
|---|---|---|
| create（准入→终态） | **107 ms** | 64 MiB 逻辑盘 |
| start（准入→终态） | **1453 ms** | 含 worker 启动、AIM 设备、NTFS 卷挂载 |
| worker working set | **20.6 MiB** | `myrd-m5-worker` 单进程 |
| 写吞吐（32 MiB，4 MiB 缓冲，flush） | **40 ms / 802 MiB/s** | 通过 `T:` 卷写文件 |
| checkpoint（32 MiB dirty，flush+commit） | **215 ms** | 操作终态成功 |
| stop（准入→终态） | **1130 ms** | 含 dismount/设备退役 |
| forget | exit 0 | 清理成功 |

判定：全部在用例的宽松上限内（create/start/stop < 60 s、checkpoint < 120 s、写 ≥ 50 MiB/s、
worker RSS < 512 MiB）。这些数字是**基线**，不是承诺；后续发布只要同一口径不出现数量级回退。

## 2. 一次 256 MiB 的失败尝试（记录，不是通过）

第一次用 `-SizeMiB 256`（写 224 MiB）跑：create 163 ms、start 869 ms、worker RSS 21.7 MiB 都正常，
但写+checkpoint 之后**客机整体卡死**（SSH 超时、`cmd` 无响应），只能硬重启；因此没有读数。
这是需要单独定性的现象（可能方向：224 MiB dirty 数据的 flush 在 8 GiB 客机上的 commit 压力、
AIM 设备队列、或 journal 写入路径）。**R-13 soak 因此使用小负载（≤64 MiB）+ 频繁 checkpoint**，
不重复这个口径；等 256 MiB 现象定性后再决定是否把它加入基线。

## 3. 口径与复现

```powershell
# guest
powershell -File <repo>\tests\e2e\test-m5-performance.ps1 -Configuration Release
```

- 逻辑盘 64 MiB、写 32 MiB（留 32 MiB 给 NTFS 元数据；写满整个逻辑盘会 "insufficient disk space"）；
- 吞吐 = 请求字节数 / 写+flush 的墙钟时间；
- checkpoint 读数是在写完 32 MiB 之后的**脏卷**上取的，因此包含 flush 的成本；
- worker RSS 取 `Get-Process myrd-m5-worker` 的 `WorkingSet64` 之和。

## 4. 状态

**PASSED**（基线已记录）。256 MiB 现象未定性，已在 §2 明确写出，不当作通过。
