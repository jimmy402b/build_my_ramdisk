#pragma once

// **A path baked into a test binary at configure time is a hint, not the answer (R-17b).**
//
// Several M3/M4 tests are configured with `${CMAKE_SOURCE_DIR}/tests/fixtures/...` and
// `$<TARGET_FILE:myrd>` as compile definitions.  Those are absolute DEV paths, so the guest
// harness has had to stage the tree at exactly the dev path -- a "mirror layout".  That means
// a different checkout location cannot run the suite at all.
//
// The fix is a runtime root: when `MYRD_TEST_ROOT` is set, the last meaningful part of the
// baked path (`tests\fixtures\...` or `out\build\...`) is appended to it.  Unset, the baked
// path is returned unchanged, so the dev machine and every existing command line keep
// working exactly as before.
//
// The helper never guesses: if neither marker is present the baked path is returned, which
// keeps a wrong root visible as a missing file instead of silently reading something else.

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace myrd::test {

namespace detail {

// `_dupenv_s` rather than `getenv`: the test targets compile with warnings-as-errors and
// C4996 would reject the simpler call.
inline std::string environment_root() {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "MYRD_TEST_ROOT") != 0 || value == nullptr) {
        return {};
    }
    std::string root(value);
    std::free(value);
    return root;
}

inline std::string rebase_on_root(const std::string_view baked, std::string root) {
    if (root.empty()) {
        return std::string(baked);
    }
    std::string text(baked);
    std::replace(text.begin(), text.end(), '/', '\\');
    for (const char* const marker : {"tests\\fixtures\\", "out\\build\\"}) {
        const auto at = text.find(marker);
        if (at == std::string::npos) continue;
        std::replace(root.begin(), root.end(), '/', '\\');
        while (!root.empty() && root.back() == '\\') root.pop_back();
        root.push_back('\\');
        root += text.substr(at);
        return root;
    }
    return std::string(baked);
}

inline std::wstring widen(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char value : text) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value)));
    }
    return wide;
}

} // namespace detail

// The narrow form, for the fixture loaders that take `const char*`.
//
// Returns a value, so a caller must not keep the `c_str()` of a temporary: use it inside the
// call expression (`load_text(MYRD_TEST_PATH(X))`) and nowhere else.
inline std::string rebase_test_path(const std::string_view baked) {
    return detail::rebase_on_root(baked, detail::environment_root());
}

// The wide form, for paths that are interpolated into a wide command line.
inline std::wstring rebase_test_path_wide(const std::wstring_view baked) {
    std::string narrow;
    narrow.reserve(baked.size());
    for (const wchar_t value : baked) {
        narrow.push_back(static_cast<char>(static_cast<unsigned char>(value)));
    }
    return detail::widen(detail::rebase_on_root(narrow, detail::environment_root()));
}

} // namespace myrd::test

// Call-site shorthands.  `MYRD_TEST_PATH` yields `const char*` valid for the enclosing
// expression only; `MYRD_TEST_PATH_W` yields a `std::wstring` value.
#define MYRD_TEST_PATH(baked) ::myrd::test::rebase_test_path(baked).c_str()
#define MYRD_TEST_PATH_W(baked) ::myrd::test::rebase_test_path_wide(baked)
