# T1-06 中断提交判定可达

**结论**：§14.3 的七条用例全部有实测支撑；过程中发现并修掉一个让"确认提交"永远不可能的缺陷
（`inspect_checkpoint` 把 **base generation** 当作 **prior generation** 比较）。

**客机**：`myrd-vm`，Release，硬重启 + 清空 `C:\ProgramData\MyRamDisk\{profiles,operations}`。

---

## 1. 增量本体：从 P4 durable correlation 造 launch 起只读检查 worker

改前 `ServiceWorkerControl::reconcile_checkpoint` 用 `require_session(profile_id, true)`：
它**只能 adopt 一个活着的 runtime**，否则报 `execution_not_bound`。而这正是本问题存在的情形——
提交已到 P4、切它的 worker 随后消失，没有 runtime 可以 adopt。所以**正面路径一次都跑不到**。

改后：没有 session 时，从 durable profile 造 launch 请求，**launch 但不 start**，问
`recovery_proof`，在**每个分支**都关闭 inspection 并等进程退出（`worker_process_gone`）。
launch 失败作为失败上报，绝不当作"未提交"。

---

## 2. 缺陷：确认提交的判据里混进了另一个量

`candidate_mismatch_mask` 的六个条件之一是 `observed.prior_generation != expected.prior_generation`，
而 `observed.prior_generation` 取自 manifest 的 **`base_generation`**——它是 delta 所基于的
**基底镜像代次**，提交 checkpoint 时**不前进**；correlation 里的 `prior_generation` 是
**这次候选所在的上一代**。两者只有在"该 bundle 的第一个 checkpoint 之前"才相等。

**客机读数**（`m5.production_execution_backend`，请求 `945edec5…`）：

```text
PROBE-RECOVERY: verdict=0 mask=0x2
  observed(base=0 candidate=2 native=3904 slot=0 uuid_equal=1)
  expected(prior=1 candidate=2 native=3904 slot=0)
```

即：镜像、代次、native id、slot、digest 全部相同，只有 base(0) ≠ prior(1) ⇒ 判
`OwnershipUnprovable`。**任何第二个及以后的 checkpoint 都不可能被确认为 committed**，
而这些正是运行中的盘会提交的全部 checkpoint。唯一的正面判据因此从未生效过。

**修法**：候选判据回到规格的**五条**（镜像 uuid、candidate generation、native operation id、
manifest slot、manifest digest）；prior generation 只用于**否定**判据——那里比较的是
bundle 真实记录的 `candidate_generation == expected.prior_generation`，成立。
`CheckpointRecoveryInspection::prior_generation` 随之改名为 **`base_generation`**（它本来就是这个意思）。

修后同一用例（请求 `3ebcb49b…`）：

```text
PROBE-RECOVERY: verdict=2 mask=0x0 observed(base=0 candidate=2 native=3904 slot=0 uuid_equal=1)
```

---

## 3. §14.3 七条用例的落点

| 用例 | 落点 | 读数 |
|---|---|---|
| prior generation 可证明 | `m5_backend_recovery_proof_proves_the_prior_generation`（真 worker/真 bundle：flush 后撤销提交，再对死掉 worker 的 bundle 取证明） | 通过：disposition `not_committed`，`generation == prior_generation` |
| candidate generation 可证明 | `m5_backend_recovery_proof_proves_the_landed_candidate_read_only` | 通过：disposition `committed`，`generation == candidate_generation` |
| 归属不可证明 | 同一用例的第二个 correlation（改一个 digest 字节） | 通过：`resolution_unresolved` + `commit_indeterminate` |
| correlation 缺 slot/digest/native id | `m5_worker_control_refuses_a_correlation_that_cannot_prove_ownership` / `..._without_a_native_id` | 通过：`recovery_not_provable`，且**不问 worker** |
| 零 digest | `m5_worker_control_treats_a_zero_digest_as_absent_not_as_a_match`（slot 非零、digest 全零） | 通过：`recovery_not_provable` |
| 检查 worker launch 失败 | `m5_worker_control_reconcile_reports_a_launch_it_could_not_complete` | 通过：surface 为**失败**，不是三种判定之一；模型层 `reconciliation_failure` 另有用例 |
| 只读性 | 两个 backend 用例都在检查前后对 bundle 全目录做字节指纹（名字+长度+内容） | 通过：指纹逐字段相同 |

**只读性用的是内容指纹而不是"看有没有新文件"**：`fingerprint_bundle` 排序后遍历每个文件，
折入文件名、长度和全部字节；两个用例都在**同一份 bundle** 上比较前后两次指纹。

---

## 4. 证据（请求 ID）

| 用途 | 请求 |
|---|---|
| 首次单次运行（暴露 mask=0x2） | `945edec513b24d35aabc59ba2f4b7473` |
| 修后两个套件同时通过 | `3ebcb49b848a4b259decf2c3d42e4640` |

开发机 fake-only 套件：`m5.service_worker_control` 17/17、`m5.service_orchestration` 25/25、
`m4.checkpoint_operation` 全绿。

## 5. 未在此增量内验证

- `require_clean` 策略下的 stop（e2e 只走 `checkpoint_required`）；
- 真实断电（强制断电）后的同一条证明链——归 T1-09。
