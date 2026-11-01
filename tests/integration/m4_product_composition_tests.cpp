#include "tests/support/test_framework.h"
#include "test_paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifndef MYRD_PRODUCT_EXE_PATH
#error "MYRD_PRODUCT_EXE_PATH must be provided by CMake"
#endif

namespace {

struct ProcessResult final {
    DWORD exit_code{};
    std::wstring stdout_text;
    std::wstring stderr_text;
};

std::wstring quote(const std::wstring_view value) {
    return L"\"" + std::wstring(value) + L"\"";
}

std::wstring read_pipe(HANDLE pipe) {
    std::string bytes;
    char buffer[4096];
    DWORD read{};
    while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read != 0)
        bytes.append(buffer, buffer + read);
    CloseHandle(pipe);
    // The exercised protocol output is ASCII/UTF-8.  Byte-wise widening keeps
    // assertions deterministic without locale state.
    return std::wstring(bytes.begin(), bytes.end());
}

ProcessResult run_product(const std::wstring_view arguments,
                          const std::wstring_view preflight = {},
                          const std::wstring_view entry = {}) {
    const auto set_env = [](const wchar_t* name, const std::wstring_view value) {
        if (value.empty()) SetEnvironmentVariableW(name, nullptr);
        else SetEnvironmentVariableW(name, std::wstring(value).c_str());
    };
    set_env(L"MYRD_M4_TEST_PREFLIGHT", preflight);
    set_env(L"MYRD_M4_TEST_ENTRY", entry);

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr;
    HANDLE err_read = nullptr, err_write = nullptr;
    REQUIRE(CreatePipe(&out_read, &out_write, &security, 0) != FALSE);
    REQUIRE(CreatePipe(&err_read, &err_write, &security, 0) != FALSE);
    REQUIRE(SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0) != FALSE);
    REQUIRE(SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0) != FALSE);

    std::wstring command = quote(MYRD_TEST_PATH_W(MYRD_PRODUCT_EXE_PATH));
    command.push_back(L' ');
    command.append(arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != FALSE);
    CloseHandle(out_write);
    CloseHandle(err_write);
    REQUIRE(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
    DWORD exit_code = STILL_ACTIVE;
    REQUIRE(GetExitCodeProcess(process.hProcess, &exit_code) != FALSE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    ProcessResult result{exit_code, read_pipe(out_read), read_pipe(err_read)};
    set_env(L"MYRD_M4_TEST_PREFLIGHT", {});
    set_env(L"MYRD_M4_TEST_ENTRY", {});
    return result;
}

std::filesystem::path unique_path(const wchar_t* stem) {
    return std::filesystem::temp_directory_path() /
        (std::wstring(stem) + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
}

void require_file_bytes(const std::filesystem::path& path,
                        const std::string& expected) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    std::string actual((std::istreambuf_iterator<char>(file)), {});
    REQUIRE_EQ(actual, expected);
}

TEST_CASE(m4_product_composition_help_captures_stdout_and_no_stderr) {
    const auto result = run_product(L"--help");
    REQUIRE_EQ(result.exit_code, 0UL);
    REQUIRE(result.stdout_text.find(L"Usage:") != std::wstring::npos);
    REQUIRE(result.stdout_text.find(L"myrd run") != std::wstring::npos);
    REQUIRE(result.stderr_text.empty());
}

TEST_CASE(m4_product_composition_invalid_command_has_usage_exit_and_stderr) {
    const auto result = run_product(L"invalid-command");
    REQUIRE_EQ(result.exit_code, 2UL);
    REQUIRE(result.stdout_text.empty());
    REQUIRE(result.stderr_text.find(L"Error: unknown command") !=
            std::wstring::npos);
    REQUIRE(result.stderr_text.find(L"Usage:") != std::wstring::npos);
}

TEST_CASE(m4_product_composition_preflight_precedes_bundle_access) {
    const auto bundle = unique_path(L"myrd-m4-preflight-");
    const auto result = run_product(
        L"run --bundle " + quote(bundle.wstring()) +
            L" --size 64M --drive-letter R:",
        L"unavailable");
    REQUIRE_EQ(result.exit_code, 1UL);
    REQUIRE(result.stderr_text.find(L"driver_unavailable") !=
            std::wstring::npos);
    REQUIRE(!std::filesystem::exists(bundle));
}

TEST_CASE(m4_product_composition_driver_unavailable_is_zero_write) {
    const auto bundle = unique_path(L"myrd-m4-zero-write-");
    std::filesystem::create_directories(bundle);
    const auto sentinel = bundle / L"sentinel.bin";
    const std::string bytes("bundle must remain byte-identical");
    std::ofstream(sentinel, std::ios::binary).write(bytes.data(),
                                                     static_cast<std::streamsize>(bytes.size()));
    const auto result = run_product(
        L"run --bundle " + quote(bundle.wstring()) +
            L" --size 64M --drive-letter R:",
        L"unavailable");
    REQUIRE_EQ(result.exit_code, 1UL);
    REQUIRE(result.stderr_text.find(L"driver_unavailable") !=
            std::wstring::npos);
    require_file_bytes(sentinel, bytes);
    REQUIRE(std::filesystem::exists(bundle));
    std::filesystem::remove_all(bundle);
}

TEST_CASE(m4_product_composition_selects_single_create_entry) {
    const auto bundle = unique_path(L"myrd-m4-create-entry-");
    const auto result = run_product(
        L"run --bundle " + quote(bundle.wstring()) +
            L" --size 64M --drive-letter R:", L"available", L"create");
    REQUIRE_EQ(result.exit_code, 0UL);
    REQUIRE_EQ(result.stdout_text, L"Test create entry selected.\r\n");
    REQUIRE(result.stderr_text.empty());
    REQUIRE(!std::filesystem::exists(bundle));
}

TEST_CASE(m4_product_composition_selects_single_recover_entry) {
    const auto bundle = unique_path(L"myrd-m4-recover-entry-");
    std::filesystem::create_directories(bundle);
    std::ofstream(bundle / L"disk.rdm", std::ios::binary) << "manifest";
    const auto result = run_product(
        L"run --bundle " + quote(bundle.wstring()) +
            L" --size 64M --drive-letter R:", L"available", L"recover");
    REQUIRE_EQ(result.exit_code, 0UL);
    REQUIRE_EQ(result.stdout_text, L"Test recover entry selected.\r\n");
    REQUIRE(result.stderr_text.empty());
    require_file_bytes(bundle / L"disk.rdm", "manifest");
    std::filesystem::remove_all(bundle);
}

} // namespace
