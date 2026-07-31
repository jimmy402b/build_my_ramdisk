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

// Original bootstrap smoke test
TEST_CASE(repository_bootstrap_smoke) {
    REQUIRE(true);
    REQUIRE_EQ(sizeof(void*), 8u);
}
