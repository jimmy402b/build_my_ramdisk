#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/windows_srb_transport.h"

#include <cerrno>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>

namespace {

int fail(const myrd::Error& error) {
    std::wcerr << L"error\t" << error.operation << L"\t"
               << error.native_code << L"\t" << error.message
               << std::endl;
    return 3;
}

const wchar_t* option(
    const int argc, wchar_t** argv, const std::wstring_view name) {
    for (int index = 2; index + 1 < argc; ++index) {
        if (std::wstring_view(argv[index]) == name) return argv[index + 1];
    }
    return nullptr;
}

bool unsigned_value(const wchar_t* text, std::uint64_t& value) {
    if (text == nullptr || *text == L'\0' || *text == L'-') return false;
    errno = 0;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != L'\0') return false;
    value = parsed;
    return true;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"usage: fixture list | remove-exact --device-id N "
                      L"--size N --path NATIVE_PATH" << std::endl;
        return 2;
    }
    auto handle = myrd::aim::discover_aim_adapter();
    if (!handle.ok()) return fail(handle.error());
    myrd::aim::WindowsSrbTransport transport(handle.value().handle.get());
    myrd::aim::DirectAimAdapter adapter(transport);
    const std::wstring_view command(argv[1]);
    if (command == L"remove-exact") {
        std::uint64_t id_value = 0;
        std::uint64_t size_value = 0;
        const auto* id_text = option(argc, argv, L"--device-id");
        const auto* size_text = option(argc, argv, L"--size");
        const auto* expected_path = option(argc, argv, L"--path");
        if (argc != 8 || !unsigned_value(id_text, id_value)
            || !unsigned_value(size_text, size_value)
            || id_value > (std::numeric_limits<std::uint32_t>::max)()
            || size_value == 0 || expected_path == nullptr) {
            std::wcerr << L"invalid exact-removal arguments" << std::endl;
            return 2;
        }
        const myrd::aim::DeviceId id{
            static_cast<std::uint32_t>(id_value)};
        if (id.value == myrd::aim::kAllDevices) {
            std::wcerr << L"sentinel device id rejected" << std::endl;
            return 2;
        }
        auto info = adapter.query_device(id);
        if (!info.ok()) return fail(info.error());
        if (info.value().size_bytes != size_value
            || info.value().native_object_path != expected_path
            || !myrd::aim::has_m1_product_proxy_flags(info.value().flags)) {
            std::wcerr << L"exact device identity mismatch" << std::endl;
            return 4;
        }
        auto removed = adapter.remove_device(id);
        if (!removed.ok()) return fail(removed.error());
        auto rescanned = myrd::aim::rescan_aim_adapter(
            handle.value().dev_inst);
        if (!rescanned.ok()) return fail(rescanned.error());
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(30);
        for (;;) {
            auto remaining = adapter.list_devices();
            if (!remaining.ok()) return fail(remaining.error());
            bool present = false;
            for (const auto candidate : remaining.value()) {
                if (candidate == id) present = true;
            }
            if (!present) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                std::wcerr << L"exact device remained registered"
                           << std::endl;
                return 5;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::wcout << L"removed\t" << id.value << std::endl;
        return 0;
    }
    if (command != L"list" || argc != 2) {
        std::wcerr << L"unknown command" << std::endl;
        return 2;
    }

    auto devices = adapter.list_devices();
    if (!devices.ok()) return fail(devices.error());

    std::wcout << L"adapter\t" << handle.value().dev_inst << L"\t"
               << devices.value().size() << std::endl;
    for (const auto id : devices.value()) {
        auto info = adapter.query_device(id);
        if (!info.ok()) return fail(info.error());
        std::wcout << L"device\t" << id.value << L"\t"
                   << info.value().size_bytes << L"\t"
                   << info.value().sector_size << L"\t"
                   << info.value().flags << L"\t"
                   << info.value().native_object_path << std::endl;
    }
    return 0;
}
