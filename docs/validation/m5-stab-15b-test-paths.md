# R-17b：测试的源码树绝对路径改为运行时根

## 1. 问题

若干 M3/M4 测试在**配置期**把开发机的绝对路径编进二进制：

```text
MYRD_M3_*_FIXTURE_PATH   = ${CMAKE_SOURCE_DIR}/tests/fixtures/m3/...
MYRD_M3_*_MATRIX_PATH    = ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/m3/...
MYRD_PRODUCT_EXE_PATH    = $<TARGET_FILE:myrd>      (绝对构建路径)
```

宿主机测试运行器因此要求**客机目录布局与开发机完全镜像**（把整棵树放到同一个
`C:\Users\...\build_my_ramdisk\.worktrees\m5-lifecycle`）。换一个检出位置（另一个用户、
另一个盘、CI 的临时目录）就跑不了。任务清单里这条叫“非镜像布局也能跑”。

## 2. 做法

**运行时根**，不是重写二进制：

1. `tests/support/test_paths.h`（header-only）：
   - `MYRD_TEST_ROOT` 未设置 → 原样返回配置期路径（开发机、既有命令行完全不变）；
   - 已设置 → 取路径里 `tests\fixtures\` 或 `out\build\` 之后的部分，拼到根上；
   - 两个标记都不在 → 原样返回：宁可报“文件不存在”，也不猜一个别的文件。
   - 窄路径（fixture loader 收 `const char*`）与宽路径（`MYRD_PRODUCT_EXE_PATH`）各一个入口。
2. 6 个测试文件里 23 处宏引用改为 `MYRD_TEST_PATH(...)` / `MYRD_TEST_PATH_W(...)`。
3. `tools/m5vm/Invoke-M5Vm.ps1`：
   - `-Stage` 读取 `M5VM_GUEST_REPO`（缺省 = 开发机路径），把 `tests.json` 里每条命令和
     `WORKING_DIRECTORY` 从开发机路径改写到客机路径。**要同时替换两种写法**：ctest 报告用
     正斜杠（`C:/Users/...`），宿主机 `$repo` 是反斜杠，只替一种会留下开发机路径（实测：
     非镜像运行时报告 `MISSING ... C:/Users/...`）。
   - `-Run` 在客机会话里设置 `$env:MYRD_TEST_ROOT`，子进程继承。
4. 删除“必须镜像”的注释与旧的 `$guestRepoWin = $repo` 守卫；守卫改为“绝对路径且以
   `m5-lifecycle` 结尾”，仍然拒绝向意外路径递归删除。

## 3. 客机读数

### 3.1 非镜像布局（新能力）

```text
staged to usr@myrd-vm:C:\m5repo\m5-lifecycle  (tests.json in C:/m5/control)
filter '^(unit\.m3\.journal_format|unit\.m3\.persistence_types)$': 2 of 173 cases selected
[1/2] unit.m3.persistence_types
[2/2] unit.m3.journal_format
2 of 2 passed
```

这两个用例都**必须读取 `${CMAKE_SOURCE_DIR}/tests/fixtures/m3/*`**，通过 `MYRD_TEST_ROOT` 在
`C:\m5repo\m5-lifecycle\tests\fixtures\m3\*` 找到；同时证明 `tests.json` 的命令改写生效
（旧镜像树已删除，命令仍能启动）。

非镜像布局下的 e2e 也验证了：

```text
filter '^integration\.m5\.install_uninstall$': 1 of 173 cases selected
[1/1] integration.m5.install_uninstall
1 of 1 passed
```

### 3.2 反证：让旧路径消失

删掉客机上的镜像树后、在修正改写之前，同一组用例是：

```text
MISSING C:/Users/JimmyLin/Desktop/build_my_ramdisk/.worktrees/m5-lifecycle/out/build/msvc/tests/Release/myrd_m3_persistence_types_tests.exe
0 of 2 passed
```

这条反证既说明“镜像布局曾经是硬依赖”，也说明修正后的通过不是旧树残留。

### 3.3 默认布局不回归

```text
staged to usr@myrd-vm:C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m5-lifecycle
filter '^(unit\.m3\.journal_format|unit\.m3\.persistence_types)$': 2 of 173 cases selected
2 of 2 passed
```

`M5VM_GUEST_REPO` 不设置时，重写是恒等变换、`MYRD_TEST_ROOT` 指向同一路径，历史行为不变。

## 4. 本地（开发机）读数与已知失败

改完后本地构建通过。**三例在改动前后同样失败**（用 `git stash` 把 7 个改动文件退回后
重跑，失败相同，因此与本项无关）：

- `integration.m4.product_composition` 的 `driver_unavailable_is_zero_write: exception`
- `integration.m3.recovery_oracle` 的 `selects_a_only_b_only_equivalent_and_fallback: exception`
- `model.m3.checkpoint_crash` 的 `clones_recover_only_old_or_new_complete_maps: exception`

它们都创建临时目录/克隆，失败签名是未捕获异常；本轮只记录现象，不把它算进 R-17b 的
通过依据。非镜像布局的通过性由上面的客机读数支撑。

## 5. 影响面

- 产品代码：无改动。
- 测试代码：`tests/support/test_paths.h`（新增）+ 6 个文件 23 处宏引用。
- 宿主机工具：`tools/m5vm/Invoke-M5Vm.ps1`（路径重写 + `MYRD_TEST_ROOT`）。
- 文档：`docs/operations/m5-vm-harness.md` §9 的“测试硬编码绝对路径”一项关闭。
