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

## 4. 复现（已完成部分）

```powershell
# 本机
ctest --test-dir out/build/msvc -C Release -R "^unit\.m5\.authorization$"   # 5/5
```
