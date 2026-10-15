# Agent 执行约束（系统提示词）

以下约束对执行 M5 实现计划的 agent 具有**最高优先级**，覆盖 plan 内任何相反建议。

设计：`docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md`
任务清单：`docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`
基线：`cb3df08`（clean M4），实现分支 `feature/m5-lifecycle`

> v3 取代 `2026-09-30` 的 spec / plan / tasklist。旧文本里与 v3 冲突的条目以 v3 为准；
> v2 → v3 的变更清单见设计文档 §0。

## 1. 模型

只使用 `deepseek-flash` 模型。不得切换到任何其他模型。

## 2. 禁止子代理

不得调用子代理——禁止 Agent 工具、Workflow 编排，以及任何多代理 fan-out。
执行时使用 `superpowers:executing-plans` 单代理逐 task 推进，**明确禁用**
`superpowers:subagent-driven-development`（后者依赖子代理）。

## 3. 持续自主执行

除非需要用户介入，否则不间断地依次完成 tasklist 内的每个增量
（T1-01 → T4-04），直到所有增量完成并进入验收阶段。

仅在遇到必须由用户才能决策的事项时暂停，说明原因并等待用户指示；其余情况一律
继续推进，不因常规错误、单步测试失败或实现细节反复询问。

## 4. 测试环境

**开发机上允许执行不涉及真实物理资源的测试。** 判断标准只有一条：**这个测试会不会
创建真实的 worker 进程、AIM 代理盘或 Windows 卷。**

- **允许在开发机执行**：纯单元测试与模型/行为测试——编解码、状态机、纯策略函数、
  锁序、跨层数据贯通、错误映射等；只用 fake / 内存实现，不 spawn 进程、不开设备、
  不建卷、不碰 `phdskmnt`。
- **只允许在虚拟机执行**：任何会创建真实 `myrd-m5-worker` 进程、AIM 代理盘或 Windows
  卷的测试——例如 `m5.production_execution_backend`、`integration.m3.*` 的 Windows
  类、`m4.multi_instance`、`integration.m3.windows_acceptance`，以及全部 e2e 脚本。

**不确定时按"只能上虚拟机"处理。**

开发机上允许做的事：编辑源码、构建（编译/链接）、git 操作、读写文件、查看进程与
设备状态、运行上述纯逻辑测试。

原因：本机执行会创建真实设备。测试进程被中断（命令超时、手动中止）时 worker 会变成
孤儿并继续占着设备，若干次之后开发机的存储栈退化到连 `Win32_DiskDrive` 查询都挂起，
只有重启才能恢复。

推论，需要一并执行：
- 涉及真实设备的验证证据来自虚拟机运行，不来自开发机。
- 测试产物（测试可执行文件与 worker 二进制）在开发机构建，再送进虚拟机执行。
- 若某个"纯逻辑"测试实际创建了设备或 worker，视为违规：立刻停止，改到 VM 上取证据。

## 5. 虚拟机接入

统一通过 **OpenSSH**（`ssh.exe` / `scp.exe`）连接，使用**密钥认证**以保证非交互可用；
不使用 `plink` / `pscp`，不使用明文密码。

客户机 IP 目前不固定，固化方案待用户指定；在那之前，脚本一律把主机地址作为参数传入，
不得把地址硬编码进任何脚本。
