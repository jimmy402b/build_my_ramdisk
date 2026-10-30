# STAB-08 / R-07：语义化版本、三处一致、changelog —— **PASSED**

**结论**：`myrd-cli`、`myrd-service`、`myrd-gui` 三个随安装包发布的二进制都支持 `--version`，
打印同一个 `myrd::product::kProductVersion`（当前 **1.0.0**），并且与源码头文件逐字符一致；
changelog 在仓库根 `CHANGELOG.md`，按提交列出 1.0.0 的内容。**PASSED。**

**读数（客机安装目录里的二进制，`integration.m5.install_uninstall`，请求 `5fbd3008…`）**：

```text
--- the service, the CLI and the GUI report the same version (R-07)
  cli: 1.0.0
  service: 1.0.0
  gui: 1.0.0
  consistent version: 1.0.0 (matches product_version.h)
```

同一段断言还检查：三者退出码都是 0；版本号来自 `src/product/product_version.h` 的
`kProductVersion`，不是各自写死的字符串。

**开发机复核**（同一构建，`Start-Process -RedirectStandardOutput`，GUI 是 Windows 子系统所以
不能直接用 `&`）：

```text
cli: exit=0 out=[myrd-cli 1.0.0]
service: exit=0 out=[myrd-service 1.0.0]
gui: exit=0 out=[myrd-gui 1.0.0]
```

## 1. 实现落点

| 件 | 改动 |
|---|---|
| `src/product/product_version.h` | 唯一的版本常量（`kProductVersion = "1.0.0"`，major/minor/patch 与之一致） |
| `src/cli/cli_main.cpp` | 已有 `--version`，打印常量 |
| `src/service/service_main.cpp` | 新增 `--version`：在 `--console` 判断之前处理，打印 `myrd-service 1.0.0` 并 exit 0 |
| `src/gui/gui_main.cpp` | 新增 `--version`：在 `--self-check` 之前处理，打印 `myrd-gui 1.0.0` 并 exit 0（不需要桌面） |
| `tests/e2e/test-m5-install-uninstall.ps1` | 安装成功后统一用 `Start-Process` 取三者输出，比较版本号并对照头文件 |
| `CHANGELOG.md` | 1.0.0 条目 + 提交映射表（单盘准入、预算、全量套件、P4/P5 恢复、六项裁定） |

**为什么检查进安装用例而不是新开一个用例**：版本是“随安装包发布的那三个文件”的属性，安装用例
恰好是唯一拿得到“已安装二进制”的地方；顺带也不改变全量 160 例的构成。

## 2. 为什么 GUI 的 `--version` 要放在最前面

GUI 是 Windows 子系统程序：`wWinMain` 之前如果先注册窗口类、读服务状态，就会在没有桌面的环境
（服务会话、CI）里失败或卡住。版本是纯字符串，先答再退出，测试才能在任意会话里拿到它。

## 3. 边界

- `myrd-m5-worker.exe` 没有 `--version`：它是服务启动的私有子进程，不接受命令行；三处一致指的是
  **用户会看到的三件**（CLI / GUI / service）。
- 版本号目前是 1.0.0（release candidate）；R-19 发布演练时如果重新冻结，`product_version.h` 与
  `CHANGELOG.md` 必须一起改，安装用例会当场发现不一致。
- `CHANGELOG.md` 的提交映射是人工维护的；它不对应 git tag（发布前还没有 tag）。
