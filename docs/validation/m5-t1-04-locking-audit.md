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

---

## 7. 把审计变成可执行的守卫（part 97）

**问题**：这次审计是**手做的**，而手做的东西对"下一个人加一个查询"毫无保护。
**一个新的读路径只要复制最近的 `std::scoped_lock`，就会静默地把排队行为带回来**，
**而且没有任何行为测试会发现**——**一个会等待的查询仍然返回正确答案**，
只是慢。**"慢"不会让断言变红。**

**因此新增结构性测试** `structural.m5.read_write_separation`
（`tests/unit/test-m5-read-write-separation.ps1`），断言两件事：

1. **门本身是读写原语**（`shared_ptr<std::shared_mutex>`），不是普通 mutex；
2. **三个纯查询取共享、五个写入取独占**，逐函数按名字核对。

**失败时会指名道姓**：

```
FAIL: list_records is a query but takes the gate exclusive; it must be shared
```

**这个失败形态比"在某台空闲机器上量一次时延"有用得多。**

### 7.1 守卫的正例与负例都验过

**一个不能失败的守卫等于没有守卫**，所以两种情形都实测：

| 情形 | 结果 |
|---|---|
| 真实源码 | `read/write separation holds: 3 queries shared, 5 mutations exclusive`，**exit 0** |
| **副本里把 `list_records` 的锁改回独占** | `FAIL: list_records is a query but takes the gate exclusive; it must be shared`，**exit 1** |

负例是在 `C:\tmp\rwprobe` 的**源码副本**上做的，**没有改动真实文件**。

### 7.2 回归读数

`ctest -R "^unit\.m3\.|^unit\.m4|^integration\.m4|^m4\.|^structural\.|^m5\.service_operation_store|^m5\.service_orchestration|^m5\.execution_scheduler"`
→ **49 / 51**，两条失败都是**开发机环境**：

| 失败 | 原因 |
|---|---|
| `unit.m4.profile_store` | 既有已知开发机环境失败 |
| `m5.service_operation_store` | `CreateDirectoryW` 断言，**客机 3/3 通过**（§5） |

**⇒ 新增的结构性测试没有引入回归。**

### 7.3 客机读数，以及我又踩了一次 Stage 坑

**第一次在客机上跑它时，只选中了 1 条 —— 因为我把 `-Stage` 忘了。**

```
filter: '^structural\.m5\.read_write_separation$|^m5\.service_operation_store$'
       : 1 of 151 cases selected        ← 我的新测试根本不在客机的 tests.json 里
```

**客机的测试清单来自 `ctest --show-only=json-v1` 的抓取，只在 `-Stage` 时刷新。**
**我加了测试、跑了 `-Run`、没跑 `-Stage`** ⇒ 客机上还是旧的 151 条。

**补跑 `-Stage` 后**：

```
staged 152 tests -> C:\tmp\m5-vm\stage
filter '^structural\.m5\.read_write_separation$': 1 of 152 cases selected
[1/3] … [3/3]  structural.m5.read_write_separation#1..#3
3 of 3 passed
```

**测试矩阵从 151 变成 152。**

**这条教训与 handoff §7.0.9 的"陈旧二进制"是同一族，但位置不同**：
**不是二进制旧了，而是测试清单旧了。判据是总数**——
**"151" 而不是 "152" 就说明清单没刷新。**


