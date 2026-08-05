# Journal v1 wire layout

This file freezes the Task 16 wire contract independently of the production
codec. All integers are little-endian. All physical structures and record
starts are aligned to 4096 bytes. CRC32C means Castagnoli CRC32C.

## Compatibility and common rules

- Writer version is major 1, minor 0.
- Unknown major versions and unknown record types are rejected.
- Feature/record flags use the upper 32 bits for mandatory-reader flags. Any
  unknown upper bit is rejected. Unknown lower optional bits are preserved,
  participate in CRC/digest, and otherwise have no v1 behavior.
- A known major with a later minor is readable only when all mandatory flags
  are known and every v1 reserved byte remains zero.
- `record_length` is the complete aligned physical record length.
- Header/record CRC covers every physical byte with only its own CRC field
  treated as zero.
- Physical bytes after the manifest-selected committed end are orphan tail
  and are never read by candidate validation.

## Immutable journal header (4096 bytes)

CRC input is `0x0000..0x0FFF` with `0x0014..0x0017` zero.

| Offset | Width | Field / rule |
|---:|---:|---|
| `0x0000` | 8 | Magic `MYRDRDJ\0` |
| `0x0008` | 4 | Format major |
| `0x000C` | 4 | Format minor |
| `0x0010` | 4 | Header length, exactly 4096 |
| `0x0014` | 4 | Header CRC32C |
| `0x0018` | 16 | Image UUID, non-nil |
| `0x0028` | 16 | Independent journal UUID, non-nil and not derived from image UUID |
| `0x0038` | 8 | Journal epoch, `1..UINT64_MAX-1` |
| `0x0040` | 8 | Virtual disk bytes |
| `0x0048` | 4 | Logical sector size, 512 or 4096 |
| `0x004C` | 4 | Physical sector size, 4096 |
| `0x0050` | 4 | M2 block size, 65536 |
| `0x0054` | 4 | VHDX data block size, 2097152 |
| `0x0058` | 8 | Base checkpoint generation |
| `0x0060` | 4 | Predecessor kind: 0 Genesis, 1 Checkpoint |
| `0x0064` | 4 | Reserved zero |
| `0x0068` | 8 | Predecessor journal epoch |
| `0x0070` | 16 | Predecessor journal UUID |
| `0x0080` | 32 | Predecessor checkpoint/genesis digest |
| `0x00A0` | 32 | Geometry digest |
| `0x00C0` | 32 | Base VHDX identity digest |
| `0x00E0` | 8 | Header feature flags |
| `0x00E8` | 3864 | Reserved zero through `0x0FFF` |

Genesis predecessor rules are exact: epoch 1, base generation 0, predecessor
epoch 0, nil predecessor journal UUID, and predecessor digest equal to the
genesis digest. Checkpoint predecessor rules are exact: epoch greater than 1,
predecessor epoch plus one equals current epoch, predecessor journal UUID is
non-nil and differs from the current UUID, and predecessor digest is nonzero.

## Common record header

The common header occupies `0x0000..0x0047` inside the first 4096-byte page.
The type-specific body starts at `0x0048`. Record CRC input is the complete
physical record with `0x0020..0x0023` zero.

| Offset | Width | Field / rule |
|---:|---:|---|
| `0x0000` | 8 | Magic `MYRDRCR\0` |
| `0x0008` | 4 | Format major |
| `0x000C` | 4 | Format minor |
| `0x0010` | 4 | Header-page length, exactly 4096 |
| `0x0014` | 2 | Type: 1 DATA, 2 ZERO, 3 DISCARD, 4 INDEX_NODE, 5 CHECKPOINT_COMMIT |
| `0x0016` | 2 | Reserved zero |
| `0x0018` | 8 | Complete physical record length |
| `0x0020` | 4 | Record CRC32C |
| `0x0024` | 4 | Reserved zero |
| `0x0028` | 8 | Checkpoint generation |
| `0x0030` | 8 | Nonzero uint64 operation ID; identical within one checkpoint |
| `0x0038` | 8 | Zero-based sequence within this checkpoint |
| `0x0040` | 8 | Record flags |

Physical lengths are fixed: DATA 69632 bytes; ZERO, DISCARD, INDEX_NODE and
CHECKPOINT_COMMIT 4096 bytes.

## DATA / ZERO / DISCARD body

| Offset | Width | Field / rule |
|---:|---:|---|
| `0x0048` | 8 | M2 block index |
| `0x0050` | 8 | Snapshot version, nonzero |
| `0x0058` | 8 | Source backing generation |
| `0x0060` | 8 | Dirty epoch, nonzero |
| `0x0068` | 4 | Logical payload length |
| `0x006C` | 4 | Stored payload length |
| `0x0070` | 4 | Full stored-payload CRC32C |
| `0x0074` | 3980 | Reserved zero through `0x0FFF` |
| `0x1000` | 65536 | DATA stored payload only |

For DATA, stored length is always 65536 and the physical payload always
exists. Logical length is calculated from geometry and block index, not
caller-selected. Bytes from logical length through 65535 are zero, and payload
CRC covers all 65536 bytes. ZERO and DISCARD have all three payload fields zero
and no second physical payload page range. Every block kind validates the block
index using checked arithmetic before allocation or payload access.

## INDEX_NODE body and entries

INDEX_NODE is one complete 4096-byte record. Its common record CRC is also the
node CRC mirrored by root/child descriptors; there is no second node CRC.

| Offset | Width | Field / rule |
|---:|---:|---|
| `0x0048` | 2 | Level: 0 leaf, 1..7 internal |
| `0x004A` | 2 | Entry count, `1..82` |
| `0x004C` | 2 | Entry kind: 0 leaf, 1 internal; must match level |
| `0x004E` | 2 | Entry width, exactly 48 |
| `0x0050` | 8 | Minimum key, derived from entries |
| `0x0058` | 8 | Maximum key, derived from entries |
| `0x0060` | 8 | Node generation, equal to common generation |
| `0x0068` | 24 | Reserved zero |
| `0x0080` | `count*48` | Fixed-width entries |
| dynamic | remainder | Unused entry capacity and final 32 bytes are zero |

Leaf entry (48 bytes):

| Relative offset | Width | Field / rule |
|---:|---:|---|
| `0x00` | 8 | Exact block key |
| `0x08` | 2 | Referenced type: DATA, ZERO or DISCARD |
| `0x0A` | 2 | Reserved zero |
| `0x0C` | 4 | Payload CRC; zero for ZERO/DISCARD |
| `0x10` | 8 | Referenced record offset |
| `0x18` | 8 | Referenced physical record length |
| `0x20` | 8 | Referenced generation |
| `0x28` | 8 | Reserved zero |

Internal entry (48 bytes):

| Relative offset | Width | Field / rule |
|---:|---:|---|
| `0x00` | 8 | Child minimum key |
| `0x08` | 8 | Child maximum key |
| `0x10` | 8 | Child record offset |
| `0x18` | 8 | Child record length, exactly 4096 |
| `0x20` | 4 | Child record CRC32C |
| `0x24` | 4 | Reserved zero |
| `0x28` | 8 | Child generation |

Keys/ranges are strictly increasing and non-overlapping. Header min/max equal
the first/last derived key. Leaf record and internal child descriptors are
aligned, precede the parent node, stay inside the caller committed bound, have
the exact type length, and do not use invalid generations. Task17 additionally
re-reads referenced records, verifies child level/type/CRC and detects cycles.

## CHECKPOINT_COMMIT body

CHECKPOINT_COMMIT is one complete 4096-byte record and is the final record in
the committed range.

| Offset | Width | Field / rule |
|---:|---:|---|
| `0x0048` | 16 | Image UUID |
| `0x0058` | 16 | Journal UUID |
| `0x0068` | 8 | Journal epoch |
| `0x0070` | 8 | Previous generation |
| `0x0078` | 8 | New generation; equal to common generation |
| `0x0080` | 32 | Previous checkpoint/genesis digest |
| `0x00A0` | 32 | New chain digest |
| `0x00C0` | 8 | DATA count |
| `0x00C8` | 8 | ZERO count |
| `0x00D0` | 8 | DISCARD count |
| `0x00D8` | 8 | INDEX_NODE count |
| `0x00E0` | 8 | Root offset |
| `0x00E8` | 8 | Root length |
| `0x00F0` | 4 | Root record CRC32C |
| `0x00F4` | 4 | Reserved zero |
| `0x00F8` | 8 | Checkpoint begin offset |
| `0x0100` | 8 | Journal committed end |
| `0x0108` | 32 | Geometry digest |
| `0x0128` | 8 | Number of non-commit input records |
| `0x0130` | 3792 | Reserved zero through `0x0FFF` |

The four counts add without overflow to input-record count. Common sequence
equals input-record count. Checkpoint records occupy the exact contiguous range
`[checkpoint_begin_offset, commit_offset)`. Commit offset plus 4096 equals
committed end. A nonempty root is aligned, length 4096, ends no later than the
commit, and may reuse an older immutable node. Empty root is exactly zero
offset/length/CRC.

Generation zero is the sole `0 -> 0` commit: begin 4096, no input records,
empty root, and chain digest equal to the header genesis digest. Every other
commit uses checked `G -> G+1` and cannot produce the invalid generation
sentinel.

## Canonical SHA-256 inputs

Domains are exact ASCII bytes without a trailing NUL.

```text
geometry_digest = SHA256(
  "MYRD-GEOMETRY-V1" ||
  LE64(virtual_bytes) || LE32(logical_sector_size) ||
  LE32(physical_sector_size) || LE64(65536) || LE32(2097152))

base_identity_digest = SHA256(
  "MYRD-BASE-ID-V1" ||
  file_write_guid_raw_16 || data_write_guid_raw_16)

genesis_digest = SHA256(
  "MYRD-GENESIS-V1" || image_uuid || geometry_digest ||
  LE64(0) || base_identity_digest)

checkpoint_chain_digest = SHA256(
  "MYRD-CHECKPOINT-V1" || previous_digest || image_uuid ||
  LE64(journal_epoch) || LE64(previous_generation) ||
  LE64(new_generation) || geometry_digest ||
  each non-commit record's complete physical bytes in sequence order ||
  complete commit bytes with only record CRC and new chain digest zero)
```

Non-commit record CRCs, flags and padding remain present in digest input. To
publish a commit: encode with CRC/digest zero; calculate chain digest; write
chain digest; calculate full commit CRC with only record CRC zero; write CRC.
Validation reverses those steps after first validating the final record CRC.

## Stateful checkpoint sequence validation

Single-record decode validates only one bounded record. The streaming sequence
validator additionally enforces contiguous offsets, zero-based sequence,
common nonzero operation ID, generation consistency, unique block updates,
block records before index nodes, exactly one final commit, exact counts,
terminal committed end, root range, predecessor relation and streaming chain
digest. It owns only bounded metadata/dedup state and never buffers the journal
or DATA payloads.
