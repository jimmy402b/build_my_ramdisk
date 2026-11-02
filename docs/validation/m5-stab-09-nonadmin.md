# R-11：非提权只读（部分完成）

## 1. 已完成：产品侧的授权边界

发布任务清单要求“非提权 list/show 可用；修改被拒且提示明确；不崩溃”。`disk_show` 原本是
`administrator_only`，所以非提权连自己拥有的盘都读不到。本项把它改成 **caller_filtered**，并在
服务端加了 owner 比较：

- `src/control/authorization.cpp`：`disk_show` 移入 caller_filtered 组（注释说明规则看不到资源，
  所以比较必须在加载 profile 的地方做）；
- `src/management/service_core.cpp`：`authorization.visibility == caller_only` 时比较
  `profile.owner_sid` 与 `caller.primary_sid`，不同则 `authorization_denied`（否则就是拿 id
  任意读别人 profile 的信息泄露）。
- 单测 `unit.m5.authorization`（本机，5/5）：`disk_show`/`disk_list` 是 caller_filtered（非提权
  得到 `caller_only`）；create/import/forget/export/repair/rebind 仍是 administrator_only
  （非提权与“过滤后的管理员”都被拒，提升的管理员通过）；start/stop/checkpoint/compact 是
  owner_or_administrator（owner 通过、外人被拒）；未认证与网络登录直接拒绝。

## 2. 未完成：客机上的进程级非提权 e2e

**没有取得客机读数，所以 R-11 不算 PASSED。** 记录所有尝试与它们的实际失败，避免下一个人
重复：

| 机制 | 结果 |
|---|---|
| `Start-Process -Credential`（CreateProcessWithLogonW） | GUI `--self-check` 退出 `0xC0000142`（STATUS_DLL_INIT_FAILED），空输出 |
| 计划任务 `-RunLevel Limited` + 用户密码 | 任务从不运行：`LastTaskResult=267011`（0x41303 "task has not yet run"）、`LastRunTime=1999-11-30` |
| `net user <name> <password> /add` | **挂起**：`net.exe` 与 `net1` 无输出存活 20 分钟，触发 host 侧 1800 s 截止（已用 `New-LocalUser` 替代，cmdlet 不挂） |
| `runas /trustlevel:0x20000 "cmd /c <wrapper.cmd>"` | 控制台命令（`whoami`）可以跑通；但 `.cmd` 里的 `echo %ERRORLEVEL%> file` 在本客机写出**空值**，且重定向失败会让批处理在写 exit 文件前中止 |
| `runas /trustlevel:0x20000 "powershell -File <wrapper.ps1>"` | 手工探针可用（exit=0、输出 1876 B），但在 e2e 的 `C:\m5\m5cap-*` 捕获目录里，写探针（`copy /y NUL C:\ProgramData\...`）仍然不产出 exit 文件 |
| `CreateProcessAsUser` + 受限令牌（`DISABLE_MAX_PRIVILEGE`，Administrators 放 disable 列表） | 子进程启动后 `0xC0000005`（access violation），两个版本（disable 列表/restrict 列表）都一样 |

这些失败都发生在**启动被测进程之前**，没有一次触及产品代码，因此它们是宿主/客机启动机制的限制，
不是产品缺陷。`kernel32`/`advapi32` 的组合在 session 0 的 agent 上下文里明显受限。

## 3. 下一步（给接续的人）

1. 不要在 agent 的 session 0 里追求“真正的非提权进程”：优先考虑**在客机交互式会话里**用一个
   标准用户登录执行同一套 CLI/GUI（VMware 控制台 + `runas`），或给测试加一个**只读的诊断入口**
   （例如 `--print-caller`）把 `CallerIdentity` 打印出来，再用 `disk_show` 的 owner 比较做
   进程级断言。
2. 若仍要在 session 0 做：先单独验证 `CreateProcessAsUser` 的最小例子（`cmd /c echo`）是否能跑，
   再叠加受限令牌；当前证据表明问题在令牌/会话组合，而不是参数。
3. 保留本页的失败表；重复上面任一机制前先读它。

## 3b. 2026-11-01 第二轮机制排查（在 24h soak 期间，空闲时段做只读探针）

| 机制 | 结果 |
|---|---|
| `vmrun -gu <标准用户> -gp <密码> runProgramInGuest` + `powershell -File step.ps1` | **能启动进程**（`whoami /user` 显示 `v\m5limited`，`myrd-cli --version` exit 0 且输出 `myrd-cli 1.0.0`）。但它是 **network logon**：`disk list` 得到 **exit 3**（`the service is not available`），与 SSH 会话语义一致——公有管道的 `(D;;GA;;;NU)` 明确拒绝网络登录。**因此它不能当 CLI 客户端。** |
| `Start-Process -Credential`（CreateProcessWithLogonW） | 连 `myrd-cli --version` 都是 **0xC0000142**（STATUS_DLL_INIT_FAILED），不只是 GUI。 |
| 计划任务 `-RunLevel Limited` + 用户密码 | 起初 `LastTaskResult=267011`（从不运行）。**给该用户补 `SeBatchLogonRight` 后运行成功**：`last_result=0`、`LastRunTime` 是当前时间、`whoami /user` 显示 `v\m5limited`（标准用户 SID）。这是目前唯一能在客机后台拿到“非提权 + batch logon”的方式。 |
| 补权限的做法 | `secedit /export /cfg before.inf /areas USER_RIGHTS` → 在 `SeBatchLogonRight` 追加该用户 SID → `secedit /configure /db secpol.sdb /cfg after.inf /areas USER_RIGHTS`（本机返回 1，但检查导出已含该用户，任务随即能跑）。 |

**待验证**：通过该计划任务运行 `disk list` 时，soak 正占着同一个 service，命令没有在 40 s 内返回（只看到进程在跑、没有输出文件）。**必须在 service 空闲时重测**：若 batch logon 仍被管道拒绝（exit 3），则 R-11 的客机读数在这个客机上不可达，只能记录为环境限制；若返回 0，则 R-11 机制成立，按下面的脚本取得五条读数。

`tools/m5vm/Test-M5NonAdmin.ps1` 目前用 vmrun 实现；重测后若计划任务可用，将改为计划任务机制（同一套 prepare/cleanup 与断言不变）。

## 4. 复现（已完成部分）

```powershell
# 本机
ctest --test-dir out/build/msvc -C Release -R "^unit\.m5\.authorization$"   # 5/5
```

---

## 6. 2026-11-02：**R-11 PASSED**（batch logon + 已安装服务）

**机制**（前六种都失败，最终成立的组合是两条）：

1. **非提权进程**：计划任务 `-RunLevel Limited` + `SeBatchLogonRight`（`secedit` 导出→追加该用户
   SID→导入）。未授予该权限时任务从不运行（`LastTaskResult=267011`）；授予后以标准用户运行。
2. **服务必须在用例之外存活**：agent 用例里 `Start-Process` 启动的控制台服务会随 agent 的 job
   对象一起被杀（实测：每步都答 `the service is not running`）。改为 **安装服务**（SCM 托管，
   LocalSystem），步骤之间持续可用。

**客机读数**（`tools/m5vm/Test-M5NonAdmin.ps1` 一次完整运行，5/5 断言通过）：

```text
gui-selfcheck: exit=0
  self-check: 1 disk(s) | T: stopped/configured Stopped generation 1 not running serial=- | tray: MyRamDisk: 1 disk
cli-list: exit=0
  disk.list: completed | bdc1c962-…  T:  Ready ... | 1 disk(s)        (只有自己的盘，没有管理员的 357c9701-…)
cli-show-own: exit=0
  disk.show: completed | bdc1c962-…
cli-show-other: exit=4
  error: The profile belongs to another user
  command: disk.show  step: service_core.authorize_owner  code: 0x5603
cli-create: exit=4
  error: Only an elevated administrator may register a profile
  command: disk.create  step: service_core.authorize_registration  code: 0x5603
PASS: the standard user reads its own disk, is refused elsewhere, and nothing crashed
READING: nonadmin.result=PASS
```

**这条用例抓出并修掉两个真实产品缺陷**：

1. **B7 的客户端校验让非提权客户端永远连不上**：`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)`
   对 LocalSystem 的服务进程返回拒绝，客户端报 `the pipe's server process could not be opened`。
   现在 `OpenProcess`/`OpenProcessToken` 失败时回退到**管道 owner**（SYSTEM 或 Administrators），
   这是抢占者伪造不了的属性；进程路径仍优先使用。
2. **公有管道没给 AU/BA `READ_CONTROL`**：owner 校验需要读安全描述符，而客户端访问掩码只有
   `0x00100183`。现在掩码与 SDDL 的 AU/BA ACE 都改为 `0x00120183`（只多了 READ_CONTROL，
   不放开数据/实例权限）；`kPublicPipeClientAccessMask` 的注释写明原因。

**交付物**：`tests/e2e/test-m5-nonadmin-prepare.ps1`（agent：装服务、建两个 profile、cleanup 忘盘+卸载）、
`tools/m5vm/Test-M5NonAdmin.ps1`（host：建标准用户+授权、调度任务跑 GUI/CLI、断言、清理）。
发布全量排除该 host 驱动用例（169/178 口径不变）。