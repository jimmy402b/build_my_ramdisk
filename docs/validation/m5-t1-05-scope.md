# T1-05 的范围与真实缺口（供下一会话直接接手）

**状态**：**未开始实现**，但**它的服务端基础比 tasklist 描述的完整得多**。
**这份文档的目的是：让接手的人不必重新摸一遍边界。**

---

## 1. 已经存在的（不要重写）

**命令面本身已经在 `src/control/` 里定义齐了**，不是从零开始：

| 组件 | 位置 | 状态 |
|---|---|---|
| 命令类型全集 | `src/control/command.h` | **齐全**：`DiskListCommand`、`DiskCreateCommand`、`DiskShowCommand`、`DiskStartCommand`、`DiskStopCommand`、`DiskCheckpointCommand`、`DiskImportCommand`、`DiskConfigSetCommand`、`DiskCompactCommand`、`OperationList/Show/Wait/Cancel`、`StatusCommand`、`DriverStatusCommand`、`DiagnosticsCollectCommand`、`DiskRepairProfileCommand`、`DiskRebindCommand`、`DiskForgetCommand` |
| 编解码 | `src/control/ipc_codec.cpp` | **双向齐全**（`encode_command_payload` / `decode_command_payload`，含每个变体） |
| 帧格式 | `src/control/ipc_frame.*` | 齐全 |
| 传输 | `src/control/pipe_transport.*` | 齐全 |
| 服务端 | `src/control/public_pipe_server.*` | 齐全 |
| 客户端连接 | `src/control/public_pipe_client.*` | **能连、能收发帧** |
| 授权 | `src/control/authorization.*` | 齐全（`authorize_public_command`） |
| **`disk list` 的处理器** | `src/management/service_core.cpp:1458` | **已实现**，含只读校验与授权 |

**⇒ 换句话说：`disk list` 在"服务端 + 协议 + 授权"上是**做完了的**。**

---

## 2. 真正的缺口（这才是 T1-05 的工作量）

**缺的是"把命令变成一次请求"的那一层，以及 CLI 可执行文件。**

**证据（已核实）**：

- `git grep encode_command_payload` 在 `tests/` 下**零命中**，
  在 `src/` 下只有 `ipc_codec.*` 与 `service_core.cpp`（**服务端的 decode 方向**）。
  **⇒ 没有任何地方从"一个 `control::Command`"构造出"一个可发送的 `Frame`"。**
- **没有任何测试端到端驱动过 `PublicPipeClient`**（`git grep PublicPipeClient tests/` 零命中）。
- `src/CMakeLists.txt` 的可执行目标里**没有 CLI**：
  `myrd`（M4 前台引擎）、`myrd-service`（SCM 宿主）、`myrd-m5-worker`、以及一批 e2e。
  **`public_pipe_client.cpp` 被编进了库，但没有任何可执行文件使用它。**

### 2.1 因此 T1-05 要写的东西，按依赖顺序

1. **客户端请求构造**（新）：`Command` + caller 身份 + request id + 超时策略 → `Frame`。
   **这是整块工作的核心**，也是 T1-04 的验收所等的那一步。
2. **响应解码与呈现**：`Frame` → 结构化结果 → 人类可读输出 + 退出码。
3. **CLI 可执行**（新目标，建议 `myrd-cli`）：参数解析 → 上面两步 → 打印。
4. **十条命令逐一接上**（`list` 是第一条，也是最简单的一条）。
5. **端到端测试**：真实服务 + 真实 CLI（**必须上客机**，它创建真实 worker/盘）。

---

## 3. 为什么我没有在这一轮强行开工

**我问了自己一个问题：能不能只做一条 `disk list` 的 CLI，把 T1-04 的验收解锁？**

**答案是不能，理由不是"时间不够"，而是"半成品会被推翻"**：

- 客户端请求构造层是**十条命令共用**的骨架。
  它的形状（caller 身份怎么带、request id 由谁生成、超时是每命令还是全局、
  错误怎么映射到退出码）**必须一次设计对**；
- **没有任何现存代码或测试可以作为它的样板**（这正是 §2 的证据所证明的）；
- **这三轮之内写不完、更验证不完**（验证需要真实服务 + 客机）。
  **而一个未验证的 IPC 客户端比没有更危险**：它会看起来"能用"，把错误固化进协议。

**所以：把边界写清楚，比留一个半成品更有用。**
**这也是 master-plan §8 第 9 项那场排查换来的判断——
"看起来绿"不能替代"实际知道"。**

---

## 4. 接手时的第一件事

**先读 `src/control/service_core.cpp:1458` 的 `DiskListCommand` 分支**：
它已经展示了服务端对"一个命令"的完整处理（只读校验 → 授权 → 返回 `ServiceResponse`）。
**客户端要做的是它的镜像**，而不是发明新东西。

**然后写那个缺失的构造层**，并**先为它写一条不涉及真实设备的测试**
（构造 `Frame` → 就地解码 → 断言往返一致）。
**这一步可以在开发机上完成**，也是唯一能在没有服务的情况下验证的环节。

---

## 5. 顺带修掉的一个循环依赖

**tasklist 原先写着 `T1-05` 依赖 `T1-04`，而 `T1-04` 的验收又需要 `T1-05` 的 CLI。**
**那是一个死锁**——两个增量互相等待，谁也不能先完成。

**已修**：**`T1-05` 的依赖改为只 `T1-02`**（它只需要服务骨架，那是 PASSED 的）。
**实际顺序因此变成**：**T1-05 先做（建命令面）→ T1-04 用它的 CLI 跑验收 → T1-04 收尾。**

**这也是为什么上一轮我判断"T1-04 保持 PARTIAL"是对的**：
它等的那个东西必须由 T1-05 提供，而 T1-05 不该等它。

