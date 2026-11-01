# R-18：兼容性环境读数

发布矩阵要回答的三个“用户实际会遇到的环境”，加上 AIM 盘自身的两个属性。全部由
`integration.m5.compatibility` 在客机上实测（1/1、100 s）。

## 1. 客机读数

```text
READING: defender.realtime=True
READING: defender.antivirus=True
READING: compressed.attribute=Directory, Compressed
READING: compressed.create=succeeded
READING: compressed.volume=present
READING: compressed.write_bytes=1048576
READING: compressed.checkpoint=succeeded
READING: compressed.stop=succeeded
READING: compressed.result=full-cycle-ok
READING: efs.cipher_exit=0
READING: efs.attribute=Directory, Archive, Encrypted
READING: encrypted.create=succeeded
READING: encrypted.volume=present
READING: encrypted.write_bytes=1048576
READING: encrypted.checkpoint=succeeded
READING: encrypted.stop=succeeded
READING: efs.result=succeeded
READING: aim.disk_number=1
READING: aim.is_system=False
READING: aim.is_boot=False
READING: aim.logical_sector_bytes=512
READING: aim.physical_sector_bytes=512
READING: aim.bus_type=Fibre Channel
READING: system.disk_number=0
READING: result=PASS
```

## 2. 逐项结论

| 环境 | 结果 | 依据 |
|---|---|---|
| NTFS **压缩目录**上的 bundle | **支持** | 目录属性确实是 `Compressed`；create → 卷出现 → 写 1 MiB → checkpoint → stop 全成功 |
| **EFS 加密目录**上的 bundle | **支持** | `cipher /e` 成功（exit 0），属性 `Encrypted`；同样的整轮动作全成功 |
| **Defender 实时保护** | **支持** | 整个用例在 `RealTimeProtectionEnabled=True`、`AntivirusEnabled=True` 下完成 |
| **非系统盘** | **成立** | AIM 盘是 disk 1，`IsSystem=False`、`IsBoot=False`；系统盘是 disk 0 |
| **扇区大小** | 记录 | AIM 盘 `LogicalSectorSize=512`、`PhysicalSectorSize=512`；4Kn 未复现，不写进承诺 |
| 总线类型 | 记录 | AIM 盘报告 `BusType=Fibre Channel`（驱动实现如此），未据此做任何分支 |

## 3. 设计边界

- **“不支持”必须是一个明确的错误，而不是挂死。** 用例对每个子动作都有上限（create/checkpoint/
  stop 各 300 s），并且把“平台没法做”（例如没有证书时 `cipher` 失败）与“产品失败”分开记：
  前者记 `efs.result=platform-unavailable` 并继续，后者只接受 `refused`/`failed` 这类**有界**状态，
  出现 `timeout` 即判失败。
- 本次客机上 `cipher /e` 直接成功，所以 EFS 的“明确拒绝”分支没有被触发；它是为没有用户证书的
  环境准备的，代码在用例里（`$encryptedAttribute -match 'Encrypted'` 的 else 分支）。

## 4. 与矩阵的关系

`docs/operations/m5-support-matrix.md` §4 新增五行（压缩目录、EFS 目录、Defender、非系统盘、
扇区大小），每行都引用本页的读数。任务清单 R-18 的验收“矩阵更新；不支持项有明确错误而不是挂死”
由“五行 + 有界失败分支”满足。

## 5. 未覆盖

- 4Kn（4096 字节物理扇区）介质：客机上的 AIM 盘固定 512/512，没有可切换的介质。
- BitLocker 加密卷上的 bundle：与 EFS 不同层，未测；矩阵不承诺。
- 网络路径（SMB）上的 bundle：`bundle` 是本地路径参数，未测，矩阵不承诺。
