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
#include <string>
#include <string_view>
#include <vector>

namespace myrd::m5 {

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
    std::uint64_t logical_size_bytes{};
    std::wstring mount_target;
    std::uint64_t session_generation{};
    std::wstring private_pipe_name;
    std::uint64_t session_nonce{};
    StartupSecret startup_secret{};

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
