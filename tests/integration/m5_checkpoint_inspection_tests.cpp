// Task 3 addendum (2026-10-02 solution, §1): the read-only recovery-side
// inspection the M5 recovery proof is built on.
//
// Every case here goes through `M3Provider::inspect_checkpoint`, which is the
// only entry point Task 5 is allowed to use: the worker must never open a
// bundle, a manifest or a journal itself.  The inspection answers exactly one
// question -- what does this bundle prove about the operation whose durable
// correlation the service recorded -- and the cases below are the ways that
// answer must fail closed.

#include "test_framework.h"

#include "src/common/m5_native_operation_id.h"
#include "src/m3/m3_provider.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/vhdx_reader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::m3;
using namespace myrd::persistence;

class TempBundle final {
public:
    TempBundle() {
        root = std::filesystem::temp_directory_path() /
            (L"myrd-m5-checkpoint-inspection-" + std::to_wstring(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    ~TempBundle() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

OperationContext context() {
    return {{}, std::chrono::steady_clock::now() + std::chrono::seconds(10)};
}

PersistenceLimits limits() {
    constexpr std::uint64_t kMiB = 1024 * 1024;
    return {64 * kMiB, 64 * kMiB, 8192,
            64 * kIndexNodeBytes, 64,
            64 * kVhdxBatPageBytes, 64,
            2 * kMiB, 2 * kMiB};
}

DynamicBlockStoreOptions store_options() {
    return {2 * kM2BlockSize, 4 * kM2BlockSize, kM2BlockSize,
            2 * kM2BlockSize, 16, 64,
            4 * kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250}};
}

M3ProviderConfig config(const TempBundle& bundle) {
    return {bundle.root, "disk.rdm", {limits()}, store_options(),
            std::chrono::milliseconds{5000}};
}

// Generation zero is created with native operation id 3901, which is
// deliberately *not* the id any M5 two-phase commit produces.  A case that
// expects 3903 therefore fails if the inspection ever reported the prior
// generation's id instead of the committed candidate's.
void create_bundle(const TempBundle& bundle) {
    GenerationZeroCreateRequest request{};
    request.bundle_root = bundle.root;
    request.manifest_relative_name = "disk.rdm";
    request.base_temporary_name = "base.create.tmp";
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {8 * 1024 * 1024, 512, 4096};
    request.image_uuid.bytes[0] = std::byte{0x11};
    request.base_identity.file_identity.file_write_guid[0] = std::byte{0x21};
    request.base_identity.file_identity.data_write_guid[0] = std::byte{0x22};
    request.base_identity.virtual_disk_id[0] = std::byte{0x23};
    request.journal_identity.journal_uuid[0] = std::byte{0x31};
    request.journal_epoch = 1;
    request.operation_id = 3901;
    request.created_time_100ns = 1;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.recovery = {limits()};
    auto created = create_generation_zero_image(OperationContext{}, request);
    REQUIRE(created.ok());
}

struct TransportProbe final {
    IBlockDevice* device{};
};

class CapturingTransport final : public IM3ProviderTransport {
public:
    CapturingTransport(IBlockDevice& device, std::shared_ptr<TransportProbe> probe)
        : probe_(std::move(probe)) {
        probe_->device = &device;
    }
    Result<void> start() override { return Result<void>::success(); }
    Result<void> stop() override { return Result<void>::success(); }
    proxy::SessionSnapshot snapshot() const override { return {}; }
    proxy::ProxyObjectNames names() const override { return {}; }

private:
    std::shared_ptr<TransportProbe> probe_;
};

M3ProviderFactories factories(const std::shared_ptr<TransportProbe>& probe) {
    M3ProviderFactories result{};
    result.transport = [probe](IBlockDevice& device,
                               const proxy::IProxyNamespacePolicy&,
                               proxy::ShmProxySessionOptions) {
        std::unique_ptr<IM3ProviderTransport> transport =
            std::make_unique<CapturingTransport>(device, probe);
        return Result<std::unique_ptr<IM3ProviderTransport>>::success(
            std::move(transport));
    };
    return result;
}

constexpr std::uint64_t kCandidateGeneration = 1;
constexpr std::uint64_t kCandidateNativeId =
    kCandidateGeneration + myrd::kM5CheckpointOperationOffset;

// Everything one prepare/commit freezes, in the shape the service persists at
// P4 and later hands to a fresh worker.
CheckpointRecoveryCorrelation correlation(const M3CheckpointPrepared& prepared) {
    CheckpointRecoveryCorrelation expected{};
    expected.image_uuid.bytes[0] = std::byte{0x11};
    expected.prior_generation = prepared.prior_generation;
    expected.candidate_generation = prepared.candidate_generation;
    expected.native_operation_id = kCandidateNativeId;
    expected.manifest_slot = prepared.manifest_slot;
    expected.candidate_bytes_digest = prepared.candidate_bytes_digest;
    return expected;
}

M3CheckpointPrepareRequest prepare_request() {
    M3CheckpointPrepareRequest request{};
    request.context = context();
    request.barrier = VolumeBarrierKind::RawBlockTestNoFilesystemCache;
    request.native_operation_id = kCandidateNativeId;
    return request;
}

M3CheckpointCommitRequest commit_request(
    const M3CheckpointPrepared& prepared) {
    M3CheckpointCommitRequest request{};
    request.context = context();
    request.native_operation_id = kCandidateNativeId;
    request.manifest_slot = prepared.manifest_slot;
    request.candidate_bytes_digest = prepared.candidate_bytes_digest;
    return request;
}

// Runs prepare + commit against a freshly created bundle and leaves the bundle
// fully closed, which is the state a recovery proof is always taken in.
M3CheckpointPrepared commit_one_generation(const TempBundle& bundle) {
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x5a});
    REQUIRE(probe->device->write(context(), 0, data).ok());
    const auto prepared = provider.value()->prepare_checkpoint(prepare_request());
    REQUIRE(prepared.ok());
    REQUIRE_EQ(prepared.value().candidate_generation, kCandidateGeneration);
    REQUIRE(provider.value()->commit_checkpoint(commit_request(prepared.value())).ok());
    REQUIRE(provider.value()->stop(context(), {}).ok());
    return prepared.value();
}

Result<CheckpointRecoveryInspection> inspect(
    const TempBundle& bundle,
    const CheckpointRecoveryCorrelation& expected) {
    return M3Provider::inspect_checkpoint(context(), config(bundle), expected);
}

std::uint32_t bit(const CheckpointRecoveryMismatch value) {
    return static_cast<std::uint32_t>(value);
}

// A whole-directory content fingerprint, used to prove the inspection wrote
// nothing: names, sizes and the SHA-256 of every byte on disk.
std::string bundle_fingerprint(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::string fingerprint;
    for (const auto& path : files) {
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
        auto digest = sha256_hex(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
        REQUIRE(digest.ok());
        fingerprint += path.filename().string();
        fingerprint += ':';
        fingerprint += std::to_string(bytes.size());
        fingerprint += ':';
        fingerprint += digest.value();
        fingerprint += '\n';
    }
    return fingerprint;
}

} // namespace

TEST_CASE(m5_checkpoint_inspection_proves_a_committed_candidate) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto prepared = commit_one_generation(bundle);

    auto result = inspect(bundle, correlation(prepared));
    REQUIRE(result.ok());
    const auto& inspection = result.value();
    REQUIRE_EQ(inspection.verdict,
               CheckpointRecoveryVerdict::CandidateGenerationProved);
    REQUIRE_EQ(inspection.candidate_mismatch_mask, 0u);
    // The observation is the bundle's own account, field by field.
    REQUIRE(inspection.image_uuid.bytes[0] == std::byte{0x11});
    REQUIRE_EQ(inspection.base_generation, 0ull);
    REQUIRE_EQ(inspection.candidate_generation, kCandidateGeneration);
    REQUIRE_EQ(inspection.journal_operation_id, kCandidateNativeId);
    REQUIRE_EQ(inspection.selected_manifest_slot, prepared.manifest_slot);
    REQUIRE(inspection.selected_manifest_digest ==
            prepared.candidate_bytes_digest);
}

TEST_CASE(m5_checkpoint_inspection_proves_the_prior_generation_after_abort) {
    TempBundle bundle;
    create_bundle(bundle);
    proxy::LocalNamespacePolicy policy;
    auto probe = std::make_shared<TransportProbe>();
    auto provider = M3Provider::open_for_test(
        context(), policy, config(bundle), factories(probe));
    REQUIRE(provider.ok());
    REQUIRE(provider.value()->start(context()).ok());
    std::vector<std::byte> data(kM2BlockSize, std::byte{0x11});
    REQUIRE(probe->device->write(context(), 0, data).ok());
    const auto prepared = provider.value()->prepare_checkpoint(prepare_request());
    REQUIRE(prepared.ok());
    REQUIRE(provider.value()->abort_checkpoint(kCandidateNativeId).ok());
    REQUIRE(provider.value()->stop(context(), {}).ok());

    // The candidate was released, so the bundle is provably still on the
    // generation the checkpoint was built on.  That is a *proof*, not a
    // failure: it is what lets the service record the operation as not
    // committed without guessing.
    auto result = inspect(bundle, correlation(prepared.value()));
    REQUIRE(result.ok());
    const auto& inspection = result.value();
    REQUIRE_EQ(inspection.verdict,
               CheckpointRecoveryVerdict::PriorGenerationProved);
    REQUIRE_EQ(inspection.candidate_generation, 0ull);
    REQUIRE_EQ(inspection.journal_operation_id, 3901ull);
    REQUIRE(inspection.candidate_mismatch_mask != 0u);
}

TEST_CASE(m5_checkpoint_inspection_refuses_a_forged_manifest_digest) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto prepared = commit_one_generation(bundle);

    auto expected = correlation(prepared);
    expected.candidate_bytes_digest = prepared.candidate_bytes_digest;
    expected.candidate_bytes_digest[0] ^= std::byte{0xff};
    auto result = inspect(bundle, expected);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().verdict,
               CheckpointRecoveryVerdict::OwnershipUnprovable);
    REQUIRE_EQ(result.value().candidate_mismatch_mask,
               bit(CheckpointRecoveryMismatch::ManifestDigest));
}

TEST_CASE(m5_checkpoint_inspection_refuses_a_mismatched_native_operation_id) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto prepared = commit_one_generation(bundle);

    // The id the inspection reports is read from the committed journal, so a
    // correlation carrying someone else's id cannot be confirmed even though
    // the generations, slot and bytes all match.
    auto expected = correlation(prepared);
    expected.native_operation_id = kCandidateNativeId + 1;
    auto result = inspect(bundle, expected);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().verdict,
               CheckpointRecoveryVerdict::OwnershipUnprovable);
    REQUIRE_EQ(result.value().candidate_mismatch_mask,
               bit(CheckpointRecoveryMismatch::NativeOperationId));
}

TEST_CASE(m5_checkpoint_inspection_refuses_a_mismatched_image_uuid) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto prepared = commit_one_generation(bundle);

    auto expected = correlation(prepared);
    expected.image_uuid.bytes[0] = std::byte{0x99};
    auto result = inspect(bundle, expected);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().verdict,
               CheckpointRecoveryVerdict::OwnershipUnprovable);
    REQUIRE_EQ(result.value().candidate_mismatch_mask,
               bit(CheckpointRecoveryMismatch::ImageUuid));
}

TEST_CASE(m5_checkpoint_inspection_refuses_a_mismatched_manifest_slot) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto prepared = commit_one_generation(bundle);

    auto expected = correlation(prepared);
    expected.manifest_slot =
        static_cast<std::uint8_t>(prepared.manifest_slot == 0 ? 1 : 0);
    auto result = inspect(bundle, expected);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().verdict,
               CheckpointRecoveryVerdict::OwnershipUnprovable);
    // The digest is taken over the slot the bundle actually selected, so it is
    // still the prepared digest and only the slot *claim* disagrees.  That is
    // the point of keeping both conditions: a P4 record naming the wrong slot
    // is caught by the slot field, and byte-level drift by the digest.
    REQUIRE_EQ(result.value().candidate_mismatch_mask,
               bit(CheckpointRecoveryMismatch::ManifestSlot));
}

TEST_CASE(m5_checkpoint_inspection_refuses_a_superseded_candidate) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto first = commit_one_generation(bundle);

    // A later checkpoint moves the committed generation past the candidate.
    // Nothing in the bundle can then attribute the *first* candidate, so the
    // proof must abstain rather than invent a "prior generation" claim: the
    // observed generation is not the candidate's prior generation either.
    {
        proxy::LocalNamespacePolicy policy;
        auto probe = std::make_shared<TransportProbe>();
        auto provider = M3Provider::open_for_test(
            context(), policy, config(bundle), factories(probe));
        REQUIRE(provider.ok());
        REQUIRE(provider.value()->start(context()).ok());
        std::vector<std::byte> data(kM2BlockSize, std::byte{0x77});
        REQUIRE(probe->device->write(context(), kM2BlockSize, data).ok());
        M3CheckpointPrepareRequest request{};
        request.context = context();
        request.barrier = VolumeBarrierKind::RawBlockTestNoFilesystemCache;
        request.native_operation_id = 2 + myrd::kM5CheckpointOperationOffset;
        const auto second = provider.value()->prepare_checkpoint(request);
        REQUIRE(second.ok());
        M3CheckpointCommitRequest commit{};
        commit.context = context();
        commit.native_operation_id = request.native_operation_id;
        commit.manifest_slot = second.value().manifest_slot;
        commit.candidate_bytes_digest = second.value().candidate_bytes_digest;
        REQUIRE(provider.value()->commit_checkpoint(commit).ok());
        REQUIRE(provider.value()->stop(context(), {}).ok());
    }

    auto result = inspect(bundle, correlation(first));
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().verdict,
               CheckpointRecoveryVerdict::OwnershipUnprovable);
    REQUIRE_EQ(result.value().candidate_generation, 2ull);
}

TEST_CASE(m5_checkpoint_inspection_abstains_when_nothing_is_recoverable) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto prepared = commit_one_generation(bundle);

    // Remove the manifest: the bundle is now unreadable as an image, and
    // nothing about the operation can be attributed.  Either the inspection
    // reports unprovable, or it fails operationally -- both refuse to confirm,
    // and neither may report a proved generation.
    std::error_code ignored;
    std::filesystem::remove(bundle.root / "disk.rdm", ignored);
    auto result = inspect(bundle, correlation(prepared));
    if (result.ok()) {
        REQUIRE_EQ(result.value().verdict,
                   CheckpointRecoveryVerdict::OwnershipUnprovable);
        REQUIRE_EQ(result.value().candidate_mismatch_mask,
                   kCheckpointCandidateConditions);
    }
}

TEST_CASE(m5_checkpoint_inspection_writes_nothing) {
    TempBundle bundle;
    create_bundle(bundle);
    const auto prepared = commit_one_generation(bundle);

    const auto before = bundle_fingerprint(bundle.root);
    auto result = inspect(bundle, correlation(prepared));
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().verdict,
               CheckpointRecoveryVerdict::CandidateGenerationProved);
    // An inspection that took a writer transaction, built a runtime or touched
    // a delete guard would show up here.
    REQUIRE_EQ(bundle_fingerprint(bundle.root), before);
}
