# M3 strict dynamic VHDX v1 golden layout

This fixture contract is derived directly from MS-VHDX v20240423.  The unit
test builds the sparse file from these independently frozen byte offsets; it
does not call the product parser or the later product writer.

| Structure | File offset | Size / value |
|---|---:|---:|
| File identifier signature | `0x000000` | ASCII `vhdxfile` |
| Header 1 | `0x010000` | 4096 bytes, sequence 10 |
| Header 2 | `0x020000` | 4096 bytes, sequence 11 |
| Region table 1 | `0x030000` | 65536 bytes |
| Region table 2 | `0x040000` | 65536 bytes |
| BAT region | `0x100000` | 1 MiB |
| Metadata region | `0x200000` | 1 MiB |
| Empty log region | `0x300000` | 1 MiB |
| File length | — | 4 MiB |

The metadata table contains exactly File Parameters, Virtual Disk Size,
Virtual Disk ID, Logical Sector Size and Physical Sector Size.  The disk is a
64 MiB dynamic, non-differencing image with 2 MiB payload blocks, 512-byte
logical sectors and 4096-byte physical sectors.  Its dynamic BAT has 32
entries because the final payload block precedes the first sector-bitmap slot.
