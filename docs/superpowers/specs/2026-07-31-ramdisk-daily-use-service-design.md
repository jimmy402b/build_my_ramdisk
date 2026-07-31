# RamDisk CLI — Daily-Use Service Design Spec

> **Status:** Approved working specification for the next implementation plan  
> **Date:** 2026-07-31  
> **Repository:** `C:\Users\JimmyLin\Desktop\build_my_ramdisk`  
> **Target milestone:** Phase 3 — reliable single-disk daily-use service  
> **Product intent:** deliver a personal RAM-disk experience close to Primo Ramdisk while reusing the already installed, signed Arsenal Image Mounter driver.

## 1. Authority and scope

This document is the authoritative specification for the next phase. Where it conflicts with `PROJECT_STATUS.md`, `CLAUDE.md`, or either 2026-07-31 predecessor spec, this document controls the next implementation plan. Earlier documents remain historical evidence.

The phase converts the verified AIM `vm` prototype into a safe single-profile Windows service. It does not claim to reproduce Primo's kernel-level dynamic memory management. It establishes the service, storage, recovery, and test boundaries needed before a tray GUI or any future DMM work.

## 2. Evidence-based baseline

The current repository proves the following:

- `ramdisk-cli.exe` builds and its `help` and `version` commands run.
- AIM VM mode can mount a raw image as an Arsenal virtual disk.
- An NTFS volume can be written, saved from `PhysicalDriveN`, unmounted, remounted, and read back.
- `StorageServer`, `DirtyBitmap`, and `SectorCache` pass internal logic checks, but the installed AIM Free Mode driver does not connect to the third-party proxy pipe.

The current source also contains release-blocking behavior:

- `g_deviceNumber` remains at its default value, so save/unmount may target AIM device 0.
- `AssignDriveLetter()` identifies a disk by capacity instead of mount identity.
- create, mount, and save allocate whole-disk byte vectors; a 32 GiB profile can require another 32 GiB in the user process.
- raw image creation uses a single `WriteFile` length represented by `DWORD`, which is not valid for a 32 GiB transfer.
- VHD numeric fields and BAT entries are written in host little-endian order instead of VHD big-endian order.
- `DoSave()` flushes every mounted fixed/removable volume rather than the managed target volume.
- shutdown persistence relies on a console control callback instead of Windows Service preshutdown handling.
- there is no automated test target.

These findings define the mandatory work; the implementation plan may not omit them.

## 3. Design decision

### 3.1 Selected approach: AIM VM plus a Windows service

Use the installed `aim_ll.exe -t vm` path and `phdskmnt.sys`. A LocalSystem service owns the single profile, expands a standard VHD to a temporary sparse raw image in bounded chunks, mounts it through AIM, and captures durable VHD snapshots on schedule and shutdown.

This route is selected because it is executable on the current machine without a new driver, test signing, BCD changes, or certificate work.

### 3.2 Rejected for this phase

| Approach | Decision | Reason |
|---|---|---|
| Third-party AIM proxy backend | Deferred | Installed AIM Free Mode rejects the connection; internal server tests are not driver E2E evidence. |
| New kernel driver | Rejected | Requires driver engineering, signing, HVCI validation, crash-dump analysis, and a separate security program. |
| GUI-first development | Deferred | A GUI would expose an unsafe lifecycle while device identity, bounded I/O, service shutdown, and recovery remain unresolved. |

## 4. Product outcomes

At phase exit, one configured RAM disk shall:

1. mount automatically when the Windows service starts;
2. use a configured drive letter and NTFS volume label;
3. run from AIM virtual memory rather than directly writing through to the VHD;
4. save on a configured interval, on explicit request, and during service preshutdown;
5. retain the last known-good image if a save is interrupted;
6. be controlled and diagnosed through a stable CLI without the CLI touching AIM or `PhysicalDriveN` directly;
7. never require a whole-disk user-mode memory buffer;
8. report honestly that capacity is fixed and DMM is unavailable.

## 5. Non-goals and protected resources

- `NG-001`: no proxy mode in the production service target.
- `NG-002`: no dynamic allocation/release of physical RAM and no DMM claim.
- `NG-003`: no multiple profiles, tray GUI, installer UI, remote control, telemetry, or networking.
- `NG-004`: no installation or replacement of AIM drivers and no `.sys`, `.inf`, or `.cat` payload.
- `NG-005`: no changes to test signing, BCD, Secure Boot, HVCI, or driver services.
- `NG-006`: no automatic access, conversion, deletion, or mounting of `C:\PR-Image-Z.vdf` or the existing Z: volume.
- `NG-007`: no destructive conversion of legacy little-endian project VHD files; conversion must always write a new destination.

## 6. Requirements

### 6.1 Build and repository structure

- `BLD-001`: refactor reusable code into a static `ramdisk_core` target.
- `BLD-002`: build separate `ramdisk-cli.exe`, `ramdisk-service.exe`, and `ramdisk-tests.exe` targets.
- `BLD-003`: enable CTest and register unit/integration test executables with labels `unit`, `integration`, and `aim-e2e`.
- `BLD-004`: retain C++11 and MinGW-w64 compatibility; production code may not depend on MSVC-only C++ extensions.
- `BLD-005`: vendor `nlohmann/json` 3.11.3 as a pinned MIT single header for config, IPC, and test fixtures. The source URL, version, license, and SHA-256 must be recorded in `THIRD_PARTY.md`. Any GitHub retrieval must use the user's port 7890 proxy.

### 6.2 Configuration

- `CFG-001`: store the single profile at `%ProgramData%\MyRamDisk\profile.json` using UTF-8 JSON.
- `CFG-002`: write configuration through `<path>.tmp`, flush it, then replace with `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
- `CFG-003`: reject unknown schema versions; unknown fields within schema version 1 are ignored and preserved during a read-modify-write cycle.
- `CFG-004`: `imagePath` must be an absolute path on a local fixed NTFS volume and must not equal or resolve through a reparse point to a protected resource.
- `CFG-005`: `driveLetter` is one uppercase letter D–Z; an occupied letter blocks startup and is never reassigned automatically.
- `CFG-006`: `sizeBytes` is at least 64 MiB and is divisible by 2 MiB and 512 bytes.
- `CFG-007`: `saveIntervalSeconds` is `0` or in the inclusive range 30–86400.

The version 1 profile schema is fixed as:

```json
{
  "schemaVersion": 1,
  "profileId": "primary",
  "profileGuid": "de305d54-75b4-431b-adb2-eb6b9e546014",
  "imagePath": "C:\\RamDiskData\\primary.vhd",
  "sizeBytes": 34359738368,
  "driveLetter": "Z",
  "volumeLabel": "RAMDISK",
  "saveIntervalSeconds": 300,
  "autoStart": true,
  "saveOnShutdown": true,
  "keepPreviousImage": true
}
```

### 6.3 VHD engine and legacy conversion

- `VHD-001`: all VHD footer, dynamic-header, geometry, checksum, timestamp, size, table-offset, block-size, table-count, and BAT values use VHD-specified big-endian representation.
- `VHD-002`: checksum calculations operate on serialized on-disk bytes with the checksum field cleared.
- `VHD-003`: dynamic VHD block writes are write-through and bounded; the engine may retain at most two 2 MiB blocks in memory.
- `VHD-004`: `ReadSectors` and `WriteSectors` accept `uint64_t` sector offsets/counts internally and split Win32 I/O into requests no larger than 64 MiB.
- `VHD-005`: a completed file must contain a valid footer copy at offset 0 and a matching footer at EOF.
- `VHD-006`: `Validate()` checks cookies, checksums, disk type, table bounds, BAT bounds, duplicate BAT mappings, file length, and declared virtual size before mount.
- `VHD-007`: detect the repository's legacy little-endian images only when the cookie is valid, big-endian decoding is invalid, host-endian decoding is internally consistent, and `creatorApp` is `RDSK`; return a distinct `LegacyFormat` result. `creatorApp` alone is never sufficient.
- `VHD-008`: implement `image convert-legacy --input <old> --output <new>` as streamed, non-in-place conversion; the source remains byte-identical on both success and failure.
- `VHD-009`: release validation requires the generated VHD to be accepted by an independent parser on the target machine (`Get-VHD` when available, otherwise AIM CLI/DiscUtils validation recorded as evidence).

Required public surface:

```cpp
enum class VhdStatus { Ok, InvalidFormat, LegacyFormat, IoError, OutOfRange };

struct VhdValidationResult {
    VhdStatus status;
    uint64_t virtualSizeBytes;
    std::string message;
};

class VhdEngine {
public:
    VhdStatus CreateDynamic(const std::wstring& path, uint64_t sizeBytes);
    VhdStatus Open(const std::wstring& path, bool writable);
    VhdStatus ReadSectors(uint64_t firstSector, uint64_t count, void* buffer);
    VhdStatus WriteSectors(uint64_t firstSector, uint64_t count, const void* buffer);
    VhdStatus Flush();
    VhdValidationResult Validate() const;
    void Close();
};
```

### 6.4 Bounded streaming and snapshots

- `IO-001`: all VHD↔raw and PhysicalDrive→VHD transfers use one reusable 32 MiB buffer; no production path allocates a buffer proportional to profile capacity.
- `IO-002`: raw staging files are marked sparse before data is written and use a unique profile GUID plus operation GUID in their filename.
- `IO-003`: every Win32 read/write loop handles partial transfers, cancellation, EOF, and offsets greater than 4 GiB.
- `IO-004`: AIM must finish loading the raw file before it is deleted; deletion failure is logged and retried during next safe cleanup.
- `SAV-001`: a save writes to `<imagePath>.next`; it never mutates the active image in place.
- `SAV-002`: after closing and flushing `.next`, run full VHD structural validation and deterministic sample reads at sector 0, each 2 MiB boundary selected by a seeded sampler, and the final sector.
- `SAV-003`: if validation succeeds, atomically rotate active to `<imagePath>.previous` and `.next` to active. If any rotation step fails, restore the previous active name before reporting failure.
- `SAV-004`: keep at most one previous generation and never delete the only validated image.
- `SAV-005`: only one save may execute. Requests arriving during a save coalesce into exactly one additional save.
- `SAV-006`: flush only the managed volume handle (`\\.\<letter>:`) before capture; never enumerate or flush unrelated volumes.
- `SAV-007`: each save records operation ID, reason, start/end UTC, bytes read, elapsed time, image generation, and result code.

Required interfaces:

```cpp
struct SnapshotResult {
    bool success;
    GUID operationId;
    uint64_t bytesRead;
    uint64_t elapsedMilliseconds;
    std::string code;
    std::string message;
};

class SnapshotEngine {
public:
    SnapshotResult ExpandVhdToSparseRaw(const ProfileConfig&, const std::wstring& rawPath,
                                        CancellationToken&);
    SnapshotResult CaptureDeviceToNextVhd(const ProfileConfig&, const MountIdentity&,
                                          CancellationToken&);
    SnapshotResult CommitNextGeneration(const ProfileConfig&);
};

struct CancellationToken {
    const std::atomic<bool>* requested;
    bool IsCancellationRequested() const {
        return requested != nullptr && requested->load();
    }
};
```

### 6.5 Exact AIM device identity

- `DEV-001`: `MountVm()` returns a structured result containing AIM device number and Windows physical-drive path; no caller relies on a default device number.
- `DEV-002`: take AIM device snapshots immediately before and after mount and require exactly one new device matching AIM stdout and expected capacity.
- `DEV-003`: resolve the new device's disk number, partition, volume GUID, volume serial, MBR signature or GPT disk GUID, and mount path through volume extents.
- `DEV-004`: drive-letter assignment targets the resolved volume GUID; capacity matching and global disk scans that choose the first equal-size disk are forbidden.
- `DEV-005`: unmount requires a current `MountIdentity` whose profile GUID, device number, physical drive, volume GUID, volume serial, disk identifier, and configured drive letter all match live observations.
- `DEV-006`: if identity cannot be proven, enter `Blocked`; never fall back to device 0, first AIM device, capacity match, or blind mount-point removal.
- `DEV-007`: all process execution uses Unicode paths, correctly quoted arguments, bounded output capture, cancellation, and a mount timeout of 600 seconds.

```cpp
struct MountIdentity {
    GUID profileGuid;
    int aimDeviceNumber;
    uint32_t windowsDiskNumber;
    std::wstring physicalDrivePath;
    std::wstring volumeGuidPath;
    uint32_t volumeSerial;
    uint32_t mbrSignature;
    GUID gptDiskGuid;
    wchar_t driveLetter;
    uint64_t diskSizeBytes;
};

struct AimMountResult {
    bool success;
    MountIdentity identity;
    std::string code;
    std::string message;
};
```

### 6.6 Provisioning and filesystem

- `PRV-001`: `configure` only writes a validated profile; it does not touch disks.
- `PRV-002`: `initialize` creates a new standard VHD at a path that does not exist, mounts it as a new AIM VM disk, verifies exact identity, then initializes only that verified disk.
- `PRV-003`: provisioning creates one MBR primary partition aligned at 1 MiB, formats NTFS with quick format, sets the configured volume label, and assigns the configured letter.
- `PRV-004`: if `diskpart.exe` is used, generate a per-operation script selecting the exact `PhysicalDriveN` disk number, capture its output, enforce a timeout, and verify the resulting partition/volume through Win32 APIs. DiskPart exit code alone is insufficient evidence.
- `PRV-005`: `initialize` refuses an existing destination image, any pre-existing AIM device ambiguity, any occupied target letter, and any disk that was present before the operation.
- `PRV-006`: after provisioning, perform a first snapshot so the active VHD contains the partition and NTFS filesystem before reporting success.

### 6.7 Session state machine

The only runtime states are:

```text
Unconfigured → Stopped → Starting → Mounted → Saving
                              │          │        │
                              └──────────┴────────┘
Mounted/Saving → Stopping → Stopped
Any state → Blocked or Failed
```

- `STA-001`: legal transitions are explicit and tested; repeated `start`, `save`, and `stop` requests are idempotent where safe.
- `STA-002`: `Starting` failure cleans only resources created by that operation and returns to `Stopped` when cleanup is proven; otherwise it enters `Blocked`.
- `STA-003`: save failure returns to `Mounted`, retains the last validated image, and records data-at-risk status.
- `STA-004`: `Stopping` performs a save unless both `discardUnsaved` and `force` are true.
- `STA-005`: runtime identity is atomically stored at `%ProgramData%\MyRamDisk\runtime.json` after mount and after every state transition.
- `STA-006`: on service restart, reconcile runtime state with live AIM/disk/volume identity. Exact match adopts the session; mismatch enters `Blocked` and performs no destructive action.

### 6.8 Windows service

- `SVC-001`: `ramdisk-service.exe` runs as LocalSystem under service name `MyRamDisk`, display name `My RAM Disk Service`, start type `SERVICE_AUTO_START`, with dependency `phdskmnt`.
- `SVC-002`: service installation uses `CreateServiceW`, configures 5-second restart recovery actions, and sets a 600-second preshutdown timeout.
- `SVC-003`: the service accepts `STOP`, `SHUTDOWN`, and `PRESHUTDOWN`; handlers only signal worker state and never perform blocking disk I/O on the SCM callback thread.
- `SVC-004`: startup validates/reconciles first, then auto-starts the profile if configured. A blocked profile leaves the service running for diagnosis.
- `SVC-005`: preshutdown rejects new state-changing IPC requests, waits for an active save, performs at most one final save, then unmounts only if identity remains proven.
- `SVC-006`: preshutdown has a hard 600-second deadline. On timeout, retain the last validated image, log an explicit incomplete-save event, and allow Windows shutdown to continue.
- `SVC-007`: Windows SCM owns restart behavior; production code must not use `TerminateProcess` as a watchdog strategy.

### 6.9 IPC and CLI

- `IPC-001`: use one local named pipe `\\.\pipe\MyRamDisk.Control.v1` in message mode.
- `IPC-002`: each message is a 4-byte little-endian payload length followed by UTF-8 JSON, limited to 1 MiB.
- `IPC-003`: every request contains `schemaVersion`, `requestId`, `command`, and optional `arguments`; every response echoes `requestId` and contains `success`, `code`, `message`, `operationId`, and optional `data`.
- `IPC-004`: pipe ACL permits LocalSystem, Administrators, and local authenticated users to connect. The server authorizes `status`/`doctor` for authenticated users and all state changes only for Administrators/System using client impersonation.
- `IPC-005`: malformed, oversized, unauthorized, duplicate, and incompatible-version requests return stable error codes and cannot terminate the service.
- `IPC-006`: cache the most recent 256 completed responses for 10 minutes by `requestId`; an identical duplicate returns the cached response, while reuse of an ID with different content returns `DuplicateRequestConflict`.
- `CLI-001`: the CLI is a pure client after this phase; it never launches `aim_ll`, opens `PhysicalDriveN`, assigns a drive letter, or writes profile/runtime files directly.
- `CLI-002`: `--json` produces one JSON document on stdout; human-readable mode sends errors to stderr.
- `CLI-003`: exit codes are fixed: `0` success, `1` argument/schema error, `2` configuration/image error, `3` AIM/device error, `4` service/IPC error, `5` access denied, `6` operation blocked/timeout.

Required commands:

```text
ramdisk-cli configure --image <path> --size <size> --mount-point <X:>
                     --label <label> --save-interval <seconds> [--auto-start]
ramdisk-cli initialize
ramdisk-cli start
ramdisk-cli save
ramdisk-cli stop [--discard-unsaved --force]
ramdisk-cli status [--json]
ramdisk-cli doctor [--json]
ramdisk-cli image validate --input <path> [--json]
ramdisk-cli image convert-legacy --input <old> --output <new>
ramdisk-cli service install|uninstall|start|stop|status
```

Legacy commands remain for one compatibility release and never bypass the service:

- `create -s <size> -o <path> ...` translates to `configure` followed by `initialize` and preserves the old defaults (`RAMDISK`, 300 seconds, first configured letter required through `--mount-point`). Missing `--mount-point` is now an explicit argument error.
- `mount -i <path>` is accepted only when `<path>` normalizes to the configured `imagePath`, then translates to `start`; otherwise it returns `ProfileMismatch` and instructs the user to run `configure`.
- `unmount` translates to `stop`; the old `--no-save` spelling is accepted only together with `--force` and maps to `--discard-unsaved --force`.
- `status` translates directly to the service `status` request.

Every legacy invocation prints a deprecation notice to stderr in human mode and includes `deprecated: true` in JSON mode.

### 6.10 Logging and observability

- `OBS-001`: write UTF-8 JSON Lines logs to `%ProgramData%\MyRamDisk\logs\service.jsonl`.
- `OBS-002`: each record includes UTC timestamp, severity, event name, profile GUID, request/operation ID, state, code, message, and safe numeric metrics.
- `OBS-003`: rotate at 10 MiB, retain 10 files, and flush Error/Critical and save-completion records.
- `OBS-004`: do not log file contents, arbitrary environment variables, access tokens, full command lines, or user document names.
- `OBS-005`: `doctor` reports service status, AIM executable/version evidence, driver service state, profile validation, live identity reconciliation, disk space, last save, and last error without changing state.

### 6.11 Performance and safety budgets

- `PERF-001`: service-side working buffers for a 32 GiB profile are at most 128 MiB total; process private bytes attributable to snapshot/provisioning are below 512 MiB.
- `PERF-002`: all long operations expose progress at least every 5 seconds through status and logs.
- `PERF-003`: cancellation is checked between I/O chunks; cancellation leaves active image and live mount unchanged unless the caller separately requests stop.
- `SAFE-001`: no operation infers ownership from capacity alone, drive letter alone, or list position.
- `SAFE-002`: paths use Win32 Unicode APIs and support long paths; command lines are constructed with Windows quoting rules.
- `SAFE-003`: destructive provisioning and `--discard-unsaved` are denied unless all listed preconditions are proven at execution time.
- `SAFE-004`: tests and default configuration never use Z:, `C:\PR-Image-Z.vdf`, or an existing production image.

## 7. Component and file ownership

The implementation plan shall use these boundaries unless a change is explicitly justified in the plan:

| File/target | Responsibility |
|---|---|
| `src/CMakeLists.txt` | `ramdisk_core`, CLI, service, and tests target wiring. |
| `src/profile_config.h/.cpp` | Schema, validation, atomic profile store, protected-path checks. |
| `src/control_protocol.h/.cpp` | IPC request/response schema, framing, stable error codes. |
| `src/service_host.h/.cpp` | SCM registration, service control handler, worker lifetime. |
| `src/service_main.cpp` | Minimal service executable entry point. |
| `src/ramdisk_session.h/.cpp` | State machine, start/save/stop/reconcile orchestration. |
| `src/runtime_store.h/.cpp` | Atomic runtime identity/state persistence. |
| `src/snapshot_engine.h/.cpp` | Sparse raw expansion, bounded capture, validation, image rotation. |
| `src/operation_context.h` | Shared cancellation token and operation/progress identifiers. |
| `src/save_coordinator.h/.cpp` | Interval timer, serialization, coalescing, shutdown deadline. |
| `src/device_identity.h/.cpp` | Disk/partition/volume discovery and exact ownership checks. |
| `src/volume_manager.h/.cpp` | Verified partitioning, formatting, label, and mount-point operations. |
| `src/aim_bridge.h/.cpp` | Unicode AIM process bridge returning structured mount/device data. |
| `src/vhd_engine.h/.cpp` | Standards-compliant, bounded-memory dynamic VHD implementation. |
| `src/cli_parser.h/.cpp` | New command tree, validation, help, and exit-code mapping. |
| `src/main.cpp` | Thin CLI→IPC client only. |
| `src/json_log.h/.cpp` | JSONL logging and rotation. |
| `tests/test_support.h` | Dependency-free test registration/assertion helpers. |
| `tests/*_tests.cpp` | Unit/integration tests grouped by owning component. |
| `scripts/Run-AimE2E.ps1` | Explicitly gated, reversible real-AIM acceptance workflow. |
| `THIRD_PARTY.md` | Pinned nlohmann/json provenance and license. |

`storage_server`, `dirty_bitmap`, `sector_cache`, `watchdog`, and the current console `daemon` remain in the repository as experimental code but are excluded from the production service target. No production path may call them in this phase.

## 8. Verification strategy

### 8.1 Unit tests

- `TST-001`: big-endian footer/header/BAT golden-byte tests and checksum tests.
- `TST-002`: sector 0, 2 MiB boundary, cross-block, final-sector, sparse-block, truncated-file, duplicate-BAT, and out-of-range tests.
- `TST-003`: simulated >4 GiB streaming with a fake seekable device; assert no I/O request exceeds 64 MiB and observed working buffer remains bounded.
- `TST-004`: profile schema, unknown-field preservation, path normalization, reparse-point rejection, occupied letter, and protected-resource tests.
- `TST-005`: before/after AIM list reconciliation, ambiguous-device refusal, exact identity match/mismatch, and no-device-0 fallback tests.
- `TST-006`: every legal and illegal state transition, idempotency, startup cleanup, save failure, blocked reconciliation, and forced-discard requirements.
- `TST-007`: save coalescing, timer cancellation, preshutdown deadline, active/next/previous rotation, and crash-point recovery tests.
- `TST-008`: IPC framing, 1 MiB limit, schema mismatch, malformed JSON, authorization matrix, duplicate request ID, and stable exit-code mapping.

### 8.2 Integration tests without AIM mutation

- `TST-009`: run service orchestration against fake AIM, fake block device, fake volume manager, and a temporary image directory.
- `TST-010`: terminate the test process at each snapshot commit boundary and verify recovery selects exactly one validated active generation.
- `TST-011`: service restart reconciliation adopts exact identity and blocks every mismatch case without invoking unmount.

### 8.3 Real AIM acceptance

- `E2E-001`: real tests require an elevated terminal, explicit per-run user approval, a new 256 MiB image, and a confirmed unused non-Z drive letter.
- `E2E-002`: capture before/after AIM device lists, Windows disks/volumes, Z: metadata, and `C:\PR-Image-Z.vdf` metadata.
- `E2E-003`: initialize, write deterministic files, save, stop, remount, and compare SHA-256 for every test file.
- `E2E-004`: verify interval save generation changes and previous-generation retention.
- `E2E-005`: verify service stop/start reconciliation. Windows reboot verification is a separate action requiring a second explicit approval.
- `E2E-006`: cleanup only the test profile, service state created by the test, its test letter, and its test images; verify protected resources are unchanged.

## 9. Milestone gates

| Gate | Evidence required |
|---|---|
| `G0 — test foundation` | CTest executes; all existing/new non-AIM tests are discoverable; no test uses protected resources. |
| `G1 — VHD correctness` | `VHD-001`–`VHD-009` and `TST-001`–`TST-003` pass; independent parser accepts an output image. |
| `G2 — exact identity` | Fake reconciliation matrix passes; code search shows no device-0/capacity fallback in production paths. |
| `G3 — service lifecycle` | Fake end-to-end configure→initialize→start→save→stop and restart reconciliation pass. |
| `G4 — durability` | Fault-injection at every snapshot boundary always leaves at least one validated image. |
| `G5 — controlled AIM acceptance` | `E2E-001`–`E2E-006` pass with recorded approval and before/after evidence. |

No gate may be marked complete from compilation alone.

## 10. Phase completion criteria

The phase is complete only when all of the following are evidenced:

- `ACC-001`: a 32 GiB simulated profile performs expand/capture with bounded buffers and without whole-disk allocation.
- `ACC-002`: all VHD structural, compatibility, corruption, and legacy-conversion tests pass.
- `ACC-003`: all device selection and unmount paths require exact `MountIdentity` proof.
- `ACC-004`: service installation, automatic startup, IPC authorization, interval saving, preshutdown, and recovery are implemented and tested.
- `ACC-005`: real AIM acceptance preserves every deterministic test file across save/stop/remount.
- `ACC-006`: interrupted saves retain at least one validated image and report data-at-risk state accurately.
- `ACC-007`: CLI and documentation label the product `Fixed-capacity AIM VM mode`; no DMM/proxy production claim remains.
- `ACC-008`: Z:, `C:\PR-Image-Z.vdf`, AIM driver installation, and boot/security configuration remain unchanged.
- `ACC-009`: the implementation plan's traceability table maps every `BLD`, `CFG`, `VHD`, `IO`, `SAV`, `DEV`, `PRV`, `STA`, `SVC`, `IPC`, `CLI`, `OBS`, `PERF`, `SAFE`, `TST`, `E2E`, and `ACC` requirement to one or more concrete tasks and verification commands.

## 11. Follow-on specifications

Only after `G5`:

1. tray GUI and profile editor;
2. multiple independently owned RAM disks;
3. migration assistant for copied user data;
4. DMM research, contingent on a connectable proxy path or an independently approved kernel design.
