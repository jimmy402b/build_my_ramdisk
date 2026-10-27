#pragma once

// The private service <-> per-profile worker transport (solution §2.2-§2.4).
//
// This is deliberately not the public pipe.  The public pipe is reachable by an
// unprivileged client and carries product commands that never touch a bundle;
// this channel carries an authenticated worker binding on every message and its
// ACL admits only the service and the one worker it launched.  Nothing here is
// shared with `src/control`, and the private protocol is not a mode of the
// public one.
//
// Two rules shape the code below:
//
//   1. The startup secret never appears on a command line.  The service writes
//      the entire `WorkerLaunchSpec` -- secret included -- into an inheritable
//      anonymous pipe and puts only the *handle value* on the child's command
//      line, which is what solution §2.3 permits and nothing more.
//   2. The worker may not open the bundle until the handshake has succeeded.
//      `WorkerPrivatePipeClient` therefore exposes `handshake()` as a distinct,
//      mandatory step rather than folding it into `connect()`, so a caller
//      cannot reach the runtime by accident on a channel that was never
//      authenticated.

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/host/m5_worker_protocol.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::m5 {

// What the worker's runtime must do with the volume behind its bundle.
//
// The line is already drawn by the profile's own lifecycle -- `append_start_actions`
// branches on exactly it, and `provisioning` means "this profile has a bundle but
// has never had a volume" -- so this restates that criterion rather than
// inventing a second one.  `adopt_existing` is the zero value deliberately: a
// launch that failed to carry a mode must not be the kind that creates anything.
enum class WorkerVolumeMode : std::uint8_t {
    adopt_existing = 0,
    provision_new,
    // Adopt an **already-mounted** volume and tear it down, assigning nothing.
    //
    // It is appended rather than inserted, and that is not tidiness: the value is on the
    // wire, so inserting one would silently reinterpret `provision_new` for every peer
    // that speaks the older encoding.
    //
    // It exists as a third mode rather than a flag on `adopt_existing` because the two
    // are opposites in what they do to the machine: `adopt_existing` brings a volume into
    // service, this one takes a leftover out of it, and the whole reason this mode is
    // needed is that the second must not assign a drive letter.  A launch that carried
    // the wrong one would mount where it was asked to dismantle.
    reclaim_existing,
};

// Everything one worker needs to know, delivered once through the inherited
// startup channel.
//
// It carries the private pipe name rather than deriving it in both places: a
// name computed twice is a name that can disagree, and a worker that connects to
// a name it derived itself could be steered by whoever owns that name.
struct WorkerLaunchSpec final {
    // The two ids have no default constructor, so they are constructor
    // parameters rather than fields a caller could leave unset.
    WorkerLaunchSpec(product::Uuid profile_id,
                     std::uint64_t profile_revision) noexcept
        : profile_id(std::move(profile_id)),
          profile_revision(profile_revision) {}

    product::Uuid profile_id;
    std::uint64_t profile_revision{};
    std::wstring bundle_path;
    // The image this bundle holds, or must be created to hold.
    //
    // `provision_new`: the uuid generation zero is created *with*, which is how
    // the service's derived image id and the bundle's actual one stay one fact
    // rather than two that have to agree later.  `adopt_existing`: the identity
    // the bundle must already have, checked against the bundle's own bytes --
    // a claim to verify, not to take on trust.  Absent only for an import, where
    // the service is recording what the bundle turns out to hold instead of
    // naming it in advance.
    std::optional<persistence::ImageUuid> image_id;
    std::uint64_t logical_size_bytes{};
    std::wstring mount_target;
    // The volume the durable profile recorded for this bundle.  A restart of a
    // stopped disk has to recognise that volume with no drive letter, and the
    // letter is the only thing select_volume_for_disk looks at first.
    std::wstring volume_guid;
    // What the runtime does with the volume.  Declared here rather than beside
    // `image_id` so the fields the launch already had keep their encoded order.
    WorkerVolumeMode volume_mode{WorkerVolumeMode::adopt_existing};
    std::uint64_t session_generation{};
    std::wstring private_pipe_name;
    std::uint64_t session_nonce{};
    StartupSecret startup_secret{};
    // The label the volume should carry (T4-02).  Declared last so the fields the launch already had keep their
    // encoded order; the codec writes it as an appended tag, which needs no protocol minor bump because the
    // service and its worker ship together.
    std::optional<std::wstring> volume_label;

    bool operator==(const WorkerLaunchSpec&) const noexcept = default;
};

[[nodiscard]] Result<std::vector<std::byte>> encode_launch_spec(
    const WorkerLaunchSpec& spec);
[[nodiscard]] Result<WorkerLaunchSpec> decode_launch_spec(
    std::span<const std::byte> bytes);

// One name per profile *and* per session: a recycled name would let a previous
// session's instance be mistaken for this one.
[[nodiscard]] std::wstring private_pipe_name(const product::Uuid& profile_id,
                                             std::uint64_t session_generation);

// ---------------------------------------------------------------------------
// Startup channel
// ---------------------------------------------------------------------------
// An anonymous pipe with exactly one inheritable end.  The parent keeps the
// write end, the child inherits the read end, and the spec goes through it.

struct StartupChannel final {
    // The end the child inherits.  Its value is the only thing that may appear
    // on the child's command line.
    void* child_read_handle{};
    // The end the service writes the spec into.  Closed immediately after.
    void* parent_write_handle{};
};

[[nodiscard]] Result<StartupChannel> create_startup_channel();
// Writes the spec and closes the writing end, so the child's read returns EOF
// rather than waiting for a message that will never come.
[[nodiscard]] Result<void> write_launch_spec(StartupChannel& channel,
                                             const WorkerLaunchSpec& spec);
[[nodiscard]] Result<WorkerLaunchSpec> read_launch_spec(void* inherited_handle);
void close_startup_handle(void* handle) noexcept;

// ---------------------------------------------------------------------------
// Private pipe
// ---------------------------------------------------------------------------

// The framed duplex channel.  A frame is a 4-byte little-endian length followed
// by exactly that many message bytes; the receiver refuses a length of zero or
// one above `kMaxWorkerMessageBytes` before it allocates anything.
class WorkerPipeChannel final {
public:
    ~WorkerPipeChannel() noexcept;
    WorkerPipeChannel(WorkerPipeChannel&&) noexcept;
    WorkerPipeChannel& operator=(WorkerPipeChannel&&) noexcept;
    WorkerPipeChannel(const WorkerPipeChannel&) = delete;
    WorkerPipeChannel& operator=(const WorkerPipeChannel&) = delete;

    [[nodiscard]] Result<void> write(const OperationContext& context,
                                     const WorkerMessage& message);
    [[nodiscard]] Result<WorkerMessage> read(const OperationContext& context);
    void close() noexcept;

private:
    friend class WorkerPrivatePipeServer;
    friend class WorkerPrivatePipeClient;
    explicit WorkerPipeChannel(void* handle) noexcept;

    void* handle_{};
};

// The service side: creates the instance, waits for the worker, and completes
// the handshake before handing back a channel that is safe to command.
class WorkerPrivatePipeServer final {
public:
    // Fails when an instance with this name already exists: `FILE_FLAG_FIRST_PIPE_INSTANCE`
    // is set so a stale or squatted name is an error rather than something this
    // session silently connects to.
    [[nodiscard]] static Result<std::unique_ptr<WorkerPrivatePipeServer>> create(
        std::wstring pipe_name);
    ~WorkerPrivatePipeServer() noexcept;
    WorkerPrivatePipeServer(const WorkerPrivatePipeServer&) = delete;
    WorkerPrivatePipeServer& operator=(const WorkerPrivatePipeServer&) = delete;

    // Accepts the connection and requires the handshake to carry this session's
    // nonce and startup secret.  On success the returned channel is
    // authenticated and the reported binding is written to `worker_binding`.
    // On failure the connection is dropped and nothing is returned.
    [[nodiscard]] Result<std::unique_ptr<WorkerPipeChannel>> accept_and_handshake(
        const OperationContext& context, const WorkerBinding& expected_binding,
        std::uint64_t session_nonce, const StartupSecret& startup_secret);

private:
    explicit WorkerPrivatePipeServer(void* handle) noexcept;
    void* handle_{};
};

// ---------------------------------------------------------------------------
// Worker process launch
// ---------------------------------------------------------------------------
//
// The launcher lives here because it is the thing that ties the three pieces
// above together and nothing else: it creates the startup channel, creates the
// pipe server, starts the process, writes the spec and waits for the handshake.
// It deliberately knows nothing about profiles or durable state -- the caller
// builds the `WorkerLaunchSpec` -- so this file stays free of `src/management`.

struct LaunchedWorker final {
    // The caller owns this and must pass it to `close_launched_worker`.  It is
    // the handle to observe the process with, not a licence to command it.
    void* process_handle{};
    // The identity the channel authenticated: the child's own pid and process
    // creation time, which is what makes "the same worker" a question with an
    // answer even after Windows recycles the pid.
    WorkerBinding binding;
    std::unique_ptr<WorkerPipeChannel> channel;
};

// Starts a worker and does not return until the handshake has authenticated it.
//
// The only thing on the child's command line is the inherited handle value, so
// no secret can reach it.  Handle inheritance is restricted to that one handle:
// a plain `CreateProcessW(bInheritHandles = TRUE)` would hand the child every
// inheritable handle this process holds, which for a service is far more than a
// startup pipe.
//
// On any failure the child is terminated and waited for before returning.  A
// half-launched worker would hold the pipe name, and once started the bundle --
// and a process nobody has a handle to is a process nobody can stop.
// The budget is the context's own deadline: a separately passed timeout would be
// a second bound on the same wait, and the two could disagree.
//
// `worker_job` is the job object the new process is placed into, and it is
// **required**: a worker outside the job is invisible to the startup sweep, so a
// service that restarted could never find or clean up after it.  Passing null is
// a failure rather than a launch without a job, because the leak it would create
// is silent and only shows up as a device nobody can account for.
//
// The assignment is atomic with creation (`PROC_THREAD_ATTRIBUTE_JOB_LIST`), not
// a separate call afterwards.  Doing it after `CreateProcessW` returns would
// leave a window in which the worker is running outside every job, and the
// objects it creates in that window would not be the job's.
[[nodiscard]] Result<LaunchedWorker> launch_private_worker(
    const OperationContext& context, const std::wstring& worker_executable,
    const WorkerLaunchSpec& spec, void* worker_job);

// Closes the process handle.
//
// Only for a worker whose handle was *not* retained.  A caller that carries the
// handle into the live-worker registry (`WindowsWorkerProcessSource::
// retain_launched`) must not also call this: the registry needs the handle to
// wait for the process, and closing it here would leave a worker that can be
// observed and never waited for.  The two are distinguishable because retention
// takes the handle by reference and clears it.
void close_launched_worker(LaunchedWorker& worker) noexcept;

// The worker side: connects to the name it was told, then authenticates.
class WorkerPrivatePipeClient final {
public:
    [[nodiscard]] static Result<std::unique_ptr<WorkerPrivatePipeClient>>
    connect(const OperationContext& context, std::wstring_view pipe_name);
    ~WorkerPrivatePipeClient() noexcept;
    WorkerPrivatePipeClient(const WorkerPrivatePipeClient&) = delete;
    WorkerPrivatePipeClient& operator=(const WorkerPrivatePipeClient&) = delete;

    // Sends the handshake and requires a matching acknowledgement.  The caller
    // must not open a bundle, an AIM device or a volume before this succeeds.
    [[nodiscard]] Result<void> handshake(const OperationContext& context,
                                         const WorkerBinding& self,
                                         std::uint64_t session_nonce,
                                         const StartupSecret& startup_secret);

    [[nodiscard]] Result<void> write(const OperationContext& context,
                                     const WorkerMessage& message);
    [[nodiscard]] Result<WorkerMessage> read(const OperationContext& context);

private:
    explicit WorkerPrivatePipeClient(WorkerPipeChannel channel) noexcept;
    WorkerPipeChannel channel_;
};

} // namespace myrd::m5
