# M1 Shared Memory Proxy 产品化验收

日期：2026-08-03

结论：**M1 GO（限本机、限已批准的 M1 范围）**。

本结论确认 M1-A、M1-B、M1-C 均已完成：本项目不加载 AIM Toolkit 用户态组件，直接使用已安装并运行的 `phdskmnt.sys`，可以创建一个 64 MiB Shared Memory Proxy 磁盘，并通过 Windows 原始磁盘路径完成真实 READ、WRITE、ZERO、UNMAP、flush、精确删除和资源回收。

这不是生产就绪结论，也不表示已经实现动态内存、磁盘镜像、加载/保存、定时写回、断电恢复、服务或 GUI。

## 1. 被验收对象与环境

| 项目 | 新鲜证据 |
|---|---|
| 分支 | `worktree-feature+m1-proxy-productization` |
| 已测试源码提交 | `3eac4f7`（包含 M1 完整实现及最终状态证据修复） |
| M0 闭环基线 | `0e2b3b3` |
| AIM 固定上游 | `d379caa027d21902e45fce8c9daf02fd116c2dda` |
| Windows | Windows 11 Pro x64，10.0.26200 |
| CPU | AMD Ryzen 5 5600，6 核 12 线程 |
| 内存 | 64 GiB |
| MSVC | 19.44.35228 |
| CMake / Ninja | 4.2.2 / 1.13.2 |
| 内核驱动服务 | `phdskmnt`，Running |
| 驱动文件版本 | 1.2.28.93 |
| 驱动 SHA-256 | `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9` |
| Authenticode | WinVerifyTrust 状态 `0x0`；本机报告未取得 signer subject 文本 |
| API / sub-version | 257 / 16915549 |
| 适配器接口 | `\\?\root#scsiadapter#0000#{2accfe60-c130-11d2-b082-00a0c91efb8b}` |

## 2. 最终验证命令与结果

```powershell
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V6 -Configuration Debug
& .\tools\Invoke-M0Verification.ps1 -Tier V4

. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --build --preset release
ctest --preset release --output-on-failure
```

| 门 | 结果 |
|---|---|
| M1-V0 | 固定上游、M0 ancestor、文档和 diff 语法检查 PASS |
| M1-V1 | 14/14 个 `unit.m1.*` 测试目标 PASS |
| M1-V2 | 固定 seed 的 10,000-case fuzz PASS；本地 M1 SHM 恰好 50/50 PASS |
| M1-V3 | Release import audit PASS；M0 只读 query-driver PASS |
| M1-V4 | Harness preflight 8/8 PASS；单次真实 Debug 生命周期 PASS |
| M1-V5 | 8 个 Debug 故障注入全部通过清理验证；其后正常生命周期 PASS |
| M1-V6 | 同一个 Release executable 完成 20/20 次真实生命周期 |
| 全量 Debug | 26/26 CTest 目标 PASS |
| 全量 Release | 26/26 CTest 目标 PASS |
| M0 回归 | V0-V4 完整 PASS，真实 M0 create/read/write/remove 未回归 |

V5 覆盖的故障点为 `provider_ready`、`create_returned`、`device_confirmed`、`physical_drive_confirmed`、`write_verified`、`zero_verified`、`unmap_verified`、`device_removed`。每次均验证精确 DeviceId 删除、provider 停止、对象释放和基线恢复；所有故障后又运行一次正常生命周期。

## 3. 不可变实机证据

最终聚合报告：

`reports/m1-v6/summary-20260803-045952-6447101.json`

- 报告 SHA-256：`9BBFC9744DC64804AEB87715700155EEF90C71715BA8CA8F703C2CA05548F7E3`
- Release executable SHA-256：`EB473D4EB8904880A104F2233EB7FE3990F495447B7C903E356F6A2D43D8645A`
- attempted / passed：20 / 20
- `deviosvc` 原始状态 / StartMode：Stopped / Manual
- 测试结束后状态恢复：true
- StartMode 未改变：true

代表性 child report：

`reports/m1-v6/run-20260803-045952-6447101-20.json`

- 报告 SHA-256：`70D246E76B40AEA7D634D0113EE6A641D2ACC8D5C77EB665E6081EB781D91CAC`
- 创建 DeviceId：0；确认路径为新出现的 `\\.\PhysicalDrive1`
- create flags：13072（`0x3310`）
- 磁盘长度：67,108,864 bytes；sector：512 bytes
- provider 最终状态：`closed`
- 请求计数：INFO=1、READ=4、WRITE=2、ZERO=1、UNMAP=1、CLOSE=1
- INFO/READ/WRITE/ZERO/UNMAP/fatal error counters：全部为 0
- 原始 I/O offset / length：8,388,608 / 1,048,576 bytes
- 写入与读回 SHA-256：均为 `9BC4499B65A13879CD776FD9391BC5D2D8C8707361F82893423CB2943424AA22`
- ZERO 后全零读回：true
- UNMAP 后全零读回：true
- flush：true
- AIM DeviceId 基线：`[] -> []`
- PhysicalDrive 基线：`[0] -> [0]`
- 精确删除、设备消失、provider stop、命名对象释放：全部为 true
- 禁止模块和禁止进程列表：均为空

Child report 在子进程退出前生成，因此其中 `deviosvc_restored` 字段不可能证明 harness 的退出后恢复；服务恢复以聚合报告的 `service_state_restored=true` 和 `service_start_mode_unchanged=true` 为准。Harness 会在恢复前后重新校验每个 child report 的 SHA-256，且不重写 child report。

M0 回归聚合报告：`reports/m0-v4/summary-20260803-045409-4902351.json`，SHA-256 为 `3904E53CD7FBD14E04A7DFD4EE8D4479672D74211974F62D3B24C8637952132E`，1/1 PASS，服务状态和 StartMode 均恢复。

## 4. M1-A / M1-B / M1-C 验收映射

| 范围 | 已验证内容 |
|---|---|
| M1-A 协议层 | INFO/READ/WRITE/UNMAP/ZERO/CLOSE ABI；严格范围校验；稳定 Win32 `errorno`；request/fatal 分类；无后端越界调用；10,000-case 确定性 fuzz |
| M1-B 本地 SHM | 随机 128-bit Session ID；Global/Local namespace policy；窄 DACL；Cancel Event；Ready barrier；请求/取消双等待；有界诊断；线程安全计数；停止、重复停止和命名对象释放 |
| M1-C 真实驱动 | 直接 SRB 控制面；精确返回 DeviceId；新 PhysicalDrive 身份确认；真实 READ/WRITE/ZERO/UNMAP；原始 flush；精确删除；8 点故障注入；Release 20 次循环 |

TRIM 使用 `DEVICE_MANAGE_DATA_SET_ATTRIBUTES` 并设置 `DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED`。本机磁盘类缓存会在真实 UNMAP 后暂时返回旧缓存，因此成功 TRIM/flush 后对同一 owned disk 调用 `IOCTL_DISK_UPDATE_PROPERTIES`，再进行全零读回。该步骤不会扩大设备操作范围，也不会替代真实 UNMAP 计数证据。

M1 provider 会公布 ZERO/UNMAP 能力；冻结的 M0 provider 通过兼容包装继续返回旧 INFO flags=0，从而保持 M0 `QUERY_DEVICE` flags=`0x310`。M1 的对应 flags 为 `0x314`。M0 V4 实机回归证明该分离有效。

## 5. 安全边界与未实现范围

- 只删除本次 CREATE 返回且经过 QUERY_DEVICE 确认的 DeviceId；拒绝 auto/all-device sentinel。
- Harness 只在受控测试窗口停止 `deviosvc`，并在 `finally` 恢复原状态；不修改 StartMode。
- Release import audit 禁止 `aimapi.dll`、`devio.dll`、`aim_ll`、RamDyn、AIM GUI 和已知 Toolkit 用户态程序；运行时 loaded-module/process 证据同样为空。
- 没有卸载 AIM Toolkit，没有安装、替换或重新签名驱动，也没有形成独立驱动分发方案。
- M1 使用一次性分配的固定 64 MiB `MemoryBlockDevice`；不是按块动态提交内存。
- 没有 backing image、镜像加载/保存、VHD、dirty tracking、定时写回、journal、checkpoint、崩溃恢复或断电保护。
- 没有文件系统格式化/挂载产品流程、后台服务、产品 CLI 或 GUI。
- 移除设备或终止 provider 后，数据丢失是当前设计的预期行为。

## 6. 最终判定

所有 M1 强制门均有新鲜的测试或不可变实机报告支撑，未发现设备、PhysicalDrive、命名对象、线程、句柄或服务状态残留，也未通过引入 M2-M4 功能来绕过 M1 门槛。

因此判定：**M1 GO**。

该 GO 只表示可以在此 M1 基线上开始后续 M2-M4 开发；不授权卸载 AIM Toolkit，不代表可发布给最终用户。
