#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace myrd {
struct RunOptions final {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::chrono::seconds checkpoint_interval{600};
    std::uint64_t checkpoint_dirty_bytes{256ull * 1024 * 1024};
    std::uint64_t ram_limit_bytes{2ull * 1024 * 1024 * 1024};
};
struct CompactOptions final { std::filesystem::path bundle; };
enum class CliAction : std::uint8_t { Run, Compact, Help, Error };
struct CliParseResult final {
    CliAction action{CliAction::Error};
    std::optional<RunOptions> run;
    std::optional<CompactOptions> compact;
    std::wstring message;
};
CliParseResult parse_cli(std::span<const std::wstring_view> args);
}
