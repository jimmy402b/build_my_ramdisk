# T1-04 读/写分离：锁审计与修正

**状态**：**部分完成（审计 + 修正已做；验收用例待 T1-05 的命令面）**。
**相关**：tasklist T1-04、`src/management/operation_store.cpp`、`src/management/execution_scheduler.cpp`

---

## 1. T1-04 的要求

> **读/写分离**：变更取独占、查询取共享或无锁（逐条审计）；
> **create 进行中 `disk list` 1 秒内返回**

---

## 2. 审计结果

### 2.1 管理层的并发原语（逐条）

| 位置 | 原语 | 结论 |
|---|---|---|
| `execution_scheduler.cpp` | 单个 `std::mutex state_mutex_` | 只保护调度状态，**没有读写区分**；它不是"查询"的路径，暂不影响 T1-04 |
| `operation_store.cpp` | **按 key 共享的门** `shared_operation_gate(key)` | **这是读/写分离的落点** |

**关键发现**：**整层没有使用任何读写锁**
（`shared_mutex` / `shared_lock` / `SRWLOCK` 在 `src/management/`、`src/control/`、`src/service/` 下命中数为 **0**）。
所以"查询取共享锁"这条**此前并不成立**——查询取的是**独占**锁。

### 2.2 具体偏差

`operation_store.cpp` 里，**纯查询与写入用的是同一个 `std::scoped_lock`（独占）**：

| 锁所在行 | 函数 | 性质 |
|---|---|---|
| L965 | `load_record` | **查询** |
| L1013 | `write_record` | 写入 |
| L1176 | `list_records` | **查询** |
| L1272 | `find_request` | **查询** |
| L1298 | `admit_with_id` | 写入（read-modify-write，必须独占） |
| L1378 | `reconcile_after_restart` | 写入 |
| L1449 | `request_cancel` | 写入 |
| L1489 | `cleanup_terminal_records` | 写入 |

**⇒ 三个纯查询会排在写入后面。**

**为什么这直接关系到验收**：`disk list` 要列全部 profile。
**若某个 profile 正在 create（一次写入），那么列到它时查询会被那个写入挡住**，
整条 `disk list` 就跟着慢下来——**而要求是 1 秒内返回**。

---

## 3. 修正（已实施）

把门从 `std::mutex` 改成 **`std::shared_mutex`**：

- `OperationStore::Impl::gate` 类型改为 `std::shared_ptr<std::shared_mutex>`；
- `shared_operation_gate()` 的返回类型与内部注册表的 `weak_ptr` 同步改；
- **三个纯查询改用 `std::shared_lock`**：`load_record`、`list_records`、`find_request`；
- **所有写入仍用 `std::scoped_lock`**（对 `shared_mutex` 即独占）。

**为什么这不削弱正确性**：
写入者仍取独占，**任何读者都不会看到写了一半的记录**，两个写入者仍不能交错。
改变的只是：**若干读者可以并行**，且读者只等"正在写的那个写入者"，而不是排在整队写入者后面。

---

## 4. 读数

| 项 | 结果 |
|---|---|
| 干净构建 | **0 error / 0 warning** |
| 开发机 `m5.service_operation_store` | **失败**（见 §5，环境原因） |
| 客机 `m5.service_operation_store` | **3/3 PASS** |
| 客机 `m5.service_orchestration` | **3/3 PASS** |

（请求 `33c75c2ed0b448af9d2f270c370b0d49`，`-Repeat 3`，共 6 次执行全部通过）

---

## 5. 开发机那条失败与本次改动无关（已核实）

`m5.service_operation_store` 在开发机报：

```
FAIL tests/integration/m5_service_operation_store_tests.cpp:92: created || error == ERROR_ALREADY_EXISTS
```

**这是 `CreateDirectoryW` 的失败，发生在任何锁被获取之前**，
因此与"锁类型从 `mutex` 改成 `shared_mutex`"无关。
**同一份二进制在客机上 3/3 通过**，所以它是**开发机的目录状态/令牌环境问题**，
与既有的 `unit.m4.profile_store` 同类。

---

## 6. 尚未完成的部分（依赖修正）

**T1-04 的验收用例"create 进行中 `disk list` 1 秒内返回"目前无法执行**：
**命令面里还没有 `list` 操作**。审计确认 `src/management/`、`src/control/`、`src/service/`
下**没有 list 类操作**——`disk list` 属于 **T1-05** 的 public pipe 命令面。

**⇒ tasklist 里 T1-04 只声明依赖 T1-02，这条依赖是错的。**
要么把 T1-04 的依赖补上 T1-05，要么把它的验收下移到 `OperationStore::list_records()` 层面
（在 create 进行中测 `list_records()` 的返回时延）。**这需要一次范围裁决，
已记入 master-plan §8，不擅自改。**

**在那之前，本增量只能标 PARTIAL，不标 PASSED。**
