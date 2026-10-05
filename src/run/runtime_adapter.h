#pragma once

#include "src/common/result.h"
#include "src/persistence/persistence_types.h"
#include "src/run/disk_runtime.h"
#include "src/run/windows_volume.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace myrd {

// Shared production adapter for the M4 engine (design spec 4).
//
// `DiskRuntime` only ever sees a `DiskRuntimeOperations` callback struct.  The
// production composition of those callbacks used to live inside
// `run/main.cpp`, mixed with foreground-only concerns (console output, Ctrl+C,
// the foreground owner token), so the M5 worker could not reuse it.  This
// header is that composition, extracted once so the foreground CLI and the
// per-profile worker drive the *same* create/recover/checkpoint/cleanup code
// instead of two divergent implementations.
//
// The split below is the one the design spec names: a Windows volume boundary,
// an M3 runtime boundary, and a small environment boundary for the pre-runtime
// gates.  Tests inject fakes for all three; production injects
// `ProductionRuntimeAdapter` for the first two.

// Windows volume boundary: provision/verify/flush/assign/dismount/remove for
// one exact adopted volume.  Every implementation must fail closed rather than
// adopt a volume it cannot prove it owns.
class WindowsVolumeOperations {
public:
    virtual ~WindowsVolumeOperations() = default;

    virtual Result<ProductVolume> provision(std::uint64_t bytes,
                                            wchar_t letter) = 0;
    virtual Result<ProductVolume> find(wchar_t letter) = 0;
    virtual Result<void> verify(const ProductVolume& volume,
                                std::uint64_t bytes) = 0;
    virtual Result<void> flush(const ProductVolume& volume) = 0;
    virtual Result<void> assign(const ProductVolume& volume, wchar_t letter) = 0;
    virtual Result<void> remove_letter(const ProductVolume& volume) = 0;
    virtual Result<void> dismount(const ProductVolume& volume) = 0;
};

// M3 runtime boundary: the persistence/device lifecycle behind one bundle.
//
// `checkpoint()` is the M4 one-shot path the foreground run loop uses and stays
// unchanged.  The three two-phase calls are M5-only and speak in the
// DiskRuntime-level types, so neither this header nor `disk_runtime.h` has to
// include M3 headers; `ProductionRuntimeAdapter` performs the translation.
class M3RuntimeOperations {
public:
    virtual ~M3RuntimeOperations() = default;

    virtual Result<void> create_generation_zero(std::uint64_t bytes) = 0;
    virtual Result<void> recover(std::uint64_t bytes) = 0;
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual Result<DiskCheckpointResult> checkpoint() = 0;
    virtual DiskMaintenanceStats maintenance_stats() const = 0;

    virtual Result<DiskCheckpointPrepared> prepare_checkpoint(
        const DiskCheckpointPrepareRequest& request) = 0;
    virtual Result<DiskCheckpointResult> commit_checkpoint(
        const DiskCheckpointCommitRequest& request) = 0;
    virtual Result<void> abort_checkpoint(std::uint64_t native_operation_id) = 0;
};

// Gates that run before the engine touches a bundle.  They are seams rather
// than members so tests can drive the adapter without an elevated token, a
// driver, or a real filesystem.
struct RuntimeAdapterEnvironment final {
    std::function<Result<void>()> preflight;
    std::function<Result<DiskBundleKind>(const std::filesystem::path&)>
        classify_bundle;
    std::function<Result<void>(const std::filesystem::path&)>
        acquire_bundle_lock;
    std::function<Result<void>()> release_bundle_lock;
};

// The real environment: elevation plus `phdskmnt` availability, filesystem
// bundle classification, and the no-op lock pair.  The bundle writer lock is
// held by M3 itself; a second adapter-level lock would self-deadlock the
// generation-zero and AIM-start windows that take it internally.
[[nodiscard]] RuntimeAdapterEnvironment make_production_environment();

// Classifies a bundle path as empty, a manifest bundle, or unknown non-empty.
// Shared by the foreground CLI's early exit and by the adapter's own
// classify_bundle callback so both agree on what a bundle is.
[[nodiscard]] Result<DiskBundleKind> classify_bundle(
    const std::filesystem::path& bundle);

// Fails closed when the current process does not hold an elevated
// administrator token.
[[nodiscard]] Result<void> require_elevated_administrator();

struct RuntimeAdapterDependencies final {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    WindowsVolumeOperations& volumes;
    M3RuntimeOperations& m3;
    std::wstring owner_token;
    RuntimeAdapterEnvironment environment;
};

// Builds the one production `DiskRuntimeOperations` used by the foreground CLI
// and the M5 worker.  Fails when the environment is incomplete: a partially
// wired adapter would silently skip a safety gate, so it is refused outright
// rather than defaulted.
[[nodiscard]] Result<DiskRuntimeOperations> make_runtime_operations(
    RuntimeAdapterDependencies& dependencies);

// Production implementation of both runtime boundaries over the accepted M4/M3
// pieces (`M4PersistenceAdapter`, `M3AimRuntime`, `Win32VolumeAdapter`).
class ProductionRuntimeAdapter final : public WindowsVolumeOperations,
                                       public M3RuntimeOperations {
public:
    struct Config final {
        std::filesystem::path bundle;
        std::uint64_t virtual_bytes{};
        wchar_t drive_letter{};
        std::wstring owner_token;
        std::uint64_t ram_limit_bytes{};
        // The image a create must produce, when the caller already knows it.
        // The service derives the image id before the bundle exists and
        // validates the recorded evidence against it, so a create that invented
        // its own identity could never be registered.  Declared last so every
        // existing aggregate initializer keeps its meaning; the foreground M4
        // path leaves it absent and keeps generating a random one.
        std::optional<persistence::ImageUuid> image_uuid;
    };

    explicit ProductionRuntimeAdapter(Config config);
    ~ProductionRuntimeAdapter() noexcept override;
    ProductionRuntimeAdapter(const ProductionRuntimeAdapter&) = delete;
    ProductionRuntimeAdapter& operator=(const ProductionRuntimeAdapter&) = delete;

    [[nodiscard]] const Config& config() const noexcept { return config_; }

    // WindowsVolumeOperations
    Result<ProductVolume> provision(std::uint64_t bytes,
                                    wchar_t letter) override;
    Result<ProductVolume> find(wchar_t letter) override;
    Result<void> verify(const ProductVolume& volume,
                        std::uint64_t bytes) override;
    Result<void> flush(const ProductVolume& volume) override;
    Result<void> assign(const ProductVolume& volume, wchar_t letter) override;
    Result<void> remove_letter(const ProductVolume& volume) override;
    Result<void> dismount(const ProductVolume& volume) override;

    // M3RuntimeOperations
    Result<void> create_generation_zero(std::uint64_t bytes) override;
    Result<void> recover(std::uint64_t bytes) override;
    Result<void> start() override;
    Result<void> stop() override;
    Result<DiskCheckpointResult> checkpoint() override;
    DiskMaintenanceStats maintenance_stats() const override;
    Result<DiskCheckpointPrepared> prepare_checkpoint(
        const DiskCheckpointPrepareRequest& request) override;
    Result<DiskCheckpointResult> commit_checkpoint(
        const DiskCheckpointCommitRequest& request) override;
    Result<void> abort_checkpoint(std::uint64_t native_operation_id) override;

private:
    // The accepted M4/M3 pieces (`M4PersistenceAdapter`, `M3AimRuntime`,
    // `Win32VolumeAdapter`) live behind a pimpl so this header stays free of
    // M3 headers and stays cheap for the M5 worker to include.
    struct Impl;

    // Fails unless the AIM runtime exists, i.e. unless start() has established
    // the device whose physical drive number every volume operation needs.
    Result<std::uint32_t> aim_disk_number(const wchar_t* operation) const;

    Config config_;
    std::unique_ptr<Impl> impl_;
};

} // namespace myrd
