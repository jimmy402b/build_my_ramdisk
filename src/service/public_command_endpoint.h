#pragma once

// **The join between the public pipe and the service core.**
//
// Every piece of the command surface existed and none of them were connected:
// `PublicPipeServer` accepted connections and called a handler that nothing ever
// supplied; `ServiceCore::dispatch` took a `ServiceCommandRequest` that only tests
// ever built; `encode_command_payload` and `decode_command_payload` spoke a protocol
// no production client used.  This class is the one function that turns "a frame
// arrived" into "the core was asked, and here is the answer as a frame".
//
// It is deliberately thin, because everything it does is a decision that must be
// readable in one place:
//
//   * the caller's identity comes from the pipe, never from the frame -- the service
//     derives it from the connection, so a client cannot claim one;
//   * the control bits come from the frame's two documented flags and nothing else;
//   * a mutating command takes the execution gate exclusively for its *admission*
//     step, and a query takes nothing at all (see `handle`);
//   * a failure is answered, not dropped: a client that gets no reply cannot tell a
//     refused command from a broken service.

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/control/ipc_frame.h"
#include "src/management/service_core.h"

#include <mutex>

namespace myrd::service {

class PublicCommandEndpoint final {
public:
    // The gate is the scheduler's, passed in rather than created: a scheduler
    // serialized on one gate and a pipe serialized on another is a service with no
    // serialization at all.  It is the same object `ServiceExecutionScheduler` holds.
    PublicCommandEndpoint(management::ServiceCore& core,
                          std::timed_mutex& execution_gate) noexcept;

    // Takes exactly one request frame and produces exactly one response frame.
    //
    // **Why a query takes no gate.**  The scheduler holds the gate exclusively for
    // the whole of an operation -- a create can hold it for minutes.  A query that
    // took the gate, shared or exclusive, would therefore wait for that create, and
    // "`disk list` returns within a second while a create is running" would be a
    // property the code cannot have.  Queries are safe without it because the state
    // they read is only ever read under the stores' own locks: the operation store's
    // pure queries take a shared lock (`structural.m5.read_write_separation` holds
    // that line), the registration store serializes per key, and `ServiceCore`'s own
    // mutable state is `pending_` behind its own mutex.  Taking the gate would add
    // serialization without adding exclusion.
    //
    // **Why a mutation takes it at all.**  Its admission step writes the durable
    // record the scheduler then claims.  If a claim could interleave with admission,
    // the scheduler could run an operation whose record was still being written.
    // `dispatch` returns as soon as the operation is admitted, so this hold is the
    // admission and not the operation.
    Result<ipc::Frame> handle(const OperationContext& context,
                              const ipc::CallerIdentity& caller,
                              const ipc::Frame& request);

private:
    management::ServiceCore& core_;
    std::timed_mutex& execution_gate_;
};

} // namespace myrd::service
