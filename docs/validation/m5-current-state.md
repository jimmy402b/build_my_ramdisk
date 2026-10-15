# M5 当前状态：虚拟机上的真实读数

日期：2026-10-14
基线：`feature/m5-lifecycle` @ `9e66cdf`（main = `cb3df08`）
回路：`tools/m5vm/`（T1-01），配置见 `docs/operations/m5-vm-harness.md`

---

## 1. 结论

```
144 of 146 passed          （9 分 0 秒）
```

* 剩余 2 个失败是**同一个原因：客机没有 Visual Studio**（脚本要 `vswhere.exe`）→ **环境受阻，不是产品失败**。
* 另有**两个间发问题各在 3 次全套里出现过 1 次**（见 §4）——它们需要重复运行才能刻画，单次读数不足以定性。
* 这份读数是**第一次在没有污染的机器上得到的**。开发机历史记录的 144/146 与它数值巧合但**环境不同**，不可比。

---

## 2. 环境与命令

| 项 | 值 |
|---|---|
| 客机 | VMware Workstation `Windows 11 x64`，MAC `00:0C:29:02:90:C1`，账户 `v\usr` |
| 客机 IP | **`192.168.180.129`（已固化）**，主机别名 `myrd-vm`（VMnet8 NAT + DHCP 按 MAC 保留） |
| 执行上下文 | 计划任务 `MyRamDisk-M5-Agent`：`elevated=True`、`session_id=0`、**无 NETWORK SID** |
| 客机工具链 | Git `2.53.0.windows.1`（已装，且镜像了 git 元数据）；**无 Visual Studio** |
| 构建 | 开发机，MSVC Release |
| 测试清单 | `ctest --show-only=json-v1` 枚举 **146** 项 |

```powershell
$env:M5VM_HOST = 'myrd-vm'; $env:M5VM_USER = 'usr'; $env:M5VM_KEY = "$env:USERPROFILE\.ssh\id_ed25519"
cmake --build out/build/msvc --config Release
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release
tools/m5vm/Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 180
```

`-MinTimeoutSeconds 180` 是**超时下限**：客机比 ctest 预算所假设的机器慢，几个 verifier 用例的
30/60 秒预算在客机上不够（实测：它们在 180 秒下通过，**不是挂住**）。下限只抬不降，所以
`m5.production_execution_backend` 自己要求的 300 秒不受影响。

---

## 3. 失败分类（144/146）

| 用例 | 退出码 | 证据 | 性质 |
|---|---|---|---|
| `structural.m2.storage_independent_build` | 1 | `vswhere.exe not found`（`tools/Enter-MsvcEnvironment.ps1:6`） | **环境**：客机无 Visual Studio |
| `structural.m5.execution_boundary` | 1 | 同上 | **环境**：客机无 Visual Studio |

> 第二个用例的失败信息在 staging 修复**之前**是 `Missing execution-boundary cmake module`。
> 把 `cmake/` 也镜像过去之后它才推进到"缺 VS"这一步——也就是说那个 staging 缺口是真实的，
> 修对了，只是后面还有一道环境墙。

---

## 4. 两个间发问题（各 3 次全套中出现 1 次）

| 用例 | run4 | run5 | run6 | 说明 |
|---|---|---|---|---|
| `m5.production_execution_backend` | **失败** | 通过 | 通过 | 见 §4.1 |
| `unit.m2.backing_generation` | **崩溃** | 通过 | 通过 | 见 §4.2 |

**这两个不能用单次绿来结案。** 它们的共同特征是"大多数时候过、偶尔不过"，而本项目的记录
已经证明这类问题只能靠重复运行刻画（transfer manifest §1.39 就是同一类教训）。

### 4.1 `m5.production_execution_backend`：§1.39 的签名在干净客机上复现了一次

run4 的失败信息：

```
myrd-test: stop A failed: op=service_worker_control.remove_mount
          msg=The worker refused to remove: its runtime stands dirty   (23559)
```

与 transfer manifest §1.39 记录的两次失败签名**逐字相符**。意义：

- 它不是开发机污染的产物——**干净客机上也会发生**；
- 但它也**不是必现**：run5、run6 都通过。所以 §1.39 的"间发"定性是对的，只是**环境不是原因**；
- `remove_mount` 的"拒绝拆卸脏运行时"是 manifest §7 item 21(a) 记录的**故意**行为（用户裁决），
  所以问题在**上游**：为什么一次正常 `stop` 到达拆卸点时，运行时被判为 dirty。

**下一步**：用 `-Filter '^m5\.production_execution_backend$'` 反复跑 N 次，统计复现率；
复现时把 worker 的 `stderr`（worker 拆卸失败会打印 op/msg/native/code）和 durable phase 一起取下来。

### 4.2 `unit.m2.backing_generation`：一次 `0xC0000409` 硬崩溃

run4 退出码 `-1073740791` = `0xC0000409`（`STATUS_STACK_BUFFER_OVERRUN`），**stdout/stderr 全空**。

- manifest §7 item 17 记录过这个签名的一个成因是**头文件改动后 `.obj` 陈旧导致的 ABI 错配**；
  但本次是删光 M5 目标 `.obj` 干净重编的，所以那个解释**被削弱**；
- manifest §1.31 记录过"经管道缓冲的输出在进程硬退出时丢失"，所以**空输出不等于没有失败信息**；
- run5、run6 都通过 → 至少是间发的。

**下一步**：`-Filter '^unit\.m2\.backing_generation$'` 反复跑，并把捕获写到文件而不是管道，
看崩溃前是否有部分输出。

---

## 5. harness 侧的四个修复（本轮完成）

| 问题 | 现象 | 修法 |
|---|---|---|
| 未 stage `src/` 与 `cmake/` 及根文件 | `unit.m4.provision_script`、`unit.m1.verification_script`、`structural.m5.execution_boundary` 因"找不到源文件/CMake 模块"失败 | Stage 复制 `tests`/`tools`/`src`/`cmake` + 根文件；并把**复制列表改为枚举**，否则每加一棵树都要记得改列表 |
| 未 stage git 元数据 | 装 git 后仍失败：`git rev-parse HEAD` 在镜像里查不到（worktree 的 `.git` 是指向主仓库的指针文件） | 镜像 `.git` 指针 + 主仓库 `.git`（40 MB，tar 传输）。**验证：客机 `rev-parse HEAD` 与宿主机返回同一提交** |
| 客机无 git | 5 个用例报 `git.exe is required for crash evidence provenance` | 装 Git 2.53（宿主机打包直传——winget 报 `0x8a15000f`、GitHub API 被限流，两条网络路都不通）；并在 runner 里显式前置 git 路径，因为计划任务的进程可能带着安装前的 PATH |
| 超时预算过紧 | 4 个 verifier 用例 TIMEOUT（30/60 秒） | 新增 `-MinTimeoutSeconds`（下限，不是覆盖）；实测它们在 180 秒下通过 |

顺带新增：`-Run -Filter <regex>`（只跑匹配的用例）与 `-TimeoutOverrideSeconds`（诊断用）。

---

## 6. 一个我自己造成又修掉的环境陷阱（值得记）

为了让无人值守启动不会卡在 "Press any key to boot from CD"，我把 CD-ROM 设成
`sata0:1.startConnected = "FALSE"`。结果 `unit.m4.profile_codec` 开始失败：

```cpp
const std::wstring canary = L"D:\\MYRD_PATH_CANARY_7f1b\\missing";
REQUIRE(!std::filesystem::exists(canary));
```

客机上 **`D:` 就是这个光驱**（`DriveType=5`）。设成 `startConnected=FALSE` 之后它变成
**"无介质"光驱**，而对无介质驱动器的路径调用 `std::filesystem::exists()` 会**抛
`ERROR_NOT_READY`**，不是返回 false——于是这个"环境"用例以 `exception` 失败。

**正解是整个移除设备**（`sata0:1.present = "FALSE"`）：既达到避免启动卡住的原意，又没有
"空光驱"这个副作用。改完 run6 该用例通过。

> 这条的普遍教训：**测试里用真实盘符做"不存在的路径"断言时，那个盘符在环境里是什么设备很重要。**
> `D:` 是光驱、是网络盘、还是没有，会给出三种不同结果。

---

## 7. 与开发机历史读数的关系

| 读数 | 环境 | 可比性 |
|---|---|---|
| 10-07 的 144/146、145、2/146（manifest §1.38/§1.39） | 开发机，且被中途杀掉的测试污染过过程表与设备集 | **不可比** |
| run4 = 132/146 | 干净客机，但 harness 还有 4 处缺口 | 修正前的基线 |
| **run6 = 144/146** | 干净客机，harness 已补齐，唯一失败是缺 Visual Studio | **当前可信读数** |

---

## 8. 下一步

1. **重复运行刻画两个间发问题**（§4）：`-Filter` 各跑 10 次，统计复现率，复现时保留现场。
2. **环境补完（可选）**：若要让 146/146 全绿，需要在客机装 Visual Studio Build Tools
   （只为两个 structural 用例）。这不影响产品结论；先记录为环境限制更划算。
3. **然后进入 T1-02（组合根 + 服务骨架）**——数据面测试已有一个可信基线，可以开始往上盖。
