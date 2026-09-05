#pragma once

#include "src/common/result.h"
#include "src/control/command.h"
#include "src/product/uuid.h"

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace myrd::cli {

struct GlobalOptions {
    bool json{};
    std::optional<std::chrono::seconds> timeout;
    std::optional<product::Uuid> request_id;
    bool no_wait{};
    bool dry_run{};
};

struct ParsedCommand {
    GlobalOptions options;
    control::Command command;
};

Result<ParsedCommand> parse_cli(std::span<const std::wstring_view> args);

} // namespace myrd::cli
