# Manifest v1 slot layout

All integers are little-endian. A slot is exactly 4096 bytes. The CRC32C
Castagnoli input is the complete byte range `0x0000..0x0FFF`, with the CRC
field `0x0018..0x001B` treated as zero. Fixed and dynamic reserved bytes must
be zero.

| Offset | Width | Field / rule |
|---:|---:|---|
| `0x0000` | 8 | Magic `MYRDRDM\0` |
| `0x0008` | 4 | Format major |
| `0x000C` | 4 | Format minor |
| `0x0010` | 4 | Slot length, exactly 4096 |
| `0x0014` | 1 | Slot index, 0=A or 1=B |
| `0x0015` | 3 | Reserved zero |
| `0x0018` | 4 | Slot CRC32C |
| `0x001C` | 4 | Reserved zero |
| `0x0020` | 16 | Image UUID |
| `0x0030` | 8 | Checkpoint generation |
| `0x0038` | 8 | Virtual bytes |
| `0x0040` | 4 | Logical sector size |
| `0x0044` | 4 | Physical sector size |
| `0x0048` | 4 | M2 block size |
| `0x004C` | 4 | Reserved zero |
| `0x0050` | 2 | Base relative-name byte length |
| `0x0052` | 2 | Journal relative-name byte length |
| `0x0054` | 4 | Reserved zero |
| `0x0058` | 256 | Base UTF-8 name area; bytes after declared length are zero |
| `0x0158` | 256 | Journal UTF-8 name area; bytes after declared length are zero |
| `0x0258` | 16 | Base VHDX file-write GUID |
| `0x0268` | 16 | Base VHDX data-write GUID |
| `0x0278` | 8 | Base expected file length |
| `0x0280` | 8 | Base generation |
| `0x0288` | 16 | Journal UUID |
| `0x0298` | 8 | Journal epoch |
| `0x02A0` | 8 | Journal committed length |
| `0x02A8` | 8 | Commit record offset |
| `0x02B0` | 8 | Commit record length |
| `0x02B8` | 4 | Commit record CRC32C |
| `0x02BC` | 4 | Reserved zero |
| `0x02C0` | 32 | Commit SHA-256 chain digest |
| `0x02E0` | 8 | Persistent index root offset |
| `0x02E8` | 8 | Persistent index root length |
| `0x02F0` | 4 | Persistent index root CRC32C |
| `0x02F4` | 4 | Reserved zero |
| `0x02F8` | 8 | Feature flags; upper 32 bits are mandatory-reader flags |
| `0x0300` | 8 | Diagnostic creation time, 100 ns units |
| `0x0308` | 8 | Diagnostic commit time, 100 ns units |
| `0x0310` | 3312 | Reserved zero through `0x0FFF` |

The relative-name lengths are `1..255`. The unused suffix of each 256-byte
name area is a dynamic reserved-zero range. An empty root is represented only
by zero offset, zero length, and zero CRC.
