// The client half of the public protocol: the response document, the command line,
// the rendering, and the exit codes.
//
// T1-05's acceptance has two halves.  What a device is needed for -- create, start,
// read, checkpoint, stop, recover, remove through a real service on a real VM -- is
// measured there.  What a device is *not* needed for is the shape of every answer on
// the way: that the wire document survives a round trip, that it carries no bundle
// path, that a strict reader refuses what it does not understand, that a command line
// becomes exactly the typed command the service decodes, and that a response maps to
// the exit code a script will see.  Those are all measured here, on every build.
//
// The tests are written against the *contract* rather than against a service: a
// response is a value, so each one can be built, encoded, decoded and classified
// without a pipe.  That is why an IPC client can be verified before the service that
// answers it exists -- a half-verified client would be worse than none, because it
// would look like it worked.

#include "tests/support/test_framework.h"

#include "src/cli/cli_parser.h"
#include "src/cli/cli_renderer.h"
#include "src/control/ipc_frame.h"
#include "src/management/service_response_wire.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace myrd;

namespace {

product::Uuid uuid(const char* text) {
    return product::Uuid::parse(text).value();
}

product::ServiceDiskView profile(const char* id,
                                             const char* mount,
                                             const product::AggregateStatus
                                                 aggregate,
                                             const std::uint64_t resident = 0,
                                             const std::uint64_t budget = 0) {
    auto projection = product::ServiceDiskView{
        uuid(id),
        product::ServiceLifecycle::ready,
        product::ServiceRuntime::running,
        product::ServiceActivity::idle,
        product::ServicePersistence::clean,
        product::ServiceHealth::healthy,
        static_cast<product::ServiceAggregate>(aggregate),
        3,
        std::optional<std::string>(mount)};
    projection.resident_bytes = resident;
    projection.budget_bytes = budget;
    return projection;
}

management::ServiceResponseWire list_response() {
    management::ServiceResponseWire wire{
        uuid("11111111-1111-4111-8111-111111111111"),
        "disk.list",
        product::ServiceDisposition::completed,
        std::nullopt,
        std::nullopt,
        {profile("22222222-2222-4222-8222-222222222222", "X:",
                 management::AggregateStatus::running, 19'660'800ull,
                 1'073'741'824ull),
         profile("33333333-3333-4333-8333-333333333333", "Y:",
                 management::AggregateStatus::blocked_needs_attention)},
        {},
        std::nullopt,
        std::nullopt};
    return wire;
}

std::vector<std::wstring> words(const std::vector<const wchar_t*>& raw) {
    std::vector<std::wstring> result;
    result.reserve(raw.size());
    for (const auto* word : raw) result.emplace_back(word);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// The response document.
// ---------------------------------------------------------------------------

TEST_CASE(cli_wire_round_trips_a_list_response) {
    const auto wire = list_response();
    auto bytes = management::encode_service_response_wire(wire);
    REQUIRE(bytes.ok());
    auto decoded = management::decode_service_response(bytes.value(),
                                                       ipc::kProtocolMinor);
    if (!decoded.ok()) {
        std::fwprintf(stderr, L"decode failed: %ls\n",
                      decoded.error().message.c_str());
    }
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().request_id == wire.request_id);
    REQUIRE_EQ(decoded.value().command, std::string("disk.list"));
    REQUIRE(decoded.value().disposition
            == product::ServiceDisposition::completed);
    REQUIRE_EQ(decoded.value().profiles.size(), 2ULL);
    REQUIRE(decoded.value().profiles.front().profile_id
            == wire.profiles.front().profile_id);
    REQUIRE(decoded.value().profiles.front().mount_target.has_value());
    REQUIRE_EQ(decoded.value().profiles.front().mount_target.value(),
               std::string("X:"));
    REQUIRE(decoded.value().profiles.front().aggregate
            == product::ServiceAggregate::running);
    REQUIRE_EQ(decoded.value().profiles.front().confirmed_generation, 3ULL);
    // M5 搂7: the memory gauges are part of the projection a client renders, so they
    // have to survive the codec.  The figures are the ones a measured run reported.
    REQUIRE_EQ(decoded.value().profiles.front().resident_bytes, 19'660'800ULL);
    REQUIRE_EQ(decoded.value().profiles.front().budget_bytes, 1'073'741'824ULL);
    // The second profile's verdict is what a client renders; a projection that lost it
    // would show a blocked disk as a healthy one.
    REQUIRE(decoded.value().profiles.back().aggregate
            == product::ServiceAggregate::blocked_needs_attention);
}

TEST_CASE(cli_wire_round_trips_an_error_response_with_a_wide_message) {
    // The service's messages are wide strings, so the codec has to convert them.  A
    // message that is not ASCII is the case where a lossy conversion would show up,
    // and it is the case that matters: a user with a non-English system locale sees
    // exactly this.
    management::ServiceResponseWire wire{
        uuid("11111111-1111-4111-8111-111111111111"),
        "disk.create",
        product::ServiceDisposition::completed,
        std::nullopt,
        std::nullopt,
        {},
        {},
        Error{ErrorDomain::storage, 0x5604, L"service_core.create",
              L"\u78c1\u76d8\u4e0d\u53ef\u7528", 5},
        std::nullopt};
    auto bytes = management::encode_service_response_wire(wire);
    REQUIRE(bytes.ok());
    auto decoded = management::decode_service_response(bytes.value(),
                                                       ipc::kProtocolMinor);
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().error.has_value());
    REQUIRE_EQ(decoded.value().error->code, 0x5604U);
    REQUIRE_EQ(decoded.value().error->native_code, 5U);
    REQUIRE(decoded.value().error->message == L"\u78c1\u76d8\u4e0d\u53ef\u7528");
    REQUIRE(decoded.value().error->operation == L"service_core.create");
}

TEST_CASE(cli_wire_rejects_an_unknown_tag) {
    auto bytes = management::encode_service_response_wire(list_response());
    REQUIRE(bytes.ok());
    // The second field's tag, at the end of the header plus the first field.
    const std::size_t second_tag = 6 + (2 + 4) + 16;
    REQUIRE(second_tag + 2 <= bytes.value().size());
    bytes.value()[second_tag] = std::byte{0x7f};
    bytes.value()[second_tag + 1] = std::byte{0x00};
    auto decoded = management::decode_service_response(bytes.value(),
                                                      ipc::kProtocolMinor);
    REQUIRE(!decoded.ok());
}

TEST_CASE(cli_wire_rejects_a_repeated_field) {
    auto bytes = management::encode_service_response_wire(list_response());
    REQUIRE(bytes.ok());
    const std::size_t second_tag = 6 + (2 + 4) + 16;
    // The same tag as the first field: a document that carries it twice is one this
    // client cannot interpret, so it is refused rather than resolved by taking one.
    bytes.value()[second_tag] = std::byte{0x01};
    bytes.value()[second_tag + 1] = std::byte{0x00};
    auto decoded = management::decode_service_response(bytes.value(),
                                                      ipc::kProtocolMinor);
    REQUIRE(!decoded.ok());
}

TEST_CASE(cli_wire_rejects_a_value_outside_its_range) {
    auto bytes = management::encode_service_response_wire(list_response());
    REQUIRE(bytes.ok());
    // The disposition is the third field: header + request id + command.
    std::size_t offset = 6;
    for (int field = 0; field < 2; ++field) {
        const auto length = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(bytes.value()[offset + 2])
            | (static_cast<std::uint32_t>(
                   static_cast<std::uint8_t>(bytes.value()[offset + 3]))
               << 8U)
            | (static_cast<std::uint32_t>(
                   static_cast<std::uint8_t>(bytes.value()[offset + 4]))
               << 16U)
            | (static_cast<std::uint32_t>(
                   static_cast<std::uint8_t>(bytes.value()[offset + 5]))
               << 24U));
        offset += 6 + length;
    }
    REQUIRE(offset + 6 < bytes.value().size());
    bytes.value()[offset + 6] = std::byte{9};
    auto decoded = management::decode_service_response(bytes.value(),
                                                      ipc::kProtocolMinor);
    REQUIRE(!decoded.ok());
}

TEST_CASE(cli_wire_rejects_a_truncated_document_and_trailing_bytes) {
    auto bytes = management::encode_service_response_wire(list_response());
    REQUIRE(bytes.ok());
    const std::vector<std::byte> truncated(bytes.value().begin(),
                                           bytes.value().end() - 1);
    REQUIRE(!management::decode_service_response(truncated, ipc::kProtocolMinor)
                 .ok());

    auto longer = bytes.value();
    longer.push_back(std::byte{0x00});
    REQUIRE(!management::decode_service_response(longer, ipc::kProtocolMinor).ok());
}

TEST_CASE(cli_wire_rejects_a_document_newer_than_the_reader) {
    auto bytes = management::encode_service_response_wire(list_response());
    REQUIRE(bytes.ok());
    // Minor 1, while the reader knows minor 0.  Refused rather than half-read: a field
    // this client does not know could be the one that changes the meaning.
    bytes.value()[3] = std::byte{0x01};
    REQUIRE(!management::decode_service_response(bytes.value(),
                                                 ipc::kProtocolMinor)
                 .ok());
    // A major it does not know is a different document, not a newer one.
    auto major = management::encode_service_response_wire(list_response());
    REQUIRE(major.ok());
    major.value()[0] = std::byte{0x02};
    REQUIRE(!management::decode_service_response(major.value(),
                                                 ipc::kProtocolMinor)
                 .ok());
}

// ---------------------------------------------------------------------------
// The command line.
// ---------------------------------------------------------------------------

TEST_CASE(cli_parses_disk_list_and_carries_no_wait_flags) {
    const auto invocation = cli::parse_command_line(words({L"disk", L"list"}));
    REQUIRE(invocation.usage_error.empty());
    REQUIRE(invocation.kind == cli::CliInvocationKind::request);
    REQUIRE(std::holds_alternative<control::DiskListCommand>(invocation.request.command));
    REQUIRE(!invocation.request.wait);
    REQUIRE(!invocation.request.no_wait);
}

TEST_CASE(cli_parses_disk_create_into_the_typed_command) {
    const auto invocation = cli::parse_command_line(words(
        {L"disk", L"create", L"--name", L"work", L"--bundle", L"C:\\bundles\\work",
         L"--size", L"64MiB", L"--mount", L"X:", L"--no-start", L"--wait",
         L"--checkpoint-interval", L"5m"}));
    REQUIRE(invocation.usage_error.empty());
    REQUIRE(std::holds_alternative<control::DiskCreateCommand>(
        invocation.request.command));
    const auto& create = std::get<control::DiskCreateCommand>(
        invocation.request.command);
    REQUIRE_EQ(create.name, std::wstring(L"work"));
    REQUIRE_EQ(create.bundle, std::wstring(L"C:\\bundles\\work"));
    REQUIRE_EQ(create.size_bytes, 64ULL * 1024ULL * 1024ULL);
    REQUIRE_EQ(create.mount, std::wstring(L"X:"));
    REQUIRE(create.no_start);
    REQUIRE(create.checkpoint_interval.has_value());
    REQUIRE_EQ(create.checkpoint_interval->duration.count(), 300LL);
    REQUIRE(invocation.request.wait);
}

TEST_CASE(cli_refuses_a_command_line_it_cannot_express) {
    // An unknown switch is refused rather than dropped: a client that ignored an option
    // would do something other than what was asked.
    REQUIRE(!cli::parse_command_line(words({L"disk", L"list", L"--wat"}))
                 .usage_error.empty());
    // A missing value is a usage error, not an empty value.
    REQUIRE(!cli::parse_command_line(words({L"disk", L"show"})).usage_error.empty());
    // A size without a unit is refused by the product's own parser, not by a second
    // one written here.
    REQUIRE(!cli::parse_command_line(words(
                 {L"disk", L"create", L"--name", L"work", L"--bundle", L"C:\\b",
                  L"--size", L"64", L"--mount", L"X:"}))
                 .usage_error.empty());
    // A malformed identifier is refused before it reaches the wire.
    REQUIRE(!cli::parse_command_line(words({L"operation", L"show", L"not-a-uuid"}))
                 .usage_error.empty());
    REQUIRE(!cli::parse_command_line(words({L"disk", L"nonsense"}))
                 .usage_error.empty());
}

TEST_CASE(cli_refuses_wait_with_no_wait_and_keeps_the_two_stop_confirmations_apart) {
    REQUIRE(!cli::parse_command_line(
                 words({L"disk", L"start", L"X:", L"--wait", L"--no-wait"}))
                 .usage_error.empty());

    const auto forced = cli::parse_command_line(
        words({L"disk", L"stop", L"X:", L"--force"}));
    REQUIRE(forced.usage_error.empty());
    const auto& stop = std::get<control::DiskStopCommand>(forced.request.command);
    // `--force` and `--accept-volatile-data-loss` are separate promises to the user;
    // the client must not make one imply the other.
    REQUIRE(stop.force);
    REQUIRE(!stop.accept_volatile_data_loss);
    REQUIRE_EQ(stop.target, std::wstring(L"X:"));
}

TEST_CASE(cli_help_and_version_are_not_requests) {
    REQUIRE(cli::parse_command_line({}).kind == cli::CliInvocationKind::help);
    const auto help = cli::parse_command_line(words({L"--help"}));
    REQUIRE(help.kind == cli::CliInvocationKind::help);
    REQUIRE(help.usage_error.empty());
    const auto version = cli::parse_command_line(words({L"--version"}));
    REQUIRE(version.kind == cli::CliInvocationKind::version);
}

// ---------------------------------------------------------------------------
// Rendering and exit codes.
// ---------------------------------------------------------------------------

TEST_CASE(cli_renders_the_verdict_not_the_durable_claim) {
    // The projection carries `runtime` (the durable record's opinion) and `aggregate`
    // (the verdict).  For a record that says running while the worker is gone they
    // disagree, and the line has to show the verdict.
    const auto wire = list_response();
    const auto text = cli::render_response(wire);
    // The product's own name for the verdict, not a second spelling of it: the CLI is
    // rendering the value `aggregate_status` decided.
    REQUIRE(text.find(std::string(management::to_string(
                management::AggregateStatus::blocked_needs_attention)))
            != std::string::npos);
    REQUIRE(text.find("X:") != std::string::npos);
    REQUIRE(text.find("Y:") != std::string::npos);
    REQUIRE(text.find("2 disk(s)") != std::string::npos);
}

TEST_CASE(cli_renders_resident_memory_against_its_budget) {
    // The one line a user checks when they ask "how much RAM is this disk using?".
    // Rendered from the projection, so this case pins the *display* half; the value's
    // source is pinned in the observation and projection cases.
    const auto wire = list_response();
    const auto text = cli::render_response(wire);
    REQUIRE(text.find("mem=19660800/1073741824") != std::string::npos);
}

TEST_CASE(cli_maps_responses_to_the_documented_exit_codes) {
    const auto with_error = [](const std::uint32_t code) {
        management::ServiceResponseWire wire{
            uuid("11111111-1111-4111-8111-111111111111"),
            "disk.start",
            product::ServiceDisposition::completed,
            std::nullopt,
            std::nullopt,
            {},
            {},
            Error{ErrorDomain::internal, code, L"op", L"message", 0},
            std::nullopt};
        return wire;
    };

    // 0: a completed command.
    REQUIRE(cli::exit_code_for(list_response()) == cli::CliExitCode::success);
    // 1: the product refused or failed the command for its own reasons.
    REQUIRE(cli::exit_code_for(with_error(0x5604U)) == cli::CliExitCode::failure);
    REQUIRE(cli::exit_code_for(with_error(0x5605U)) == cli::CliExitCode::failure);
    // 2: the command's shape was wrong.
    REQUIRE(cli::exit_code_for(with_error(0x5601U)) == cli::CliExitCode::usage);
    REQUIRE(cli::exit_code_for(with_error(0x5602U)) == cli::CliExitCode::usage);
    // 4: authorisation.
    REQUIRE(cli::exit_code_for(with_error(0x5603U)) == cli::CliExitCode::permission);
    // 5: the result is not known.
    REQUIRE(cli::exit_code_for(with_error(0x100BU))
            == cli::CliExitCode::indeterminate);

    // An operation that ended badly is a failure even though the request succeeded.
    auto failed = list_response();
    failed.operation = product::ServiceOperationView{
        uuid("44444444-4444-4444-8444-444444444444"),
        uuid("22222222-2222-4222-8222-222222222222"),
        product::ServiceOperationState::failed,
        "terminal",
        1,
        2,
        false,
        false};
    REQUIRE(cli::exit_code_for(failed) == cli::CliExitCode::failure);

    // Still deciding whether a commit landed: reporting success would claim something
    // nobody knows yet.
    auto reconciling = failed;
    reconciling.operation->state = product::ServiceOperationState::reconciling;
    REQUIRE(cli::exit_code_for(reconciling) == cli::CliExitCode::indeterminate);

    // A wait that timed out did not fail; it just did not finish.
    auto waiting = list_response();
    waiting.disposition = product::ServiceDisposition::wait_timeout;
    REQUIRE(cli::exit_code_for(waiting) == cli::CliExitCode::indeterminate);

    // A queued operation is a success: the command was admitted and is running.
    auto queued = list_response();
    queued.disposition = product::ServiceDisposition::operation_queued;
    queued.operation = failed.operation;
    queued.operation->state = product::ServiceOperationState::running;
    REQUIRE(cli::exit_code_for(queued) == cli::CliExitCode::success);
}

TEST_CASE(cli_renders_an_error_with_its_step_and_code) {
    management::ServiceResponseWire wire{
        uuid("11111111-1111-4111-8111-111111111111"),
        "disk.start",
        product::ServiceDisposition::completed,
        std::nullopt,
        std::nullopt,
        {},
        {},
        Error{ErrorDomain::internal, 0x5603U, L"service_core.authorize_start",
              L"The caller is not authorized", 0},
        std::nullopt};
    const auto text = cli::render_response(wire);
    REQUIRE(text.find("error:") != std::string::npos);
    REQUIRE(text.find("service_core.authorize_start") != std::string::npos);
    REQUIRE(text.find("0x5603") != std::string::npos);
}
TEST_CASE(cli_wire_carries_the_volume_serial_and_renders_it) {
    // The serial is how a user matches a disk against `vol`; it travels through the same profile
    // payload as the memory gauges, and the guard that refuses a too-short payload had to grow by
    // eight bytes with it -- which is what the two failed attempts were about.
    auto wire = list_response();
    wire.profiles.front().volume_serial = 0xA1B2C3D4ULL;
    auto bytes = management::encode_service_response_wire(wire);
    REQUIRE(bytes.ok());
    auto decoded = management::decode_service_response(bytes.value(), ipc::kProtocolMinor);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().profiles.front().volume_serial, 0xA1B2C3D4ULL);
    REQUIRE_EQ(decoded.value().profiles.back().volume_serial, 0ULL);

    const auto text = cli::render_response(decoded.value());
    REQUIRE(text.find("serial=A1B2-C3D4") != std::string::npos);
}
