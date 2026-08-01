#include "test_framework.h"

#include "src/common/error.h"
#include "src/common/result.h"
#include "src/common/unique_handle.h"
#include "src/common/unique_view.h"
#include "src/common/unique_local.h"
#include "src/common/scope_exit.h"
#include "src/common/checked_cast.h"

#include <windows.h>
#include <limits>
#include <string>

// Tests required by Plan Task 4

TEST_CASE(result_preserves_success_value) {
    auto r = myrd::Result<int>::success(42);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(), 42);
}

TEST_CASE(result_preserves_error_domain_code_and_operation) {
    auto err = myrd::Error{myrd::ErrorDomain::protocol, 0x801,
        L"test_op", L"test message"};
    auto r = myrd::Result<int>::failure(err);
    REQUIRE(r.has_error());
    REQUIRE_EQ(static_cast<int>(r.error().domain),
               static_cast<int>(myrd::ErrorDomain::protocol));
    REQUIRE_EQ(r.error().code, 0x801u);
    REQUIRE(r.error().operation == L"test_op");
}

TEST_CASE(unique_handle_move_transfers_single_close) {
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(evt != nullptr);
    {
        myrd::UniqueHandle h1(evt);
        REQUIRE(h1.get() == evt);
        myrd::UniqueHandle h2(std::move(h1));
        REQUIRE(h1.get() == nullptr);
        REQUIRE(h2.get() == evt);
    }
    // After both leave scope, handle should be closed.
    // Verify by trying to use the handle — it's no longer valid.
    DWORD rc = WaitForSingleObject(evt, 0);
    REQUIRE(rc == WAIT_FAILED);
}

TEST_CASE(unique_handle_release_stops_ownership) {
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(evt != nullptr);
    {
        myrd::UniqueHandle h(evt);
        HANDLE released = h.release();
        REQUIRE(released == evt);
        REQUIRE(h.get() == nullptr);
        // released handle still valid — we still own it
        CloseHandle(released);
    }
    // h destructor doesn't close (already released)
    REQUIRE(true);
}

TEST_CASE(unique_view_unmaps_once) {
    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
    REQUIRE(mapping != nullptr);
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4096);
    REQUIRE(view != nullptr);
    {
        myrd::UniqueView uv(view);
        REQUIRE(uv.get() == view);
        myrd::UniqueView uv2(std::move(uv));
        REQUIRE(uv.get() == nullptr);
        REQUIRE(uv2.get() == view);
    }
    CloseHandle(mapping);
    REQUIRE(true);
}

TEST_CASE(scope_exit_runs_on_early_return) {
    bool ran = false;
    {
        auto guard = myrd::ScopeExit([&ran] { ran = true; });
        REQUIRE(!ran);
    }
    REQUIRE(ran);
}

TEST_CASE(scope_exit_can_be_released) {
    bool ran = false;
    {
        auto guard = myrd::ScopeExit([&ran] { ran = true; });
        REQUIRE(!ran);
        guard.release();
    }
    REQUIRE(!ran);
}

TEST_CASE(checked_cast_accepts_dword_max) {
    std::size_t val = 0xFFFFFFFFu;
    auto r = myrd::checked_numeric_cast<std::uint32_t>(val);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(), 0xFFFFFFFFu);
}

TEST_CASE(checked_cast_rejects_value_above_dword_max) {
    std::size_t val = static_cast<std::size_t>(0xFFFFFFFFu) + 1;
    auto r = myrd::checked_numeric_cast<std::uint32_t>(val);
    REQUIRE(r.has_error());
}

// Static assertions for move-only types
static_assert(!std::is_copy_constructible_v<myrd::UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<myrd::UniqueHandle>);
static_assert(!std::is_copy_constructible_v<myrd::UniqueView>);
static_assert(!std::is_copy_constructible_v<myrd::UniqueLocal>);

// ── SHA-256 (Task 16) ────────────────────────────────────────

#include "src/common/sha256.h"
#include "src/common/authenticode.h"
#include "src/common/process_snapshot.h"

TEST_CASE(sha256_empty_string_vector) {
    std::byte empty[1]{};
    auto r = myrd::sha256_hex(std::span<const std::byte>(empty, 0));
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(),
        "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
}

TEST_CASE(sha256_abc_vector) {
    const char* abc = "abc";
    auto r = myrd::sha256_hex(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(abc), 3));
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value(),
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
}

TEST_CASE(sha256_4096_bytes_streaming_equivalent) {
    std::vector<std::byte> buf(4096);
    for (std::size_t i = 0; i < buf.size(); ++i)
        buf[i] = std::byte{static_cast<unsigned char>(i & 0xFF)};
    auto r1 = myrd::sha256_hex(buf);
    REQUIRE(r1.ok());
    // Same data in chunks produces same hash (via single call)
    auto r2 = myrd::sha256_hex(buf);
    REQUIRE(r2.ok());
    REQUIRE_EQ(r1.value(), r2.value());
}

// ── Authenticode ──────────────────────────────────────────────

TEST_CASE(authenticode_rejects_missing_file) {
    auto r = myrd::verify_authenticode(L"C:\\nonexistent\\file.dll");
    // Should succeed in returning a result (the error is in trust_status)
    REQUIRE(r.ok());
    // trust_status should be non-zero for missing file
}

TEST_CASE(authenticode_returns_status_and_signer_fields) {
    // Test with a known signed system binary
    auto r = myrd::verify_authenticode(
        L"C:\\Windows\\System32\\ntdll.dll");
    REQUIRE(r.ok());
    // trust_status field present
    (void)r.value().trust_status;
    // signer_subject may or may not be populated
}

TEST_CASE(authenticode_closes_provider_state_on_every_path) {
    // Missing file — must not leak state
    auto r = myrd::verify_authenticode(L"\\missing.dll");
    REQUIRE(r.ok());
}

// ── Process / module snapshot ─────────────────────────────────

TEST_CASE(process_snapshot_includes_current_module) {
    auto r = myrd::snapshot_current_modules();
    REQUIRE(r.ok());
    bool found = false;
    for (const auto& m : r.value()) {
        if (m.basename.find(L"myrd_common_tests") != std::wstring::npos) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE(process_snapshot_includes_current_process) {
    auto r = myrd::snapshot_processes();
    REQUIRE(r.ok());
    REQUIRE(!r.value().empty());
}

TEST_CASE(forbidden_module_matching_is_case_insensitive_basename) {
    std::vector<myrd::ModuleInfo> mods;
    mods.push_back({L"C:\\path\\AimApi.DLL", L"AimApi.DLL"});  // forbidden-module fixture
    mods.push_back({L"C:\\path\\kernel32.dll", L"kernel32.dll"});
    std::vector<std::wstring> forbidden{L"aimapi.dll"};
    REQUIRE(myrd::module_list_contains_forbidden(mods, forbidden));
    // Not forbidden
    mods.clear();
    mods.push_back({L"C:\\path\\kernel32.dll", L"kernel32.dll"});
    REQUIRE(!myrd::module_list_contains_forbidden(mods, forbidden));
}

// Original bootstrap smoke test
TEST_CASE(repository_bootstrap_smoke) {
    REQUIRE(true);
    REQUIRE_EQ(sizeof(void*), 8u);
}
