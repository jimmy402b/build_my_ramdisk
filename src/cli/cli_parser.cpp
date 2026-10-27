#include "src/cli/cli_parser.h"

#include "src/product/quantity_parser.h"
#include "src/product/uuid.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <utility>

namespace myrd::cli {

namespace {

const char* kUsage =
    "myrd-cli -- the MyRamDisk command line client\n"
    "\n"
    "Usage:\n"
    "  myrd-cli disk list\n"
    "  myrd-cli disk show <disk>\n"
    "  myrd-cli disk create --name <name> --bundle <dir> --size <size> --mount <target>\n"
    "                       [--owner <sid>] [--no-start] [--memory-mode automatic|fixed]\n"
    "                       [--memory-hard-limit <size>] [--checkpoint-interval <interval>]\n"
    "  myrd-cli disk import --name <name> --bundle <dir> --mount <target> [--owner <sid>]\n"
    "  myrd-cli disk start <disk>\n"
    "  myrd-cli disk stop <disk> [--force] [--accept-volatile-data-loss]\n"
    "  myrd-cli disk checkpoint <disk>\n"
    "  myrd-cli disk compact <disk>\n"
    "  myrd-cli disk config set auto_start <true|false> <disk>\n"
    "  myrd-cli disk forget <profile-id> --confirmation <uuid>\n"
    "  myrd-cli disk repair-profile <profile-id> [--from-previous]\n"
    "  myrd-cli disk rebind <profile-id> --bundle <dir> --expected-image <uuid>\n"
    "  myrd-cli operation show <operation-id>\n"
    "  myrd-cli operation wait <operation-id>\n"
    "  myrd-cli operation cancel <operation-id>\n"
    "\n"
    "Options:\n"
    "  --wait                 wait for the admitted operation to finish\n"
    "  --no-wait              return as soon as the operation is admitted\n"
    "  --dry-run              report what would be done, do nothing\n"
    "  --timeout <seconds>    bound on one exchange and on --wait (default 300)\n"
    "  --pipe <name>          connect to a named control pipe (diagnostics/tests)\n"
    "  --help                 print this text\n"
    "  --version              print the client version\n"
    "\n"
    "Exit codes: 0 success, 1 product failure, 2 usage/schema, 3 service unavailable,\n"
    "            4 permission, 5 timeout or indeterminate result.\n";

std::wstring lower(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](const wchar_t ch) {
                       return static_cast<wchar_t>(std::towlower(ch));
                   });
    return result;
}

std::string narrow(const std::wstring& value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        result.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return result;
}

// A parsed command line, before it becomes one of the typed commands.
//
// Options are pulled out of the argument vector first and the remaining words are
// matched positionally, because that is the shape both a user and a script expect:
// `disk stop D: --force` and `disk stop --force D:` are the same command.
struct Parsed final {
    std::vector<std::wstring> words;
    std::map<std::wstring, std::wstring> values;
    std::vector<std::wstring> flags;
};

bool is_option(const std::wstring& word) {
    return word.size() > 2 && word[0] == L'-' && word[1] == L'-';
}

std::string describe(const std::wstring& word) { return narrow(word); }

} // namespace

const char* to_string(const CliExitCode code) noexcept {
    switch (code) {
    case CliExitCode::success: return "success";
    case CliExitCode::failure: return "failure";
    case CliExitCode::usage: return "usage";
    case CliExitCode::unavailable: return "unavailable";
    case CliExitCode::permission: return "permission";
    case CliExitCode::indeterminate: return "indeterminate";
    }
    return "unknown";
}

const char* usage_text() noexcept { return kUsage; }

CliInvocation parse_command_line(const std::span<const std::wstring> arguments) {
    CliInvocation invocation{};
    invocation.kind = CliInvocationKind::request;

    // The two flags that carry a value.  Everything else is a switch, so a mistyped
    // switch is refused instead of being silently dropped -- a client that ignored an
    // option would be a client that did something other than what was asked.
    const std::vector<std::wstring> value_options{
        L"--name", L"--bundle", L"--size", L"--mount", L"--owner",
        L"--memory-mode", L"--memory-hard-limit", L"--checkpoint-interval",
        L"--confirmation", L"--expected-image", L"--timeout", L"--pipe"};
    const std::vector<std::wstring> switch_options{
        L"--no-start", L"--force", L"--accept-volatile-data-loss", L"--wait",
        L"--no-wait", L"--dry-run", L"--from-previous", L"--help", L"-h",
        L"--version"};

    Parsed parsed;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring word(arguments[index]);
        if (!is_option(word) && word != L"-h") {
            parsed.words.push_back(word);
            continue;
        }
        const auto slash = word.find(L'=');
        const std::wstring name =
            slash == std::wstring::npos ? word : word.substr(0, slash);
        const auto in_values = std::find(value_options.begin(), value_options.end(),
                                         name) != value_options.end();
        const auto in_switches = std::find(switch_options.begin(),
                                           switch_options.end(), name)
            != switch_options.end();
        if (!in_values && !in_switches) {
            invocation.kind = CliInvocationKind::request;
            invocation.usage_error =
                "unknown option '" + describe(word) + "'";
            return invocation;
        }
        if (in_switches) {
            if (slash != std::wstring::npos) {
                invocation.usage_error =
                    "option '" + describe(name) + "' takes no value";
                return invocation;
            }
            parsed.flags.push_back(name);
            continue;
        }
        std::wstring value;
        if (slash != std::wstring::npos) {
            value = word.substr(slash + 1);
        } else if (index + 1 < arguments.size()) {
            value = arguments[++index];
        } else {
            invocation.usage_error =
                "option '" + describe(name) + "' needs a value";
            return invocation;
        }
        if (value.empty()) {
            invocation.usage_error =
                "option '" + describe(name) + "' needs a value";
            return invocation;
        }
        if (parsed.values.contains(name)) {
            invocation.usage_error =
                "option '" + describe(name) + "' was given twice";
            return invocation;
        }
        parsed.values.emplace(name, value);
    }

    const auto has_flag = [&parsed](const wchar_t* name) {
        return std::find(parsed.flags.begin(), parsed.flags.end(),
                         std::wstring(name)) != parsed.flags.end();
    };
    const auto value_of = [&parsed](const wchar_t* name) -> std::optional<std::wstring> {
        const auto found = parsed.values.find(name);
        if (found == parsed.values.end()) return std::nullopt;
        return found->second;
    };
    const auto require_value = [&](const wchar_t* name,
                                   std::wstring& target) -> bool {
        auto value = value_of(name);
        if (!value.has_value()) {
            invocation.usage_error = std::string("option '") + narrow(name)
                + "' is required";
            return false;
        }
        target = std::move(value.value());
        return true;
    };

    if (has_flag(L"--version")) {
        invocation.kind = CliInvocationKind::version;
        return invocation;
    }
    if (has_flag(L"--help") || has_flag(L"-h") || parsed.words.empty()) {
        invocation.kind = CliInvocationKind::help;
        return invocation;
    }

    auto& request = invocation.request;
    request.no_wait = has_flag(L"--no-wait");
    request.dry_run = has_flag(L"--dry-run");
    request.wait = has_flag(L"--wait");
    if (request.no_wait && request.wait) {
        invocation.usage_error = "--wait and --no-wait cannot both be given";
        return invocation;
    }
    if (const auto timeout = value_of(L"--timeout"); timeout.has_value()) {
        std::uint64_t seconds = 0;
        for (const wchar_t ch : timeout.value()) {
            if (ch < L'0' || ch > L'9') {
                invocation.usage_error = "--timeout must be a whole number of seconds";
                return invocation;
            }
            seconds = seconds * 10 + static_cast<std::uint64_t>(ch - L'0');
            if (seconds > 86'400) {
                invocation.usage_error = "--timeout must not exceed 86400 seconds";
                return invocation;
            }
        }
        if (seconds == 0) {
            invocation.usage_error = "--timeout must be greater than zero";
            return invocation;
        }
        request.timeout_seconds = static_cast<std::uint32_t>(seconds);
    }
    if (auto pipe = value_of(L"--pipe"); pipe.has_value()) {
        request.pipe_name = std::move(pipe.value());
    }

    // `<uuid>` arrives as text and becomes the type the protocol uses.  The
    // canonical form is the only accepted one: a client that guessed at an
    // identifier's spelling would be inventing an id the service never issued.
    const auto parse_uuid = [&](const std::wstring& text,
                                std::optional<product::Uuid>& target) -> bool {
        auto parsed_uuid = product::Uuid::parse(narrow(text));
        if (!parsed_uuid.ok()) {
            invocation.usage_error =
                "'" + narrow(text) + "' is not a canonical identifier";
            return false;
        }
        target = parsed_uuid.value();
        return true;
    };

    const auto& words = parsed.words;
    const std::wstring& noun = words[0];
    if (noun == L"disk") {
        if (words.size() < 2) {
            invocation.usage_error = "disk needs a verb: list, show, create, ...";
            return invocation;
        }
        const std::wstring& verb = words[1];
        if (verb == L"list") {
            if (words.size() != 2) {
                invocation.usage_error = "disk list takes no arguments";
                return invocation;
            }
            request.command = control::DiskListCommand{};
            return invocation;
        }
        if (verb == L"create") {
            control::DiskCreateCommand create{};
            if (!require_value(L"--name", create.name)
                || !require_value(L"--bundle", create.bundle)
                || !require_value(L"--mount", create.mount)) {
                return invocation;
            }
            // The bundle is a directory on this machine, and the service compares it
            // as an absolute path, so a relative one is resolved here rather than
            // being refused a round trip later.  The mount target is *not* treated
            // this way: a drive letter is a legitimate target and would be
            // misinterpreted as a relative directory.
            auto normalized = product::normalize_local_absolute_path(create.bundle);
            if (!normalized.ok()) {
                invocation.usage_error = "--bundle is not a usable local path";
                return invocation;
            }
            create.bundle = normalized.move_value();
            std::wstring size;
            if (!require_value(L"--size", size)) return invocation;
            auto parsed_size = product::parse_binary_size(narrow(size));
            if (!parsed_size.ok()) {
                invocation.usage_error = "--size is not a size (for example 64MiB)";
                return invocation;
            }
            create.size_bytes = parsed_size.value();
            if (auto owner = value_of(L"--owner"); owner.has_value()) {
                create.owner_sid = std::move(owner.value());
            }

            create.no_start = has_flag(L"--no-start");
            if (auto mode = value_of(L"--memory-mode"); mode.has_value()) {
                const auto lowered = lower(mode.value());
                if (lowered == L"automatic") {
                    create.memory_mode = control::MemoryMode::automatic;
                } else if (lowered == L"fixed") {
                    create.memory_mode = control::MemoryMode::fixed;
                } else {
                    invocation.usage_error =
                        "--memory-mode must be automatic or fixed";
                    return invocation;
                }
            }
            if (auto limit = value_of(L"--memory-hard-limit"); limit.has_value()) {
                auto parsed_limit = product::parse_binary_size(narrow(limit.value()));
                if (!parsed_limit.ok()) {
                    invocation.usage_error =
                        "--memory-hard-limit is not a size (for example 2GiB)";
                    return invocation;
                }
                create.memory_hard_limit_bytes = parsed_limit.value();
            }
            if (auto interval = value_of(L"--checkpoint-interval");
                interval.has_value()) {
                auto parsed_interval =
                    product::parse_checkpoint_interval(narrow(interval.value()));
                if (!parsed_interval.ok()) {
                    invocation.usage_error =
                        "--checkpoint-interval is not an interval (60s, 5m, off)";
                    return invocation;
                }
                create.checkpoint_interval = parsed_interval.value();
            }
            if (words.size() != 2) {
                invocation.usage_error = "disk create takes no positional arguments";
                return invocation;
            }
            request.command = std::move(create);
            return invocation;
        }
        if (verb == L"import") {
            control::DiskImportCommand import{};
            if (!require_value(L"--name", import.name)
                || !require_value(L"--bundle", import.bundle)
                || !require_value(L"--mount", import.mount)) {
                return invocation;
            }
            auto normalized = product::normalize_local_absolute_path(import.bundle);
            if (!normalized.ok()) {
                invocation.usage_error = "--bundle is not a usable local path";
                return invocation;
            }
            import.bundle = normalized.move_value();
            if (auto owner = value_of(L"--owner"); owner.has_value()) {
                import.owner_sid = std::move(owner.value());
            }
            request.command = std::move(import);
            return invocation;
        }
        if (verb == L"config") {
            if (words.size() != 6 || words[2] != L"set"
                || words[3] != L"auto_start") {
                invocation.usage_error =
                    "usage: disk config set auto_start <true|false> <disk>";
                return invocation;
            }
            const auto value = lower(words[4]);
            if (value != L"true" && value != L"false") {
                invocation.usage_error = "auto_start must be true or false";
                return invocation;
            }
            request.command = control::DiskConfigSetCommand{
                words[5],
                {control::ConfigAssignment{L"auto_start", value}}};
            return invocation;
        }
        if (verb == L"forget") {
            if (words.size() != 3) {
                invocation.usage_error =
                    "usage: disk forget <profile-id> --confirmation <uuid>";
                return invocation;
            }
            std::optional<product::Uuid> profile_id;
            if (!parse_uuid(words[2], profile_id)) return invocation;
            std::wstring confirmation;
            if (!require_value(L"--confirmation", confirmation)) return invocation;
            std::optional<product::Uuid> confirmation_id;
            if (!parse_uuid(confirmation, confirmation_id)) return invocation;
            request.command = control::DiskForgetCommand{
                profile_id.value(), confirmation_id.value()};
            return invocation;
        }
        if (verb == L"repair-profile") {
            if (words.size() != 3) {
                invocation.usage_error = "usage: disk repair-profile <profile-id>";
                return invocation;
            }
            std::optional<product::Uuid> profile_id;
            if (!parse_uuid(words[2], profile_id)) return invocation;
            request.command = control::DiskRepairProfileCommand{
                profile_id.value(), has_flag(L"--from-previous")};
            return invocation;
        }
        if (verb == L"rebind") {
            if (words.size() != 3) {
                invocation.usage_error =
                    "usage: disk rebind <profile-id> --bundle <dir> --expected-image <uuid>";
                return invocation;
            }
            std::optional<product::Uuid> profile_id;
            if (!parse_uuid(words[2], profile_id)) return invocation;
            control::DiskRebindCommand rebind{
                profile_id.value(), L"", product::Uuid::parse(
                    "00000000-0000-0000-0000-000000000000").value()};
            if (!require_value(L"--bundle", rebind.bundle)) return invocation;
            auto normalized = product::normalize_local_absolute_path(rebind.bundle);
            if (!normalized.ok()) {
                invocation.usage_error = "--bundle is not a usable local path";
                return invocation;
            }
            rebind.bundle = normalized.move_value();
            std::wstring expected;
            if (!require_value(L"--expected-image", expected)) return invocation;
            std::optional<product::Uuid> expected_image;
            if (!parse_uuid(expected, expected_image)) return invocation;
            rebind.expected_image_id = expected_image.value();
            request.command = std::move(rebind);
            return invocation;
        }
        // The verbs that take exactly one disk target.
        if (words.size() != 3) {
            invocation.usage_error = "usage: disk " + narrow(verb) + " <disk>";
            return invocation;
        }
        if (verb == L"show") {
            request.command = control::DiskShowCommand{words[2]};
            return invocation;
        }
        if (verb == L"start") {
            request.command = control::DiskStartCommand{words[2]};
            return invocation;
        }
        if (verb == L"stop") {
            control::DiskStopCommand stop{};
            stop.target = words[2];
            // The two confirmations stay independent.  `--force` stops even when the
            // teardown would otherwise be refused; `--accept-volatile-data-loss`
            // permits discarding dirty writes.  They are different promises to the
            // user, and the service requires both before it will force a stop -- but
            // the client must not conflate them into one flag, because the point of
            // two flags is that a user can mean only one of them.
            stop.force = has_flag(L"--force");
            stop.accept_volatile_data_loss = has_flag(L"--accept-volatile-data-loss");
            request.command = std::move(stop);
            return invocation;
        }
        if (verb == L"checkpoint") {
            request.command = control::DiskCheckpointCommand{words[2]};
            return invocation;
        }
        if (verb == L"compact") {
            request.command = control::DiskCompactCommand{words[2]};
            return invocation;
        }
        invocation.usage_error = "unknown disk verb '" + narrow(verb) + "'";
        return invocation;
    }

    if (noun == L"operation") {
        if (words.size() != 3) {
            invocation.usage_error =
                "usage: operation show|wait|cancel <operation-id>";
            return invocation;
        }
        std::optional<product::Uuid> identifier;
        if (!parse_uuid(words[2], identifier)) return invocation;
        if (words[1] == L"show") {
            request.command = control::OperationShowCommand{identifier.value()};
            return invocation;
        }
        if (words[1] == L"wait") {
            request.command = control::OperationWaitCommand{identifier.value()};
            return invocation;
        }
        if (words[1] == L"cancel") {
            request.command = control::OperationCancelCommand{identifier.value()};
            return invocation;
        }
        invocation.usage_error = "unknown operation verb '" + narrow(words[1]) + "'";
        return invocation;
    }

    invocation.usage_error = "unknown command '" + narrow(noun) + "'";
    return invocation;
}

} // namespace myrd::cli