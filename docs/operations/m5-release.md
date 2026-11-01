# MyRamDisk M5 发布流程（1.0.0）

**定位**：这份文档回答“同一个 commit 怎么产出可核对的发布物”。用户面向的支持范围/限制在
`docs/operations/m5-support-matrix.md`；发布前的演练在 R-19（`docs/validation/`）。

## 1. 版本

- **单一版本源**：`src/product/product_version.h` 的 `kProductVersion`。
- 三个二进制都支持 `--version`，都打印同一个字符串（R-07 客机读数：`cli/service/gui: 1.0.0`，
  `consistent version: 1.0.0 (matches product_version.h)`）。
- `CHANGELOG.md` 记录版本与提交映射。

## 2. 构建（可重复）

```powershell
# 开发机；Release
cmake --build out/build/msvc --config Release
```

**可重复构建**：顶层 `CMakeLists.txt` 给 MSVC 加了 `/Brepro`（编译与链接）。没有它，PE 时间戳是构建
时间，同一个 commit 每次构建的 SHA-256 都不同；发布承诺是 SHA-256 校验，所以构建必须确定。

**实测（2026-11-01，同一工作树、同一 commit）**：普通构建与 `--clean-first` 构建的四个二进制
SHA-256 **完全一致**：

```text
myrd-cli.exe        08D3837A869AAE50A2B6C786C95B8505E609EB5E33902A2C525C0733F123F5EE
myrd-service.exe    7E79BDEF28A0990FC5A3D42BC62E764837AC87635FF817E4094B2E4345F22D34
myrd-m5-worker.exe  55EEC4E64CFE3937F94EBFFA12460BB9D44EFB2BC7A8848D3F04A777536AA348
myrd-gui.exe        05D15424B7E53CF23180B98FE5C6C6BFA1682128D349937FE119E71B28A650C1
```

复现方法：连续两次 `--clean-first` 构建，比较上面四个文件；两次之间不改源码、不换工具链。

## 3. 未签名发布（决策 2）

**1.0.0 没有代码签名证书，发布物是未签名的。** 后果要写进发布说明，不藏着：

- Windows SmartScreen 会对下载的安装包显示“未知发布者”，用户需要“仍要运行”；
- 没有 Authenticode 签名，无法用 `signtool verify` 证明来源；
- 完整性与来源靠 **SHA-256 清单**：发布时给出四个二进制 + 安装脚本的哈希，用户可自行核对。

**不做的事**：不自签证书冒充受信任发布者；不因为未签名而降低其他安全措施。

## 4. 发布物与校验和

- 二进制：`myrd-service.exe`、`myrd-cli.exe`、`myrd-gui.exe`、`myrd-m5-worker.exe`；
- 安装/卸载：`tools/install/Install-MyRamDisk.ps1`、`Uninstall-MyRamDisk.ps1`；
- 清单：`tools/New-M5ReleaseManifest.ps1 -Configuration Release` 写出 manifest（二进制 SHA-256 +
  evidence 目录读数），`tools/Verify-M5Evidence.ps1 -EvidenceDirectory out/m5-evidence` 复核。

**纪律**：任何产品二进制变化都会让已冻结的 manifest 失效；发布前必须重新冻结并让 validator 通过
（R-19）。

## 5. 发布说明必须包含

1. 版本与 `CHANGELOG.md` 链接；
2. 支持范围与明确不支持项（引用 `m5-support-matrix.md`）：仅 Win11 x64、无卷标、无 over-commit、
   无版本回滚（只支持重装）、簇大小固定为默认（R-17a 之前）；
3. 数据丢失上限：已提交（checkpoint 完成）的数据在断电后保留；未提交的写入可能丢失，最多是最后一次
   成功 checkpoint 之后的内容；
4. 驱动要求：`phdskmnt` ≥ 1.2.28.93，安装器只检查不安装；
5. 卸载注意：默认保留 `C:\ProgramData\MyRamDisk` 记录，`-PurgeData` 才删除；运行中的盘会先被停掉；
6. 未签名与 SmartScreen 提示，以及 SHA-256 核对方法。

## 6. 发布前检查（与 R-19 对应）

- [ ] 同一 commit 构建 + 两次 `--clean-first` 哈希一致（§2）
- [ ] `New-M5ReleaseManifest.ps1 -Configuration Release` 重新冻结，`Verify-M5Evidence.ps1` accepted
- [ ] 干净 VM：安装 → 冒烟 → 升级（覆盖安装）→ 重装 → 卸载，读数写进 R-19 证据
- [ ] 全量套件连续两次一致（默认门禁 162 例；`vswhere` 缺失导致的 2 个 SKIP 要如实记录）
- [ ] 8 条 release gate（`2026-10-29-m5-stability-plan.md` §0 + §4）逐条引用读数
- [ ] 发布说明按 §5 写完，支持矩阵无“未测”被写成“支持”
