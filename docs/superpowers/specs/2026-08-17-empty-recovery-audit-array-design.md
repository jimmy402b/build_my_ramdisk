# RecoverShutdown 空审计数组修复设计

## 背景与根因

真实虚拟机在 `RecoverShutdown` 阶段已经成功完成恢复和干净退出：生命周期证据为 `status=PASS`，代次从 CreateKill 的 2 推进到 4，A/D 工作负载哈希有效，且恢复后没有 AIM 盘、R:/S: 盘符或相关进程。

失败发生在证据序列化边界。`Assert-AuditDescendants` 在没有子进程时通过 PowerShell 管道返回零个对象；PowerShell 5.1 将调用结果折叠成 `$null`，而 `ConvertTo-Json` 在当前有序字典中将该值写成 `{}`。控制器按冻结契约要求 `recovery_audit.children` 必须是 JSON 数组，因此正确地以 `RecoverShutdown audit is invalid` 阻止后续阶段。

## 设计决定

采用生产者修复：`Assert-AuditDescendants` 无论返回零个、一个还是多个子进程，都必须返回一个数组对象。零子进程必须稳定序列化为：

```json
"children": []
```

不放宽控制器或最终验证器；`{}`、`null` 和其他非数组形状仍然无效。也不修改或复用已进入终态的资格根 `E:\myrd-v1-qualification-20260817-a`。

## 实现与验证

先在生命周期 runner 的真实自测中加入回归：调用生产 `Assert-AuditDescendants` 的零子进程路径，断言返回值是数组、元素数为零，并断言经生产 `Json` 函数序列化后精确得到 `{"children":[]}`。该测试在旧实现上必须以预期原因失败。

随后只修改审计函数的返回边界，使数组作为单个管道对象返回。原有非空子进程的身份、命令行、时间窗口和禁止配置进程检查保持不变。

GREEN 验证包括：生命周期原生自测、guest qualification/controller 自测、相关单元测试、PowerShell AST、`git diff --check`，以及适当的 Debug/Release m4-v3 回归。完成后提交代码，并以新提交重新生成 exact-nine qualification kit，逐项核对 manifest 与文件哈希。

## 人工测试续接

旧 `-a` 根保留为不可变的失败证据。用户替换整个 kit 后，使用新根 `E:\myrd-v1-qualification-20260817-b` 从 Initial 状态重新开始。第一次再次到达 `CRASH RESET ARMED` 后，仍只通过 VMware UI 执行一次 Reset，并在 Windows 启动后使用同一命令和同一 `-b` 根继续。
