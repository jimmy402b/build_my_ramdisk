#include "src/management/service_worker_control.h"

#include "src/common/diagnostic_trace.h"
#include "src/management/windows_worker_observation.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace myrd::management {
namespace {

Error control_error(const ServiceWorkerControlError code,
                    const wchar_t* operation, const wchar_t* message) {
    return {ErrorDomain::internal, static_cast<std::uint32_t>(code), operation,
            message, 0};
}

bool is_zero_uuid(const product::Uuid& id) noexcept {
    for (const auto byte : id.bytes()) {
        if (byte != std::byte{0}) return false;
    }
    return true;
}

// The persistence layer's image uuid, which is raw bytes, as the product uuid
// the durable boundary evidence carries.  Text is the only bridge: `Uuid` has no
// byte-level factory, and the one canonical parser is `Uuid::parse`, so the
// conversion goes through the canonical form rather than inventing a second
// byte encoding that could disagree with it.
std::optional<product::Uuid> to_product_uuid(
    const persistence::ImageUuid& id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < id.bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text.push_back('-');
        const auto byte = std::to_integer<std::uint8_t>(id.bytes[index]);
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0FU]);
    }
    auto parsed = product::Uuid::parse(text);
    if (!parsed.ok()) return std::nullopt;
    return parsed.value();
}

// The reverse of `to_product_uuid`, and a copy rather than a conversion for the
// same reason: `Uuid` stores its canonical form's sixteen bytes in order, so
// going through text here would be two parses that can only fail.
persistence::ImageUuid to_image_uuid(const product::Uuid& id) noexcept {
    persistence::ImageUuid value{};
    const auto bytes = id.bytes();
    std::memcpy(value.bytes.data(), bytes.data(), value.bytes.size());
    return value;
}

// succeeded_clean is the initializer ServiceCore itself uses to mean "this step
// was done and nothing needs reconciling".
WorkerActionResult completed_result() {
    return WorkerActionResult{CompletionOutcome::succeeded_clean, {}};
}

bool is_commit_landed(const CheckpointCommitDisposition disposition) noexcept {
    return disposition == CheckpointCommitDisposition::committed
        || disposition
            == CheckpointCommitDisposition::committed_needs_reconciliation;
}

// The management-side disposition, from the protocol's own spelling.  One
// mapping, mirroring the one in the worker, so the four cases cannot be folded
// back together on either side of the wire.
std::optional<CheckpointCommitDisposition> to_management_disposition(
    const m5::WorkerCommitDisposition disposition) noexcept {
    switch (disposition) {
    case m5::WorkerCommitDisposition::committed:
        return CheckpointCommitDisposition::committed;
    case m5::WorkerCommitDisposition::committed_needs_reconciliation:
        return CheckpointCommitDisposition::committed_needs_reconciliation;
    case m5::WorkerCommitDisposition::commit_indeterminate:
        return CheckpointCommitDisposition::commit_indeterminate;
    case m5::WorkerCommitDisposition::not_committed:
        return CheckpointCommitDisposition::not_committed;
    }
    return std::nullopt;
}

// The persistence failure the worker relayed, carried across unchanged.  Every
// field is the worker's own report; composing one here would name a failure that
// never happened, and ServiceCore validates its evidence against this record, so
// the two have to agree exactly.
CheckpointPersistenceErrorContext to_management_context(
    const m5::WorkerCommitFailure& failure,
    const CheckpointCommitDisposition disposition) {
    CheckpointPersistenceErrorContext context{};
    context.operation = failure.operation;
    context.code = failure.code;
    context.phase = failure.phase;
    context.native_code = failure.native_code;
    context.native_operation_id = failure.native_operation_id;
    context.image_uuid = failure.image_uuid.bytes;
    context.generation = failure.generation;
    context.disposition = disposition;
    context.safe_context = failure.safe_context;
    return context;
}

// The worker takes a drive letter, so a launch target that is not one has to be
// refused -- once, for every producer -- rather than silently truncated into a
// plausible letter.  Refused before any process exists: a worker launched for
// the wrong mount is a worker holding the wrong disk.
bool is_drive_letter(const std::wstring& mount_target) noexcept {
    return mount_target.size() >= 2 && mount_target[1] == L':';
}

// Defined below, beside the forget proof that first needed it.  Declared here so
// the checkpoint reconciliation can close an inspection and wait for the worker
// it launched to actually leave.
bool worker_process_gone(std::uint32_t process_id,
                         std::uint64_t process_creation_time,
                         DWORD timeout_ms);

} // namespace

Result<WorkerLaunchRequest> make_launch_request(
    const profile::ProfileRecord& profile) {
    const auto& record = profile.profile;
    if (record.mount_kind != profile::MountKind::drive_letter) {
        return Result<WorkerLaunchRequest>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"make_launch_request",
            L"only a drive-letter mount can be launched in M5"));
    }
    if (!is_drive_letter(record.mount_target)) {
        return Result<WorkerLaunchRequest>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"make_launch_request",
            L"the durable mount target is not a drive letter"));
    }
    // The line the profile's own lifecycle already draws: `provisioning` means
    // this profile has a bundle and has never had a volume, which is exactly
    // what `append_start_actions` branches on when it decides whether to emit
    // the provisioning steps.
    WorkerLaunchRequest request{
        record.profile_uuid,
        profile.revision,
        record.bundle_path,
        record.image_uuid,
        record.logical_capacity_bytes,
        record.mount_target,
        record.lifecycle == profile::ProfileLifecycle::provisioning
            ? m5::WorkerVolumeMode::provision_new
            : m5::WorkerVolumeMode::adopt_existing};
    // `\\?\Volume{...}\` -- the shape `enumerate_native_volumes` produces.  The
    // selection compares with `_wcsicmp` and strips only a trailing backslash, so a
    // bare uuid matches nothing and falls back to the letter filter.
    if (record.volume_guid.has_value()) {
        const auto text = record.volume_guid->to_string();
        request.volume_guid =
            L"\\\\?\\Volume{" + std::wstring(text.begin(), text.end()) + L"}\\";
    }
    // The label the profile carries, so a restart re-applies it: the volume remembers a label only until it is
    // re-created from the bundle, and re-creating is exactly what a recovery does.
    request.volume_label = record.volume_label;
    // The budget the profile was registered with.  The worker's store is opened with it,
    // so the memory policy the user asked for is the one enforced -- previously the
    // worker always opened with its default and `--memory-hard-limit` was inert.
    request.ram_limit_bytes = record.memory.hard_limit_bytes;
    return Result<WorkerLaunchRequest>::success(std::move(request));
}

Result<WorkerLaunchRequest> make_launch_request(
    const LifecycleOperationIntent& intent) {
    if (!is_drive_letter(intent.mount_target)) {
        return Result<WorkerLaunchRequest>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"make_launch_request",
            L"the operation's mount target is not a drive letter"));
    }
    // A create makes the volume; an import adopts the one the bundle already
    // has.  There is no third case: the intent's kind *is* that distinction.
    WorkerLaunchRequest request{
        intent.profile_id,
        // No durable record yet, so no revision to name.
        0,
        intent.bundle_path,
        intent.expected_image_id,
        intent.requested_capacity_bytes,
        intent.mount_target,
        intent.kind == LifecycleOperationKind::create
            ? m5::WorkerVolumeMode::provision_new
            : m5::WorkerVolumeMode::adopt_existing};
    // A create's budget is decided at admission and must reach the worker that opens the
    // store; materializing it here uses the same rule the registration uses, so the
    // profile record and the runtime cannot disagree about the limit.  An import's
    // capacity is only known once the worker has inspected the bundle, so it stays zero.
    if (intent.kind == LifecycleOperationKind::create
        && intent.physical_memory.total_bytes != 0) {
        auto memory = materialize_memory_policy(
            intent.memory_mode, intent.memory_hard_limit_bytes,
            intent.requested_capacity_bytes, intent.physical_memory);
        if (!memory.ok()) {
            return Result<WorkerLaunchRequest>::failure(memory.error());
        }
        request.ram_limit_bytes = memory.value().hard_limit_bytes;
    }
    // The create-only cluster size travels with the operation, not the record: the worker
    // formats the volume once, and a later start adopts what was formatted (R-17a).
    if (intent.kind == LifecycleOperationKind::create) {
        request.cluster_size_bytes = intent.cluster_size_bytes;
    }
    return Result<WorkerLaunchRequest>::success(std::move(request));
}

ServiceWorkerControl::ServiceWorkerControl(
    IWorkerSessionProvider& sessions,
    const std::uint32_t control_timeout_ms,
    IWorkerFingerprintSink* const fingerprints) noexcept
    : sessions_(sessions),
      control_timeout_ms_(control_timeout_ms == 0 ? kDefaultWorkerControlTimeoutMs
                                                   : control_timeout_ms),
      fingerprints_(fingerprints) {}

OperationContext ServiceWorkerControl::context() const {
    OperationContext value{};
    value.deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(control_timeout_ms_);
    return value;
}

OperationContext ServiceWorkerControl::reclaim_context() const {
    OperationContext value{};
    // The longer of the configured command budget and the session's own maximum, so a
    // service configured with a large budget is not silently narrowed here, and one
    // configured with a small budget still gets a bound that a real volume can meet.
    const auto budget = std::max<std::uint64_t>(
        control_timeout_ms_, kMaximumWorkerCommandTimeoutMs);
    value.deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(budget);
    return value;
}

OperationContext ServiceWorkerControl::provisioning_context() const {
    OperationContext value{};
    // See the declaration: a create's cost is set by the disk, so it gets the provisioning
    // ceiling rather than the ordinary command bound.  `max` keeps a service configured with an
    // even larger budget from being narrowed here.
    const auto budget = std::max<std::uint64_t>(
        control_timeout_ms_, kProvisioningWorkerCommandTimeoutMs);
    value.deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(budget);
    return value;
}

bool ServiceWorkerControl::begins_noncancellable(const ActionKind) const noexcept {
    // Deliberately nothing.  Every mutation this adapter performs is either
    // already forced noncancellable by ServiceCore's own stop-removal-boundary
    // rule, or is cancellable *by design*: M4 resolves a checkpoint through the
    // pre-commit boundary callback, where a cancel becomes a proven abort rather
    // than a lost cut, and a stop that abandoned its candidate resolves it
    // through resume_mutations.  Claiming a boundary here would narrow M4's
    // cancellation semantics, which M5 may not change.
    return false;
}

ServiceWorkerSession* ServiceWorkerControl::find_session(
    const product::Uuid& profile_id) noexcept {
    const auto found = std::find_if(
        sessions_by_profile_.begin(), sessions_by_profile_.end(),
        [&profile_id](const auto& entry) { return entry.first == profile_id; });
    return found == sessions_by_profile_.end() ? nullptr : found->second.get();
}

void ServiceWorkerControl::forget_session(
    const product::Uuid& profile_id) noexcept {
    const auto found = std::find_if(
        sessions_by_profile_.begin(), sessions_by_profile_.end(),
        [&profile_id](const auto& entry) { return entry.first == profile_id; });
    if (found != sessions_by_profile_.end()) sessions_by_profile_.erase(found);
}

Result<ServiceWorkerSession*> ServiceWorkerControl::require_session(
    const product::Uuid& profile_id, const bool attach_if_absent) {
    if (auto* const held = find_session(profile_id); held != nullptr) {
        return Result<ServiceWorkerSession*>::success(held);
    }
    if (!attach_if_absent) {
        return Result<ServiceWorkerSession*>::failure(control_error(
            ServiceWorkerControlError::execution_not_bound,
            L"service_worker_control.require_session",
            L"No authenticated worker is bound to this profile"));
    }
    auto attached = sessions_.attach(profile_id, context());
    if (!attached.ok()) {
        return Result<ServiceWorkerSession*>::failure(attached.error());
    }
    if (!attached.value()) {
        return Result<ServiceWorkerSession*>::failure(control_error(
            ServiceWorkerControlError::execution_not_bound,
            L"service_worker_control.require_session",
            L"No runtime is bound to this profile to adopt"));
    }
    auto adopted = attached.move_value();
    auto* const adopted_raw = adopted.get();
    sessions_by_profile_.emplace_back(profile_id, std::move(adopted));
    return Result<ServiceWorkerSession*>::success(adopted_raw);
}

Result<WorkerActionResult> ServiceWorkerControl::verify_binding(
    const WorkerActionRequest& request,
    ServiceWorkerSession& session) const {
    // The binding this channel authenticated.  It carries the profile and the
    // process identity but no image: in M5 the image is a fact about the runtime
    // the worker holds, so it is checked where a status is read rather than
    // asserted by the binding.
    if (session.binding().profile_id != request.profile_id) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::execution_identity_conflict,
            L"service_worker_control.verify_binding",
            L"The bound worker serves a different profile"));
    }
    return Result<WorkerActionResult>::success(completed_result());
}

std::optional<ServiceWorkerControl::HeldCandidate>
ServiceWorkerControl::take_candidate(const product::Uuid& profile_id,
                                     const product::Uuid& operation_id) {
    const auto found = std::find_if(
        held_candidates_.begin(), held_candidates_.end(),
        [&profile_id, &operation_id](const HeldCandidate& candidate) {
            return candidate.profile_id == profile_id
                && candidate.operation_id == operation_id;
        });
    if (found == held_candidates_.end()) return std::nullopt;
    auto taken = *found;
    held_candidates_.erase(found);
    return taken;
}

std::optional<ServiceWorkerControl::HeldRemoval>
ServiceWorkerControl::find_removal(const product::Uuid& profile_id,
                                   const product::Uuid& operation_id) const noexcept {
    const auto found = std::find_if(
        held_removals_.begin(), held_removals_.end(),
        [&profile_id, &operation_id](const HeldRemoval& removal) {
            return removal.profile_id == profile_id
                && removal.operation_id == operation_id;
        });
    return found == held_removals_.end() ? std::nullopt
                                         : std::optional<HeldRemoval>(*found);
}

void ServiceWorkerControl::advance_held_removal(
    const product::Uuid& profile_id, const product::Uuid& operation_id,
    const std::uint64_t generation) noexcept {
    const auto found = std::find_if(
        held_removals_.begin(), held_removals_.end(),
        [&profile_id, &operation_id](const HeldRemoval& removal) {
            return removal.profile_id == profile_id
                && removal.operation_id == operation_id;
        });
    if (found == held_removals_.end()) return;
    // Only the generation moves.  A commit publishes a snapshot; it does not
    // remove a mount, so it cannot make a removal look performed.
    found->generation = generation;
}

Result<WorkerActionResult> ServiceWorkerControl::unrealized(
    const WorkerActionRequest& request) const {
    static_cast<void>(request);
    // The action has no authenticated worker command behind it.  Reporting
    // success would tell ServiceCore a mutation had happened when nothing had,
    // which is exactly the failure this adapter exists to prevent.
    return Result<WorkerActionResult>::failure(control_error(
        ServiceWorkerControlError::unrealized_action,
        L"service_worker_control.unrealized_action",
        L"The action has no authenticated worker command that realizes it"));
}

Result<WorkerActionResult> ServiceWorkerControl::bind_worker(
    const WorkerActionRequest& request, const bool launch_if_absent) {
    if (is_zero_uuid(request.profile_id)) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.bind_worker",
            L"The action names no profile"));
    }
    if (auto* const held = find_session(request.profile_id); held != nullptr) {
        // A runtime for this profile is already supervised.  Launching a second
        // one would make two runtimes claim one disk, so the runtime that exists
        // is verified instead.
        return verify_binding(request, *held);
    }
    if (!launch_if_absent) {
        auto adopted = require_session(request.profile_id, true);
        if (!adopted.ok()) {
            return Result<WorkerActionResult>::failure(adopted.error());
        }
        return verify_binding(request, *adopted.value());
    }
    if (!request.durable_profile.has_value()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.bind_worker",
            L"A worker cannot be launched without its durable profile"));
    }
    auto launch_request = make_launch_request(request.durable_profile.value());
    if (!launch_request.ok()) {
        return Result<WorkerActionResult>::failure(launch_request.error());
    }
    // **The create-only cluster size rides the operation's intent, not the record (R-17a).**
    // A create registers its profile before the worker that provisions the volume is bound,
    // so the record-based launch above cannot know how the volume should be formatted.  The
    // intent is still in the request; copying the one field here is what makes
    // `--cluster-size` reach the provisioning helper.
    if (request.lifecycle_intent.has_value()) {
        launch_request.value().cluster_size_bytes =
            request.lifecycle_intent->cluster_size_bytes;
    }
    auto launched = sessions_.launch(launch_request.value(), context());
    if (!launched.ok()) {
        return Result<WorkerActionResult>::failure(launched.error());
    }
    auto owned = launched.move_value();
    auto* const session = owned.get();
    sessions_by_profile_.emplace_back(request.profile_id, std::move(owned));
    auto bound = verify_binding(request, *session);
    if (!bound.ok()) return bound;

    // Launching authenticates and opens the bundle; it does not start the
    // runtime.  The worker's own ordering is what makes that safe -- it may not
    // touch the bundle before the handshake -- so bringing the runtime up is a
    // separate, attributable command and the startup steps below can rely on it
    // having happened.
    auto started = session->start(context(), request.operation_id);
    if (!started.ok()) {
        forget_session(request.profile_id);
        return Result<WorkerActionResult>::failure(started.error());
    }
    return Result<WorkerActionResult>::success(completed_result());
}

Result<WorkerActionResult> ServiceWorkerControl::verify_startup_step(
    const WorkerActionRequest& request) {
    auto session = require_session(request.profile_id, true);
    if (!session.ok()) {
        return Result<WorkerActionResult>::failure(session.error());
    }
    auto bound = verify_binding(request, *session.value());
    if (!bound.ok()) return bound;
    auto status = session.value()->require_runtime_observation(context());
    if (!status.ok()) {
        return Result<WorkerActionResult>::failure(status.error());
    }
    auto result = completed_result();

    switch (request.action) {
    case ActionKind::start_proxy:
    case ActionKind::create_exact_device:
    case ActionKind::locate_exact_physical_drive:
    case ActionKind::provision_gpt_ntfs:
    case ActionKind::assign_exact_mount:
    case ActionKind::readiness_io:
        // The worker performs this whole sequence inside its own start() and
        // answers only once it has finished, and the answer is authenticated and
        // image-verified.  Departure worth stating plainly: the historical
        // adapter *also* checked device geometry per step (extent length, port,
        // lun).  The M5 status carries no geometry, so those checks are gone --
        // a coarser proof boundary, chosen deliberately (manifest 搂7 item 19).
        return Result<WorkerActionResult>::success(std::move(result));

    case ActionKind::record_volume_identity: {
        // The identity the worker proved is the identity ServiceCore writes into
        // the durable profile.  It is read from the worker's own observation and
        // never composed here.
        if (!status.value().volume.has_value()
            || !status.value().volume->complete()) {
            return Result<WorkerActionResult>::failure(control_error(
                ServiceWorkerControlError::volume_identity_unproven,
                L"service_worker_control.record_volume_identity",
                L"The worker holds a volume but did not prove its identity"));
        }
        // `product::Uuid` has no public default constructor, so each is parsed
        // into its own Result and the evidence is aggregate-initialized from
        // them -- it cannot be created empty and then filled in.
        auto disk_id = product::Uuid::parse(status.value().volume->disk_guid);
        auto partition_id =
            product::Uuid::parse(status.value().volume->partition_guid);
        auto volume_id = product::Uuid::parse(status.value().volume->volume_guid);
        if (!disk_id.ok() || !partition_id.ok() || !volume_id.ok()) {
            // Canonical text that this build cannot parse is an identity the
            // service cannot record.  Refused rather than stored as zeroes.
            return Result<WorkerActionResult>::failure(control_error(
                ServiceWorkerControlError::volume_identity_unproven,
                L"service_worker_control.record_volume_identity",
                L"The worker's volume identity is not canonical text"));
        }
        result.lifecycle.volume = VolumeRegistrationEvidence{
            disk_id.value(), partition_id.value(), volume_id.value()};
        return Result<WorkerActionResult>::success(std::move(result));
    }

    case ActionKind::verify_existing_volume_identity: {
        if (!request.durable_profile.has_value()) break;
        const auto& profile = request.durable_profile->profile;
        const std::size_t recorded =
            (profile.gpt_disk_guid.has_value() ? 1U : 0U)
            + (profile.partition_guid.has_value() ? 1U : 0U)
            + (profile.volume_guid.has_value() ? 1U : 0U);
        // The durable record is either fully identified or not identified at
        // all.  A partial identity is a durable-state fault, not something to
        // verify a live runtime against, so it fails closed here.
        if (recorded != 0 && recorded != 3) break;
        if (recorded == 0) {
            // Nothing to compare against.  That is what a profile created before
            // its first successful start looks like, and refusing here would
            // make such a profile unstartable.
            return Result<WorkerActionResult>::success(std::move(result));
        }
        if (!status.value().volume.has_value()
            || !status.value().volume->complete()) {
            break;
        }
        std::string expected_volume;
        {
            // The recorded volume id is compared as canonical text, because the
            // worker reports text and a second encoding would be a second thing
            // to keep in step.
            expected_volume = profile.volume_guid.value().to_string();
        }
        if (status.value().volume->volume_guid != expected_volume) break;
        {
            auto parsed = product::Uuid::parse(status.value().volume->disk_guid);
            if (!parsed.ok()
                || !(parsed.value() == profile.gpt_disk_guid.value())) {
                break;
            }
        }
        {
            auto parsed =
                product::Uuid::parse(status.value().volume->partition_guid);
            if (!parsed.ok()
                || !(parsed.value() == profile.partition_guid.value())) {
                break;
            }
        }
        return Result<WorkerActionResult>::success(std::move(result));
    }

    default:
        break;
    }
    return Result<WorkerActionResult>::failure(control_error(
        ServiceWorkerControlError::worker_reply_unexpected,
        L"service_worker_control.verify_startup_step",
        L"The live runtime does not evidence the step the plan reached"));
}

Result<WorkerActionResult> ServiceWorkerControl::freeze_writes(
    const WorkerActionRequest& request) {
    auto session = require_session(request.profile_id, true);
    if (!session.ok()) {
        return Result<WorkerActionResult>::failure(session.error());
    }
    auto bound = verify_binding(request, *session.value());
    if (!bound.ok()) return bound;

    // Unlike the quiesce above, this one *is* a mutation: it changes what the
    // mounted filesystem may do.  It is therefore required to name the operation
    // it belongs to, because the only thing that may release it is the same
    // operation -- `resume_mutations` -- and a freeze nobody can attribute is a
    // freeze nobody can release.
    if (is_zero_uuid(request.operation_id)) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.freeze_writes",
            L"A write freeze cannot be taken without an operation identity"));
    }
    // The identity is checked *before* the freeze, not after it.  Freezing is the
    // step that starts refusing writes to a mounted volume, and a refusal that
    // came back saying "different image" once the volume was already frozen would
    // leave this adapter holding a lock on a disk it has just declared it does
    // not own -- with no operation able to release it.  Observed first, so the
    // fail-closed answer happens while nothing has been taken.
    auto observed = session.value()->require_runtime_observation(context());
    if (!observed.ok()) {
        return Result<WorkerActionResult>::failure(observed.error());
    }
    if (request.durable_profile.has_value()) {
        auto image = to_product_uuid(observed.value().image_id);
        if (!image.has_value()
            || !(*image == request.durable_profile->profile.image_uuid)) {
            return Result<WorkerActionResult>::failure(control_error(
                ServiceWorkerControlError::execution_identity_conflict,
                L"service_worker_control.freeze_writes",
                L"The live runtime serves a different product image"));
        }
    }
    auto status = session.value()->freeze_writes(context(), request.operation_id);
    if (!status.ok()) {
        return Result<WorkerActionResult>::failure(status.error());
    }
    return Result<WorkerActionResult>::success(completed_result());
}

Result<WorkerActionResult> ServiceWorkerControl::quiesce_mutations(
    const WorkerActionRequest& request) {
    auto session = require_session(request.profile_id, true);
    if (!session.ok()) {
        return Result<WorkerActionResult>::failure(session.error());
    }
    auto bound = verify_binding(request, *session.value());
    if (!bound.ok()) return bound;

    // Strict: "could not observe a live runtime" must not read as clean.  This
    // adapter holds no write freeze to take, so quiescing is a proof and not a
    // mutation -- the runtime the operation is about to stop is the one the
    // durable profile describes, and it is reachable and live.
    auto status = session.value()->require_runtime_observation(context());
    if (!status.ok()) {
        return Result<WorkerActionResult>::failure(status.error());
    }
    // The worker reports its image as raw bytes and the durable profile holds a
    // product uuid, so the comparison goes through the one canonical conversion
    // rather than comparing two different representations.
    if (request.durable_profile.has_value()) {
        auto image = to_product_uuid(status.value().image_id);
        if (!image.has_value()
            || !(*image == request.durable_profile->profile.image_uuid)) {
            return Result<WorkerActionResult>::failure(control_error(
                ServiceWorkerControlError::execution_identity_conflict,
                L"service_worker_control.quiesce_mutations",
                L"The live runtime serves a different product image"));
        }
    }

    // The generation the runtime stands at is recorded here, and it is the only
    // generation a later removal may be authorized at.
    const auto existing = std::find_if(
        held_removals_.begin(), held_removals_.end(),
        [&request](const HeldRemoval& removal) {
            return removal.profile_id == request.profile_id;
        });
    // The witness is not the removal: a fresh authorization is recorded as
    // unproven, so a removal this operation then fails to perform cannot leave a
    // proof standing for the steps that follow it.
    const auto generation = status.value().committed_generation;
    if (existing == held_removals_.end()) {
        held_removals_.push_back(HeldRemoval{
            request.profile_id, request.operation_id, generation, false});
    } else {
        existing->operation_id = request.operation_id;
        existing->generation = generation;
        existing->proven = false;
    }
    return Result<WorkerActionResult>::success(completed_result());
}

Result<ServiceWorkerControl::HeldCandidate>
ServiceWorkerControl::cut_candidate(ServiceWorkerSession& session,
                                    const WorkerActionRequest& request) {
    if (is_zero_uuid(request.operation_id)) {
        return Result<HeldCandidate>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.cut_candidate",
            L"A checkpoint cannot be cut without an operation identity"));
    }
    // The image this cut belongs to is what the live worker holds, not what the
    // request claims: the evidence names an image, and naming one the worker is
    // not running would let a durable record point at the wrong bundle.
    auto observed = session.require_runtime_observation(context());
    if (!observed.ok()) {
        return Result<HeldCandidate>::failure(observed.error());
    }
    auto image_id = to_product_uuid(observed.value().image_id);
    if (!image_id.has_value()) {
        return Result<HeldCandidate>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.cut_candidate",
            L"The live runtime's image identity is not a canonical uuid"));
    }
    if (request.durable_profile.has_value()
        && !(*image_id == request.durable_profile->profile.image_uuid)) {
        return Result<HeldCandidate>::failure(control_error(
            ServiceWorkerControlError::execution_identity_conflict,
            L"service_worker_control.cut_candidate",
            L"The live runtime serves a different product image"));
    }

    // **The cut is a data-heavy action, so it gets the provisioning ceiling.**
    //
    // `prepare_checkpoint` flushes the mounted volume, takes a consistent snapshot and writes
    // the dirty set into the journal -- and every one of those scales with the disk.  Measured
    // on the guest: a 7 GiB write into a 16 GiB disk crossed the dirty threshold, the
    // maintenance checkpoint's `flush-volume` was abandoned at the ordinary 120 s bound
    // (`code=4107`), and the whole checkpoint was then refused.  The ceiling is the same one a
    // create gets, because it is the same kind of work.  A genuinely wedged worker still fails,
    // just later.
    auto prepared = session.prepare_checkpoint(provisioning_context(),
                                               request.operation_id);
    if (!prepared.ok()) {
        return Result<HeldCandidate>::failure(prepared.error());
    }
    const auto& reply = prepared.value();
    // Every field below is the worker's authenticated fact: the generations, the
    // slot and the digest were read from the live snapshot, and nothing here is
    // derived.  A candidate generation that does not follow the committed one is
    // not a snapshot this service can name.
    if (reply.candidate_generation != reply.prior_generation + 1) {
        return Result<HeldCandidate>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.cut_candidate",
            L"The candidate generation does not follow the committed one"));
    }
    const auto& binding = session.binding();
    // Aggregate initialization, not default-then-assign: the struct holds
    // `product::Uuid`s, which have no public default constructor, so it cannot
    // be value-initialized at all.
    return Result<HeldCandidate>::success(HeldCandidate{
        request.profile_id,
        request.operation_id,
        CheckpointCommitBoundaryEvidence{
            request.operation_id,
            request.profile_id,
            *image_id,
            binding.process_id,
            binding.process_creation_time,
            binding.session_generation,
            reply.prior_generation,
            reply.candidate_generation,
            reply.cut_dirty_epoch,
            reply.native_operation_id == 0
                ? std::nullopt
                : std::optional<std::uint64_t>(reply.native_operation_id),
            // Persisted with the boundary so a later recovery can ask the
            // bundle whether *this* candidate landed, which needs the slot it
            // would occupy and the digest of the bytes it would contain.
            reply.manifest_slot,
            reply.candidate_bytes_digest},
        // Kept so the commit can repeat the frozen identity; the boundary
        // evidence carries neither the slot nor the digest.
        reply});
}

Result<WorkerActionResult> ServiceWorkerControl::flush_exact_volume(
    const WorkerActionRequest& request) {
    if (!request.durable_profile.has_value()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.flush_exact_volume",
            L"The volume flush needs the operation's durable profile"));
    }
    auto session = require_session(request.profile_id, true);
    if (!session.ok()) {
        return Result<WorkerActionResult>::failure(session.error());
    }
    auto bound = verify_binding(request, *session.value());
    if (!bound.ok()) return bound;

    // The flush and the consistent cut are one operation, and the cut it takes
    // is the one a later commit publishes.  Re-cutting for an operation that
    // already holds a candidate would describe a different moment than the one
    // the service is about to commit, so the existing candidate is reused.
    const auto held = std::find_if(
        held_candidates_.begin(), held_candidates_.end(),
        [&request](const HeldCandidate& candidate) {
            return candidate.profile_id == request.profile_id
                && candidate.operation_id == request.operation_id;
        });
    if (held != held_candidates_.end()) {
        return Result<WorkerActionResult>::success(completed_result());
    }
    auto cut = cut_candidate(*session.value(), request);
    if (!cut.ok()) return Result<WorkerActionResult>::failure(cut.error());
    held_candidates_.push_back(cut.move_value());
    return Result<WorkerActionResult>::success(completed_result());
}

Result<WorkerActionResult> ServiceWorkerControl::abort_held_candidate(
    ServiceWorkerSession& session, const HeldCandidate& candidate) {
    auto aborted = session.abort_checkpoint(context(), candidate.operation_id,
                                            0);
    if (!aborted.ok()) {
        return Result<WorkerActionResult>::failure(aborted.error());
    }
    if (!aborted.value().aborted) {
        // "Not aborted" covers two facts the caller must not confuse, and the
        // reply now says which: there was no candidate to cancel, which leaves
        // the bundle untouched, or the candidate existed and could not be
        // cancelled, which leaves its state unknown and is what forces a
        // reconciliation.
        return Result<WorkerActionResult>::failure(control_error(
            aborted.value().refusal == m5::WorkerAbortRefusal::no_candidate
                ? ServiceWorkerControlError::checkpoint_candidate_missing
                : ServiceWorkerControlError::abort_refused,
            L"service_worker_control.abort_held_candidate",
            L"The worker did not cancel the checkpoint candidate"));
    }
    // A successful abort does not move the committed generation.  One that did
    // would mean the runtime published something while releasing the candidate,
    // which is not a release this service may record.
    if (aborted.value().generation != candidate.evidence.prior_generation) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.abort_held_candidate",
            L"An aborted candidate changed the committed generation"));
    }
    return Result<WorkerActionResult>::success(completed_result());
}

Result<WorkerActionResult> ServiceWorkerControl::resume_mutations(
    const WorkerActionRequest& request) {
    auto held = take_candidate(request.profile_id, request.operation_id);
    if (!held.has_value()) {
        // Nothing this call owns is still undecided.  The write freeze, when one
        // was taken, is still released below: it belongs to the operation, not to
        // the candidate, and a rolled-back stop that left the volume frozen would
        // strand a profile that is supposed to be running.  There is nothing to
        // resume here only when no freeze was taken either -- the release is
        // idempotent, so the safe direction is always to ask.
        return release_freeze_if_held(request);
    }
    auto* const session = find_session(request.profile_id);
    if (session == nullptr) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::execution_not_bound,
            L"service_worker_control.resume_mutations",
            L"An undecided candidate outlived the runtime that cut it"));
    }
    auto aborted = abort_held_candidate(*session, held.value());
    if (!aborted.ok()) return aborted;
    return release_freeze_if_held(request);
}

Result<WorkerActionResult> ServiceWorkerControl::release_freeze_if_held(
    const WorkerActionRequest& request) {
    // Deliberately `find_session` and not `require_session`: a profile this
    // adapter has no bound channel to holds no freeze either.  A freeze can only
    // have been taken through this adapter's own session -- the reply that
    // confirmed it came over that channel -- so no session is a proof that there
    // is nothing here to release, not a failure to reach something.  Attaching
    // instead would let a resume launch or adopt a worker for a profile that is
    // mid-rollback, which is a much larger action than giving a lock back.
    auto* const session = find_session(request.profile_id);
    if (session == nullptr) {
        return Result<WorkerActionResult>::success(completed_result());
    }
    auto released =
        session->release_write_freeze(context(), request.operation_id);
    if (!released.ok()) {
        return Result<WorkerActionResult>::failure(released.error());
    }
    return Result<WorkerActionResult>::success(completed_result());
}

Result<WorkerActionResult> ServiceWorkerControl::remove_mount(
    const WorkerActionRequest& request) {
    auto session = require_session(request.profile_id, true);
    if (!session.ok()) {
        return Result<WorkerActionResult>::failure(session.error());
    }
    auto bound = verify_binding(request, *session.value());
    if (!bound.ok()) return bound;

    // A candidate this profile is still holding must not survive into a removal.
    // ServiceCore decided not to publish it -- that is why the removal is being
    // reached at all -- and removal drops every uncommitted write, so the
    // undecided cut is aborted first, provably or not at all.
    for (auto candidate = held_candidates_.begin();
         candidate != held_candidates_.end();) {
        if (candidate->profile_id != request.profile_id) {
            ++candidate;
            continue;
        }
        auto aborted = abort_held_candidate(*session.value(), *candidate);
        if (!aborted.ok()) {
            return Result<WorkerActionResult>::failure(aborted.error());
        }
        candidate = held_candidates_.erase(candidate);
    }

    // Strict, before anything is authorized: the removal names an image, and a
    // removal authorized against an image the worker is not running would remove
    // the wrong profile's resources.
    auto observed = session.value()->require_runtime_observation(context());
    if (!observed.ok()) {
        return Result<WorkerActionResult>::failure(observed.error());
    }
    if (request.durable_profile.has_value()) {
        auto image = to_product_uuid(observed.value().image_id);
        if (!image.has_value()
            || !(*image == request.durable_profile->profile.image_uuid)) {
            return Result<WorkerActionResult>::failure(control_error(
                ServiceWorkerControlError::execution_identity_conflict,
                L"service_worker_control.remove_mount",
                L"The live runtime serves a different product image"));
        }
    }

    // The authorization the worker re-checks against its own image and its own
    // three proofs.  On the stop path those are facts the worker established
    // when its start() took the writer lock, matched the device and re-read the
    // bundle uuid -- and the private channel authenticates that this is that
    // worker.  The service names the operation, the profile and the image it
    // verified; it does not assert proofs it did not obtain (manifest 搂7
    // item 21).
    m5::WorkerRemovalAuthorization authorization{request.operation_id,
                                                 request.profile_id};
    authorization.image_id = observed.value().image_id;
    authorization.ownership_evidence_digest = {};
    authorization.writer_lock_proved = true;
    authorization.device_match_proved = true;
    authorization.bundle_identity_proved = true;
    // A force stop is the only plan admitted with the user's volatile-data-loss
    // acknowledgement (`force_stop_requested` requires both flags), so the
    // operation's own command is the authorization.  A normal stop keeps the
    // worker's fail-closed refusal for a dirty runtime.
    authorization.accept_data_loss =
        request.command == ManagementCommand::force_stop;

    auto removed = session.value()->remove_and_stop(context(),
                                                    request.operation_id,
                                                    authorization);
    if (!removed.ok()) {
        return Result<WorkerActionResult>::failure(removed.error());
    }
    // **Ask the worker to terminate, which nothing in this service has ever done.**  `shutdown` was
    // written for exactly this and had no callers: the worker only left when its channel closed, and the
    // session is held until the service exits, so a stopped disk kept a live worker -- measured as
    // `worker: command received` every fifteen seconds and a process still present two minutes after
    // `disk stop`, while `disk list` reported `runtime=Stopped` and `mem=0/0`.
    //
    // Best effort by design: the removal above already succeeded and the runtime is stopped, so a worker
    // that left before answering is the outcome this asked for, not a failure.
    const auto asked = session.value()->shutdown(context(), request.operation_id);
    // **Not swallowed silently.**  The first version threw this away, so when the worker still outlived the
    // stop there was no way to tell whether the request had reached it -- the same habit this project keeps
    // paying for.  This line says which of the two happened.
    diagnostic_trace(asked.ok() ? "stop: worker asked to shut down and answered"
                                : "stop: shutdown request failed");
    const auto& reply = removed.value();
    if (reply.refused()) {
        // The worker's own decision about its own runtime, and the reason is
        // the only part this caller can act on: a dirty runtime must be resolved
        // against durable truth, never overridden.
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::removal_refused,
            L"service_worker_control.remove_mount",
            reply.refusal == m5::WorkerRemovalRefusal::runtime_dirty
                ? L"The worker refused to remove: its runtime stands dirty"
                : L"The worker refused to remove"));
    }
    if (!reply.removed || !reply.stopped) {
        // The protocol refuses this combination on the wire, so reaching it
        // means the reply did not come from a peer this adapter understands.
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.remove_mount",
            L"The removal reply claims a partial teardown"));
    }
    // The generation the removal happened at must be the one this authority
    // proved committed.  Removal drops every uncommitted write, so a removal
    // proven at the wrong generation is not a removal this service may record.
    std::uint64_t authorized = reply.generation;
    if (auto held = find_removal(request.profile_id, request.operation_id);
        held.has_value()) {
        authorized = held->generation;
    }
    if (reply.generation != authorized) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::removal_generation_mismatch,
            L"service_worker_control.remove_mount",
            L"The worker removed at a generation removal was not authorized at"));
    }

    const auto existing = std::find_if(
        held_removals_.begin(), held_removals_.end(),
        [&request](const HeldRemoval& removal) {
            return removal.profile_id == request.profile_id;
        });
    if (existing == held_removals_.end()) {
        held_removals_.push_back(HeldRemoval{
            request.profile_id, request.operation_id, reply.generation, true});
    } else {
        existing->operation_id = request.operation_id;
        existing->generation = reply.generation;
        existing->proven = true;
    }
    // The worker's runtime is gone, so the session no longer describes anything
    // this adapter may command.
    forget_session(request.profile_id);
    return Result<WorkerActionResult>::success(completed_result());
}

Result<WorkerActionResult> ServiceWorkerControl::confirm_removal(
    const WorkerActionRequest& request) {
    // A witnessed generation is not enough here.  Only a removal the worker
    // actually performed may stand behind a claim that the device is gone and
    // the writer lock is released.
    const auto held = find_removal(request.profile_id, request.operation_id);
    if (!held.has_value() || !held->proven) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::removal_unproven,
            L"service_worker_control.confirm_removal",
            L"No proven removal stands behind this step"));
    }
    // The removal barrier takes the exact device together with the mount and
    // stops the runtime in the same authenticated call, and a reply that said
    // removed-without-stopped was refused on the wire.  Both steps are therefore
    // already done, and the proof that they are is the recorded removal.
    return Result<WorkerActionResult>::success(completed_result());
}

Result<WorkerActionResult> ServiceWorkerControl::register_bundle(
    const WorkerActionRequest& request, const BundleEvidenceSource source) {
    // The launch is built from the operation's intent, not from a durable
    // record: both of these actions run before their profile is published, and
    // this is the fact that made the ordering the plan already has workable.
    if (!request.lifecycle_intent.has_value()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.register_bundle",
            L"The bundle action needs the operation's lifecycle intent"));
    }
    const auto& intent = request.lifecycle_intent.value();
    // Which action asked and which operation this is have to agree.  A create
    // intent answered by an inspection would record a bundle it never created as
    // read-only, and the source is what ServiceCore's own validation turns on.
    const auto expected_kind = source == BundleEvidenceSource::created_and_verified
        ? LifecycleOperationKind::create
        : LifecycleOperationKind::import;
    if (intent.kind != expected_kind) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.register_bundle",
            L"The lifecycle intent is not the operation this action registers"));
    }
    auto launch_request = make_launch_request(intent);
    if (!launch_request.ok()) {
        return Result<WorkerActionResult>::failure(launch_request.error());
    }
    // The bundle action's own budget follows what it is about to do.  A create builds and then
    // provisions, so it gets the provisioning ceiling; an inspection opens an existing bundle
    // and stays on the ordinary command bound.
    const bool creating = source == BundleEvidenceSource::created_and_verified;
    const auto action_context = creating ? provisioning_context() : context();
    auto launched = sessions_.launch(launch_request.value(), action_context);
    if (!launched.ok()) {
        return Result<WorkerActionResult>::failure(launched.error());
    }
    auto owned = launched.move_value();
    auto* const session = owned.get();

    // The command follows the source, because they are one decision seen from
    // two sides: a bundle that was created is brought up and provisioned, and a
    // bundle that was found is opened and left alone.
    auto observed = creating
        ? session->create_bundle(action_context, request.operation_id)
        : session->inspect_bundle(action_context, request.operation_id);
    if (!observed.ok()) {
        // Nothing this adapter holds may be left behind on a failed
        // registration.  The session is dropped either way, which closes the
        // channel and lets the worker exit and release whatever it opened.
        return Result<WorkerActionResult>::failure(observed.error());
    }

    if (creating && request.command != ManagementCommand::create_stopped) {
        // Held, because the plan continues: `create_runtime_and_worker` finds
        // this session rather than launching a second worker for one bundle, and
        // the provisioning-family steps that follow read the runtime this
        // command started.
        sessions_by_profile_.emplace_back(request.profile_id, std::move(owned));
    }
    // An inspection is not held: nothing is running to command, and the worker
    // exits when the channel closes.  Holding it would keep a process alive for
    // a profile whose operation has nothing left to do.
    //
    // **A stopped create is the same case and was not treated as one.**  `creating` is about *how* the bundle
    // was produced, and it was also deciding whether to keep the session -- so `create_stopped`, whose plan ends
    // here, kept a worker alive with nothing to command: measured as a process still running two minutes after a
    // `--no-start` create, while `disk list` reported `stopped/configured` and `mem=0/0`.  Only a plan that goes
    // on to start the runtime needs the session kept, which is why the condition above names the command.

    const auto& evidence = observed.value().bundle.value();
    auto image_id = to_product_uuid(evidence.image_id);
    if (!image_id.has_value()) {
        // Canonical text that this build cannot parse is an identity the service
        // cannot record.  Refused rather than stored as zeroes.
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.register_bundle",
            L"The bundle's image identity is not a canonical uuid"));
    }
    WorkerActionResult result = completed_result();
    // Every field below is the worker's own authenticated reading of the bundle
    // -- the geometry and the uuid from the inspection that verified it, and the
    // file identity from the handle it opened.  The manifest name and the
    // payload block size are the product constants the service already holds,
    // which is why they do not travel.
    const BundleFileIdentity file_identity{evidence.volume_serial, evidence.file_id};
    result.lifecycle.bundle = BundleRegistrationEvidence{
        image_id.value(),
        std::string(kProductManifestName),
        evidence.virtual_bytes,
        evidence.logical_sector_size,
        evidence.physical_sector_size,
        kProductPayloadBlockBytes,
        file_identity,
        source};
    // The generation this reply observed.  Captured here because this is the one
    // moment the worker is certainly the live authority on it: a stopped create's
    // session is dropped below, and the worker exits, so a later publication step
    // that asked again would be asking a process that has already left.
    result.lifecycle.bundle_generation = observed.value().committed_generation;
    // **A stopped create did not checkpoint, but its bundle still stands at
    // generation 1.**  Generation 0 is the empty base; the first checkpoint the
    // runtime can prepare advances 1 -> 2 (measured on the guest: prior=1,
    // confirmed=0, candidate=2 was exactly the refusal this caused).  Publishing
    // the reply's raw 0 would make every later checkpoint fail its boundary, so
    // the durable starting point is the first generation the image can commit.
    if (creating && request.command == ManagementCommand::create_stopped
        && result.lifecycle.bundle_generation == 0) {
        result.lifecycle.bundle_generation = 1;
    }
    // **The volume identity the create reply carries.**  A stopped create drops
    // its session below, so the service-side actions that follow have no worker
    // left to ask for this; capturing it here is what lets the profile become
    // `ready` with the volume it actually provisioned, and therefore what lets a
    // later `disk start` adopt that volume instead of provisioning a new one.
    if (observed.value().volume.has_value() &&
        observed.value().volume->complete()) {
        auto disk_id =
            product::Uuid::parse(observed.value().volume->disk_guid);
        auto partition_id =
            product::Uuid::parse(observed.value().volume->partition_guid);
        auto volume_id =
            product::Uuid::parse(observed.value().volume->volume_guid);
        if (disk_id.ok() && partition_id.ok() && volume_id.ok()) {
            result.lifecycle.volume = VolumeRegistrationEvidence{
                disk_id.value(), partition_id.value(), volume_id.value()};
        }
    }

    // **This is the moment the bundle's identity becomes knowable**, and therefore the
    // only moment it can be recorded: the worker has just read it off the handle it
    // opened.  Before this existed the identity lived only in the return value, so
    // every profile's leftovers were unprovable and the reclamation could never act.
    //
    // Best-effort, and deliberately not fatal.  The disk this operation just created
    // and verified is real and useful; failing it because a *fingerprint* could not be
    // written would trade a working disk for a bookkeeping problem.  What a failure
    // costs is bounded and visible: the profile's leftovers become unprovable, and the
    // next start's sweep reports that instead of reclaiming them.
    if (fingerprints_ != nullptr) {
        static_cast<void>(
            fingerprints_->amend_bundle_identity(request.profile_id, file_identity));
    }

    return Result<WorkerActionResult>::success(std::move(result));
}

Result<WorkerActionResult> ServiceWorkerControl::reclaim_leftover(
    const WorkerActionRequest& request) {
    std::unique_lock lock(command_gate_);
    return reclaim_leftover_locked(request);
}

Result<WorkerActionResult> ServiceWorkerControl::reclaim_leftover_locked(
    const WorkerActionRequest& request) {
    // The **durable profile** is the input, not the lifecycle intent, and the difference
    // matters: an intent is what a *command* carries before a profile exists, while a
    // leftover is by definition a profile that has already been published.  It also avoids
    // a conversion this layer does not have -- there is no profile-to-intent helper, and
    // inventing one here to satisfy a parameter shape would be a second translation of the
    // same facts.
    if (!request.durable_profile.has_value()) {
        // No profile means no bundle path, no mount target and no image -- and a reclaim
        // without those could only guess which volume to dismantle, which is the
        // identity-free cleanup the design forbids outright.
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.reclaim_leftover",
            L"A leftover reclaim needs the profile it is reclaiming for"));
    }
    auto launch_request = make_launch_request(request.durable_profile.value());
    if (!launch_request.ok()) {
        return Result<WorkerActionResult>::failure(launch_request.error());
    }
    // **The mode is overridden here, and that is the whole point of this method.**  The
    // translation above picks `provision_new`/`adopt_existing` from the profile's
    // lifecycle, and both of those *bring a volume into service*.  A reclaim must take a
    // leftover **out** of service, which means adopting the already-mounted volume and
    // assigning nothing -- `reclaim_existing`.  Leaving the translated mode in place would
    // have this method mount the very volume it was called to dismantle.
    launch_request.value().volume_mode = m5::WorkerVolumeMode::reclaim_existing;

    auto launched = sessions_.launch(launch_request.value(), reclaim_context());
    if (!launched.ok()) {
        return Result<WorkerActionResult>::failure(launched.error());
    }
    auto owned = launched.move_value();
    auto* const session = owned.get();

    // `start` performs the adoption: the runtime finds the volume by the recorded letter
    // and attaches without assigning anything.  A failure here is reported rather than
    // retried, because a volume that cannot be attached is one this service may not have
    // owned in the first place.
    //
    // Both commands take the reclaim budget rather than the ordinary one, because the
    // adoption is the expensive part and it happens inside this pair.
    const auto adopted = session->start(reclaim_context(), request.operation_id);
    if (!adopted.ok()) {
        return Result<WorkerActionResult>::failure(adopted.error());
    }
    // The teardown, which is the same call every other path uses.
    const auto reclaimed =
        session->reclaim_leftover(reclaim_context(), request.operation_id);
    if (!reclaimed.ok()) {
        return Result<WorkerActionResult>::failure(reclaimed.error());
    }

    WorkerActionResult result = completed_result();
    // No evidence is produced, and that is deliberate rather than an omission: a reclaim
    // *removes* durable facts about a resource, so answering with a
    // `VolumeRegistrationEvidence` would be the service recording a volume it has just
    // dismantled.  The caller's proof that it worked is the reply's status, which reports
    // the runtime with no volume -- and, in the acceptance, the drive letter being gone.
    return Result<WorkerActionResult>::success(std::move(result));
}

Result<WorkerActionResult> ServiceWorkerControl::execute(
    const WorkerActionRequest& request) {
    std::unique_lock lock(command_gate_);
    switch (request.action) {
    case ActionKind::create_and_verify_bundle:
        return register_bundle(request, BundleEvidenceSource::created_and_verified);
    case ActionKind::inspect_bundle_read_only:
        return register_bundle(request, BundleEvidenceSource::read_only_inspection);
    case ActionKind::create_runtime_and_worker:
        return bind_worker(request, true);
    case ActionKind::recover_bundle:
        return bind_worker(request, false);
    case ActionKind::prove_new_uninitialized:
        // A service-side preflight with no worker in it: the durable profile's
        // mount target is checked before a later start, and the check is that
        // the plan reached this step at all with a profile in hand.
        if (!request.durable_profile.has_value()) {
            return Result<WorkerActionResult>::failure(control_error(
                ServiceWorkerControlError::invalid_argument,
                L"service_worker_control.prove_new_uninitialized",
                L"The preflight needs the operation's durable profile"));
        }
        return Result<WorkerActionResult>::success(completed_result());
    case ActionKind::start_proxy:
    case ActionKind::create_exact_device:
    case ActionKind::locate_exact_physical_drive:
    case ActionKind::provision_gpt_ntfs:
    case ActionKind::verify_existing_volume_identity:
    case ActionKind::record_volume_identity:
    case ActionKind::assign_exact_mount:
    case ActionKind::readiness_io:
        return verify_startup_step(request);
    case ActionKind::quiesce_mutations:
        return quiesce_mutations(request);
    case ActionKind::freeze_writes:
        return freeze_writes(request);
    case ActionKind::flush_exact_volume:
        return flush_exact_volume(request);
    case ActionKind::resume_mutations:
        return resume_mutations(request);
    case ActionKind::remove_mount:
        return remove_mount(request);
    case ActionKind::remove_exact_device:
    case ActionKind::stop_runtime_and_release_lock:
        return confirm_removal(request);
    case ActionKind::reclaim_leftover:
        // Explicit rather than left to a catch-all, and that is not style: a
        // catch-all answered `unrealized`, so a new action kind that reached it
        // was reported as "no worker did this" and a leftover was never
        // dismantled -- silently, while every test that did not exercise it
        // still passed.
        // The gate is already held by `execute`, so the body is called directly: taking
        // it again on this thread would deadlock, which is exactly why the body is its
        // own method.
        return reclaim_leftover_locked(request);
    case ActionKind::none:
    case ActionKind::publish_provisioning_profile:
    case ActionKind::publish_imported_ready_profile:
    case ActionKind::record_created_volume_identity:
    case ActionKind::checkpoint:
    case ActionKind::checkpoint_if_required:
    case ActionKind::publish_ready_profile:
    case ActionKind::compact:
    case ActionKind::publish_live_config:
    case ActionKind::publish_stopped_config:
    case ActionKind::repair_profile_from_previous:
    case ActionKind::verify_rebind_bundle:
    case ActionKind::publish_rebind:
    case ActionKind::forget_metadata:
    case ActionKind::resolve_commit:
    case ActionKind::read_status:
    case ActionKind::read_config:
    case ActionKind::collect_diagnostics:
    case ActionKind::read_operation:
    case ActionKind::request_operation_cancel:
        // Plan steps no worker executes: the service performs them, or the
        // checkpoint path owns them.  They are listed one by one, and the switch
        // has no `default:`, so a new action kind is a compile error here
        // (C4062, promoted below) instead of silently answering "no worker did
        // this".
        return unrealized(request);
    }
    // Unreachable for any enumerator of ActionKind.  A new kind that was added
    // without being classified lands here too, which is why the message names
    // both possibilities: either the request is corrupt, or the action was
    // never assigned to "a worker realizes it" or "the service owns it".  It is
    // deliberately not `unrealized_action`: that code means "a plan step no
    // worker performs", and answering it for an unknown kind is how a leftover
    // once went un-dismantled.
    return Result<WorkerActionResult>::failure(control_error(
        ServiceWorkerControlError::invalid_argument,
        L"service_worker_control.execute",
        L"The action is neither realized by a worker nor classified as "
        L"service-owned"));
}

Result<WorkerActionResult> ServiceWorkerControl::checkpoint_across_boundary(
    const WorkerActionRequest& request,
    const CheckpointCommitBoundaryHandler& boundary) {
    if (!request.durable_profile.has_value()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.checkpoint",
            L"The checkpoint needs the operation's durable profile"));
    }
    auto session = require_session(request.profile_id, true);
    if (!session.ok()) {
        return Result<WorkerActionResult>::failure(session.error());
    }
    auto bound = verify_binding(request, *session.value());
    if (!bound.ok()) return bound;

    // Every plan that reaches a checkpoint reaches it through flush_exact_volume
    // first, and that flush holds the candidate this commit publishes.  Cutting
    // a fresh one here would commit a snapshot taken after ServiceCore's durable
    // compare-and-swap decision, which is exactly the ordering the boundary
    // protocol exists to prevent.
    auto held = take_candidate(request.profile_id, request.operation_id);
    if (!held.has_value()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::checkpoint_candidate_missing,
            L"service_worker_control.checkpoint",
            L"No flushed candidate is held for this operation to commit"));
    }
    const auto evidence = held->evidence;

    auto decision = boundary(evidence);
    if (!decision.ok()) {
        // The service could not decide, so nothing may be published.  The
        // candidate is still resolved rather than left outstanding, and the
        // decision's own error is what is reported.
        auto aborted = abort_held_candidate(*session.value(), held.value());
        if (!aborted.ok()) {
            return Result<WorkerActionResult>::failure(aborted.error());
        }
        return Result<WorkerActionResult>::failure(decision.error());
    }
    if (!decision.value().continue_commit) {
        // ServiceCore withdrew the commit, which is what a cancel arriving
        // before the boundary produces.  The candidate is aborted, so nothing
        // was published and the committed generation is unchanged.
        auto aborted = abort_held_candidate(*session.value(), held.value());
        if (!aborted.ok()) {
            return Result<WorkerActionResult>::failure(aborted.error());
        }
        WorkerActionResult result = completed_result();
        result.checkpoint = WorkerCheckpointEvidence{
            evidence,
            CheckpointCommitDisposition::not_committed,
            evidence.prior_generation,
            evidence.cut_dirty_epoch,
            std::nullopt,
            std::nullopt};
        return Result<WorkerActionResult>::success(std::move(result));
    }

    // The commit repeats exactly what prepare froze -- the native id, the slot
    // and the digest -- because the worker refuses anything else and leaves the
    // candidate prepared.  The slot and digest come from the held candidate
    // rather than from the boundary evidence, which does not carry them.
    // The commit finalises the journal and publishes the manifest, which is bounded by the same
    // dirty set the cut had to write, so it uses the same ceiling (see `cut_candidate`).
    auto committed = session.value()->commit_checkpoint(
        provisioning_context(), request.operation_id,
        m5::WorkerCheckpointCommit{
            held->prepared.native_operation_id,
            held->prepared.manifest_slot,
            held->prepared.candidate_bytes_digest});
    if (!committed.ok()) {
        // The transport failed, so the commit's fate is not knowable here.  It
        // is reported as a failure and ServiceCore parks the operation for
        // reconciliation; guessing either way would destroy the outcome that
        // reconciliation exists to resolve.
        return Result<WorkerActionResult>::failure(committed.error());
    }
    const auto& reply = committed.value();
    const auto disposition = to_management_disposition(reply.disposition);
    if (!disposition.has_value()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.checkpoint",
            L"The worker reported an unknown commit disposition"));
    }
    if (is_commit_landed(disposition.value())
        && reply.failure.has_value()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.checkpoint",
            L"A committed checkpoint reported failure evidence"));
    }
    if (!is_commit_landed(disposition.value()) && !reply.failure.has_value()) {
        // Not-committed and indeterminate both require the persistence failure
        // that decided them.  Without it ServiceCore would reject the evidence
        // anyway, so it is refused here with the reason named rather than
        // handed on to fail somewhere less informative.
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.checkpoint",
            L"A commit that did not land reported no failure evidence"));
    }

    // The epoch the commit is observed at.  The cut epoch is a live observation
    // the worker already made and this status read is a later one; the larger of
    // two real observations is taken, which can only make a later checkpoint
    // more likely to be required, never less.
    std::uint64_t observed_dirty_epoch = evidence.cut_dirty_epoch;
    observed_dirty_epoch = (std::max)(observed_dirty_epoch,
                                      reply.status.dirty_epoch);

    std::optional<Error> commit_error;
    std::optional<CheckpointPersistenceErrorContext> commit_context;
    if (reply.failure.has_value()) {
        const auto& failure = *reply.failure;
        commit_error = Error{
            ErrorDomain::persistence,
            failure.code,
            std::wstring(failure.operation.begin(), failure.operation.end()),
            L"The worker checkpoint commit did not land",
            failure.native_code};
        commit_context =
            to_management_context(failure, disposition.value());
    }

    WorkerCheckpointEvidence observed{
        evidence,
        disposition.value(),
        // The generation is relayed exactly as the worker reported it, never
        // adjusted to satisfy a validator: a generation rewritten to fit would
        // describe a commit that did not happen.
        reply.generation,
        observed_dirty_epoch,
        std::move(commit_error),
        std::move(commit_context)};

    WorkerActionResult result = completed_result();
    if (is_commit_landed(disposition.value())) {
        // A landed commit publishes a new generation, and this adapter is the
        // authority that performed it -- so it is a generation this authority
        // witnessed.  A stop that quiesced before its checkpoint is holding a
        // removal witness at the pre-commit generation and would have its
        // removal refused, so the witness moves with the commit.
        advance_held_removal(request.profile_id, request.operation_id,
                             reply.generation);
        // The provisioning start path requires the first checkpoint to be
        // acknowledged by the worker, and a landed commit is that
        // acknowledgement.
        result.lifecycle.first_checkpoint_acknowledged = true;
    }
    result.checkpoint = std::move(observed);
    return Result<WorkerActionResult>::success(std::move(result));
}

Result<WorkerActionResult> ServiceWorkerControl::execute_checkpoint(
    const WorkerActionRequest& request,
    const CheckpointCommitBoundaryHandler& boundary) {
    std::unique_lock lock(command_gate_);
    return checkpoint_across_boundary(request, boundary);
}

Result<WorkerActionResult> ServiceWorkerControl::reconcile_checkpoint(
    const WorkerActionRequest& request,
    const CheckpointCommitBoundaryEvidence& correlation) {
    std::unique_lock lock(command_gate_);
    // A correlation recorded before the manifest slot and the digest existed
    // cannot support a proof, and this is where that has to be said rather than
    // discovered.  It is not a match against zeroes: it is the absence of the
    // two facts the question is made of.
    if (!correlation.recovery_recoverable()) {
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::recovery_not_provable,
            L"service_worker_control.reconcile_checkpoint",
            L"The durable correlation carries no manifest slot and digest, so "
            L"ownership of this operation cannot be proven"));
    }
    if (!correlation.native_m3_operation_id.has_value()) {
        // Without the native id the bundle has nothing to match its journal
        // against, so a proof would be answering a different question.
        return Result<WorkerActionResult>::failure(control_error(
            ServiceWorkerControlError::recovery_not_provable,
            L"service_worker_control.reconcile_checkpoint",
            L"The durable correlation carries no native operation id"));
    }

    // **The inspection may have to be launched, because after P4 there is often
    // nothing left to attach to.**
    //
    // This used to be `require_session(profile_id, true)`, which adopts a live
    // runtime or fails `execution_not_bound`.  That is exactly the state this
    // question is about: a commit that reached its durable boundary and whose
    // worker then went away.  No runtime is left to adopt, so the one path that
    // must answer could never run -- which is why the recovery table's positive
    // arms had never been executed once.
    //
    // Launching is safe for the reason `prove_forget_safe` gives: a launch
    // authenticates and opens the bundle, and *starting* the runtime is a separate
    // command this function never issues.  The worker enforces that ordering
    // itself, and `DiskWorkerRuntime::prove_recovery` is documented as callable
    // before `start()`.  The inspection answers one question and is then closed in
    // every branch, because a session left open is a worker nobody commands and a
    // bundle nobody can take for writing.
    auto* const held = find_session(request.profile_id);
    std::unique_ptr<ServiceWorkerSession> inspection;
    ServiceWorkerSession* session = held;
    std::uint32_t inspection_pid{};
    std::uint64_t inspection_created{};
    if (session == nullptr) {
        if (!request.durable_profile.has_value()) {
            return Result<WorkerActionResult>::failure(control_error(
                ServiceWorkerControlError::invalid_argument,
                L"service_worker_control.reconcile_checkpoint",
                L"The reconciliation needs the operation's durable profile to "
                L"launch an inspection"));
        }
        auto launch_request =
            make_launch_request(request.durable_profile.value());
        if (!launch_request.ok()) {
            return Result<WorkerActionResult>::failure(launch_request.error());
        }
        auto launched = sessions_.launch(launch_request.value(), context());
        if (!launched.ok()) {
            // A launch that could not be completed is not an answer about the
            // bundle.  It is returned as a failure so the caller reconciles; it
            // must never become "the commit did not happen".
            return Result<WorkerActionResult>::failure(launched.error());
        }
        inspection = launched.move_value();
        const auto binding = inspection->binding();
        inspection_pid = binding.process_id;
        inspection_created = binding.process_creation_time;
        session = inspection.get();
    }
    auto bound = verify_binding(request, *session);
    if (!bound.ok()) return bound;

    // The correlation the worker judges against is the durable record, handed
    // over unchanged.  The worker re-derives nothing from the service's claim
    // and the service derives nothing from the worker's: the bundle decides.
    // Aggregate initialization: the product operation id is a `product::Uuid`,
    // which has no public default constructor, so the struct cannot be
    // value-initialized and then filled in.
    const m5::WorkerRecoveryCorrelation wire{
        correlation.product_operation_id,
        persistence::CheckpointRecoveryCorrelation{
            to_image_uuid(correlation.image_id),
            correlation.prior_generation,
            correlation.candidate_generation,
            *correlation.native_m3_operation_id,
            correlation.manifest_slot,
            correlation.candidate_bytes_digest}};

    auto proof = session->recovery_proof(context(), wire);
    if (inspection) {
        // Closing the channel is what ends an inspection: no runtime was started,
        // so there is nothing to stop and the worker leaves on its next read.  The
        // wait is not allowed to turn into a finding, but a worker left running
        // would be the next operation's problem, so it is released before any
        // answer is returned.
        inspection.reset();
        (void)worker_process_gone(inspection_pid, inspection_created, 5000);
    }
    if (!proof.ok()) {
        // The proof could not be taken at all -- an unreadable bundle, a
        // transport failure.  That is not one of the three answers, and
        // reporting it as `ownership_unprovable` would turn "could not ask" into
        // a finding about the bundle.
        return Result<WorkerActionResult>::failure(proof.error());
    }

    // What the bundle proved, relayed.  Which durable state that implies -- and
    // whether the operation is promoted, rolled back or parked -- is ServiceCore's
    // decision, not this adapter's; inventing one here would be deciding the
    // recovery table in the wrong layer.
    WorkerActionResult result = completed_result();
    switch (proof.value().outcome) {
    case m5::WorkerRecoveryOutcome::prior_generation_proved:
        result.checkpoint = WorkerCheckpointEvidence{
            correlation,
            CheckpointCommitDisposition::not_committed,
            proof.value().observed_generation,
            correlation.cut_dirty_epoch,
            std::nullopt,
            std::nullopt};
        return Result<WorkerActionResult>::success(std::move(result));

    case m5::WorkerRecoveryOutcome::candidate_generation_proved:
        // The bytes this operation cut are the ones on disk.  That is a landed
        // commit and nothing weaker; whether its epoch was published is a
        // separate question the proof does not answer, so it is not claimed.
        result.checkpoint = WorkerCheckpointEvidence{
            correlation,
            CheckpointCommitDisposition::committed,
            proof.value().observed_generation,
            correlation.cut_dirty_epoch,
            std::nullopt,
            std::nullopt};
        result.lifecycle.first_checkpoint_acknowledged = true;
        return Result<WorkerActionResult>::success(std::move(result));

    case m5::WorkerRecoveryOutcome::ownership_unprovable:
        break;
    }
    // The bundle supports neither claim.  Reported as indeterminate rather than
    // as a rollback: "not ours" is not the same as "did not happen", and the one
    // answer that must never be given here is that nothing was published.
    //
    // The generation is the worker's own observation of what is on disk, not a
    // value derived from what this service expected to find.  Relaying a number
    // the adapter computed would make an unprovable bundle look like a measured
    // one.
    result.completion = CompletionOutcome::resolution_unresolved;
    result.checkpoint = WorkerCheckpointEvidence{
        correlation,
        CheckpointCommitDisposition::commit_indeterminate,
        proof.value().observed_generation,
        correlation.cut_dirty_epoch,
        std::nullopt,
        std::nullopt};
    return Result<WorkerActionResult>::success(std::move(result));
}

Result<void> ServiceWorkerControl::publish_checkpoint_epoch(
    const WorkerActionRequest& request,
    const CheckpointCommitBoundaryEvidence& correlation,
    const CheckpointEpochPublicationHandler& publish) {
    std::unique_lock lock(command_gate_);
    // The fence runs *inside* the worker's own commit, around the publication
    // of the durable epoch, so that a service decision cannot land between the
    // bundle's snapshot and the record that names it.  Reproducing it here would
    // be a fence around nothing: by the time this adapter has the reply, the
    // worker has already published.
    //
    // So the fence is invoked exactly once, immediately, and its verdict is what
    // decides whether this service accepts the commit the worker already made.
    // A fence that refuses turns a landed commit into one this service will not
    // record, which is the reconciliation path rather than a silent acceptance.
    if (!publish) {
        // No fence wired is not a licence to skip it: an unwired publication
        // boundary means the caller cannot be told the epoch moved.
        return Result<void>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.publish_checkpoint_epoch",
            L"The dirty-epoch fence was not supplied"));
    }
    // **Cleanliness is a fact about the worker's blocks, not an ordering.**
    // A successful commit resets the dirty epoch, so `live == cut` was false
    // after every real commit -- which is why every `stop` was classified dirty
    // and could never complete.  The worker that just committed is asked what it
    // still holds; "could not ask" is a failure here, never "clean".
    auto session = require_session(request.profile_id, true);
    if (!session.ok()) {
        return Result<void>::failure(session.error());
    }
    auto status = session.value()->require_runtime_observation(context());
    if (!status.ok()) {
        return Result<void>::failure(status.error());
    }
    return publish(CheckpointEpochObservation{
        correlation, status.value().dirty_epoch, status.value().dirty});
}

namespace {

// A pid is not an identity: Windows reuses it.  The binding's creation time is
// what makes "the worker I launched" a question with an answer, and the same
// comparison keeps a recycled pid from being waited on for five seconds and then
// reported as a worker that would not leave.
bool worker_process_gone(const std::uint32_t process_id,
                         const std::uint64_t process_creation_time,
                         const DWORD timeout_ms) {
    HANDLE process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) return true;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    ULARGE_INTEGER creation{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        creation.LowPart = created.dwLowDateTime;
        creation.HighPart = created.dwHighDateTime;
    }
    if (creation.QuadPart != process_creation_time) {
        static_cast<void>(CloseHandle(process));
        return true;
    }
    const bool gone = WaitForSingleObject(process, timeout_ms) == WAIT_OBJECT_0;
    static_cast<void>(CloseHandle(process));
    return gone;
}

} // namespace

Result<ForgetSafetyEvidence> ServiceWorkerControl::prove_forget_safe(
    const WorkerActionRequest& request) {
    std::unique_lock lock(command_gate_);
    if (!request.durable_profile.has_value()) {
        return Result<ForgetSafetyEvidence>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.prove_forget_safe",
            L"The forget proof needs the operation's durable profile"));
    }
    // A worker bound to this profile is the one thing that could still hold the
    // bundle, so a proof taken beside it would be about a state that is already
    // over.  The check is the session table, which is the only record of a worker
    // this service can command.
    if (find_session(request.profile_id) != nullptr) {
        return Result<ForgetSafetyEvidence>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.prove_forget_safe",
            L"A worker is still bound to this profile"));
    }
    auto launch_request = make_launch_request(request.durable_profile.value());
    if (!launch_request.ok()) {
        return Result<ForgetSafetyEvidence>::failure(launch_request.error());
    }
    auto launched = sessions_.launch(launch_request.value(), context());
    if (!launched.ok()) {
        return Result<ForgetSafetyEvidence>::failure(launched.error());
    }
    auto owned = launched.move_value();
    const auto binding = owned->binding();
    // The inspection is the one command that opens the bundle without starting a
    // runtime, and it answers with the two absences a running runtime would
    // contradict.  It is also the only command available here: an inspection
    // session is not kept, so there is no second exchange to ask in.
    auto observed = owned->inspect_bundle(context(), request.operation_id);
    if (!observed.ok()) {
        return Result<ForgetSafetyEvidence>::failure(observed.error());
    }
    const auto& status = observed.value();
    if (!status.forget_probe.has_value() || !status.bundle.has_value()
        || !status.bundle->complete()) {
        return Result<ForgetSafetyEvidence>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.prove_forget_safe",
            L"The inspection did not answer the forget proof"));
    }
    auto image_id = to_product_uuid(status.image_id);
    if (!image_id.has_value()) {
        return Result<ForgetSafetyEvidence>::failure(control_error(
            ServiceWorkerControlError::worker_reply_unexpected,
            L"service_worker_control.prove_forget_safe",
            L"The inspected image identity is not a canonical uuid"));
    }
    BundleFileIdentity identity{};
    identity.volume_serial = status.bundle->volume_serial;
    identity.file_id = status.bundle->file_id;
    // Aggregate-initialised: the two ids have no default constructor, so a
    // default-constructed evidence could not be filled in afterwards.
    ForgetSafetyEvidence evidence{
        request.profile_id,
        image_id.value(),
        request.durable_profile->revision,
        false,
        status.forget_probe->writer_lock_absent,
        status.forget_probe->matching_device_absent,
        // The worker opened it and read its identity; that is what "present"
        // means here, and the identity travels with it so ServiceCore can
        // compare it with the one the tombstone recorded.
        true,
        identity};

    // Closing the channel is what ends an inspection: no runtime was started, so
    // there is nothing to stop, and the worker leaves on its next read.  It is
    // waited for rather than assumed, because "absent" is the fact being
    // certified.
    owned.reset();
    evidence.worker_absent = worker_process_gone(
        binding.process_id, binding.process_creation_time, 5000);
    return Result<ForgetSafetyEvidence>::success(evidence);
}

Result<std::optional<std::uint64_t>> ServiceWorkerControl::current_dirty_epoch(
    const product::Uuid& profile_id) {
    // **Never waits for the channel**, for the same reason and in the same shape as
    // `observe_runtime` below: the gate is *tried*, not taken.  A read that queued here
    // holds `command_gate_` while the command it is queued behind needs that same gate
    // to finish -- and a service process in that state never reaches the terminal state
    // of the operation it is answering.  Measured on the guest: `disk show` during a
    // `create --no-start` and a worker that answers every command, with
    // `execute_operation` never returning.
    //
    // `nullopt` says "not observed right now".  It is the honest answer for a read path:
    // the durable snapshot is still reported, and what must never happen is reporting
    // `dirty_epoch = 0` (or calling the disk clean) because the question could not be
    // asked.  Callers that need the strict answer for a mutation use
    // `require_runtime_observation`, which does queue.
    std::unique_lock lock(command_gate_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    // `find_session`, not `require_session(..., true)`: asking for an epoch must not
    // adopt or launch a worker.  A profile this authority holds no session for has no
    // live epoch it can read, and a read path that launched a worker to answer would
    // turn `disk list` into a writer.
    auto* const session = find_session(profile_id);
    if (session == nullptr) {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    // Strict about the *answer*: a transport failure is a fact about the channel, not
    // an absence of truth, so it is still returned as a failure.  Callers on the read
    // path already treat a failure as "no live epoch to add".
    auto status = session->require_runtime_observation(context());
    if (!status.ok()) {
        return Result<std::optional<std::uint64_t>>::failure(status.error());
    }
    return Result<std::optional<std::uint64_t>>::success(
        status.value().dirty_epoch);
}

Result<void> ServiceWorkerControl::export_bundle(
    const profile::ProfileRecord& profile,
    const std::wstring& destination) {
    if (destination.empty()) {
        return Result<void>::failure(control_error(
            ServiceWorkerControlError::invalid_argument,
            L"service_worker_control.export_bundle",
            L"The export destination is empty"));
    }
    std::unique_lock lock(command_gate_);
    auto launch = make_launch_request(profile);
    if (!launch.ok()) return Result<void>::failure(launch.error());
    // The bundle already exists and is only read.  `provision_new` is the mode a
    // create uses to write generation zero; an export must never take it.
    launch.value().volume_mode = m5::WorkerVolumeMode::adopt_existing;
    auto launched = sessions_.launch(launch.value(), context());
    if (!launched.ok()) return Result<void>::failure(launched.error());
    auto owned = launched.move_value();
    auto operation_id = product::Uuid::random();
    if (!operation_id.ok()) {
        return Result<void>::failure(operation_id.error());
    }
    auto exported =
        owned->export_bundle(context(), operation_id.value(), destination);
    const auto binding = owned->binding();
    owned.reset();
    // Closing the channel is what ends the inspection worker; the export has no
    // runtime that needs stopping.  Wait for the process so a leftover worker is
    // not the next command's problem.
    (void)worker_process_gone(
        binding.process_id, binding.process_creation_time, 5000);
    if (!exported.ok()) return Result<void>::failure(exported.error());
    return Result<void>::success();
}

Result<std::optional<WorkerRuntimeObservation>>
ServiceWorkerControl::observe_runtime(const product::Uuid& profile_id) {
    // **Never waits for the channel.**  A worker's command pipe is one request/reply
    // sequence, so a status query issued while a create is being executed would sit
    // behind that create for as long as it runs -- minutes -- and `disk list` with it.
    // T1-04's requirement is that a list answers inside a second during a create, so the
    // gate is *tried*: if this adapter is mid-command, nothing is known about the runtime
    // right now, and `nullopt` says so.
    //
    // `nullopt` is a weaker claim than an observation, not a wrong one: the read path
    // reports the durable record and does not fail closed on it.  What it must never do is
    // claim `worker_alive == false`, which is why this is not an empty observation -- an
    // observation that says the worker is gone would fail a running disk closed.
    std::unique_lock lock(command_gate_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return Result<std::optional<WorkerRuntimeObservation>>::success(
            std::nullopt);
    }
    auto* const session = find_session(profile_id);
    if (session == nullptr) {
        // No worker this authority can see is an observation -- "observed to be
        // absent" -- and it is the answer the read path already knows how to
        // handle.  Reporting `nullopt` here would say "not observed", which is a
        // weaker claim than the one this authority can actually make about a
        // profile it holds no session for.
        WorkerRuntimeObservation absent{};
        absent.worker_alive = false;
        return Result<std::optional<WorkerRuntimeObservation>>::success(absent);
    }
    auto status = session->status(context());
    if (!status.ok()) {
        // The transport failed, so nothing is known.  `nullopt` is the honest
        // answer, and it is why this method is allowed to return one.
        return Result<std::optional<WorkerRuntimeObservation>>::success(
            std::nullopt);
    }
    return Result<std::optional<WorkerRuntimeObservation>>::success(
        observe_worker_runtime(status.value(), true));
}

Result<WorkerRuntimeObservation> ServiceWorkerControl::require_runtime_observation(
    const product::Uuid& profile_id) {
    // Strict, so it *does* wait: a caller deciding whether a mutation is safe must not be
    // told "could not ask" when the answer is "the channel is busy".  Queueing here is
    // correct precisely because the caller is a mutation, and mutations are serialized by
    // the execution gate anyway.
    std::unique_lock lock(command_gate_);
    auto session = require_session(profile_id, true);
    if (!session.ok()) {
        return Result<WorkerRuntimeObservation>::failure(session.error());
    }
    auto status = session.value()->require_runtime_observation(context());
    if (!status.ok()) {
        return Result<WorkerRuntimeObservation>::failure(status.error());
    }
    return Result<WorkerRuntimeObservation>::success(
        observe_worker_runtime(status.value(), true));
}

} // namespace myrd::management
