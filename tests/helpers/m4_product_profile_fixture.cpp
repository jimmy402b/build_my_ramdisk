#include "src/profile/profile_store.h"
#include "src/profile/profile_validation.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/production_policy.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <sddl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using myrd::product::Uuid;
using namespace myrd::profile;

std::optional<std::wstring_view> option(
    const int argc, wchar_t** argv, const std::wstring_view name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::wstring_view(argv[index]) == name) return argv[index + 1];
    }
    return std::nullopt;
}

std::optional<std::uint64_t> unsigned_option(
    const int argc, wchar_t** argv, const std::wstring_view name) {
    const auto value = option(argc, argv, name);
    if (!value || value->empty()) return std::nullopt;
    std::uint64_t parsed = 0;
    for (const auto character : *value) {
        if (character < L'0' || character > L'9') return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(character - L'0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit)
                / 10) {
            return std::nullopt;
        }
        parsed = parsed * 10 + digit;
    }
    return parsed;
}

std::optional<Uuid> uuid_option(
    const int argc, wchar_t** argv, const std::wstring_view name) {
    const auto value = option(argc, argv, name);
    if (!value || value->size() != 36) return std::nullopt;
    std::string narrow;
    narrow.reserve(value->size());
    for (const auto character : *value) {
        if (character > 0x7f) return std::nullopt;
        narrow.push_back(static_cast<char>(character));
    }
    auto parsed = Uuid::parse(narrow);
    if (!parsed.ok() || parsed.value().to_string() != narrow) {
        return std::nullopt;
    }
    return parsed.move_value();
}

bool create_protected_directory(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    LocalFree(descriptor);
    return created != FALSE;
}

int fail(const wchar_t* message) {
    std::wcerr << message << L'\n';
    return 2;
}

std::optional<Uuid> product_uuid(
    const myrd::persistence::ImageUuid& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string canonical;
    canonical.reserve(36);
    for (std::size_t index = 0; index < value.bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            canonical.push_back('-');
        }
        const auto byte = static_cast<unsigned char>(value.bytes[index]);
        canonical.push_back(digits[byte >> 4U]);
        canonical.push_back(digits[byte & 0x0fU]);
    }
    auto parsed = Uuid::parse(canonical);
    if (!parsed.ok()) return std::nullopt;
    return parsed.move_value();
}

void write_json_string(std::wostream& output, const std::wstring_view value) {
    output << L'"';
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': output << L"\\\""; break;
        case L'\\': output << L"\\\\"; break;
        case L'\b': output << L"\\b"; break;
        case L'\f': output << L"\\f"; break;
        case L'\n': output << L"\\n"; break;
        case L'\r': output << L"\\r"; break;
        case L'\t': output << L"\\t"; break;
        default:
            if (character < 0x20 || character > 0x7e) {
                const auto flags = output.flags();
                const auto fill = output.fill();
                output << L"\\u" << std::hex << std::nouppercase
                       << std::setw(4) << std::setfill(L'0')
                       << static_cast<unsigned int>(character);
                output.flags(flags);
                output.fill(fill);
            } else {
                output << character;
            }
        }
    }
    output << L'"';
}

std::optional<std::wstring> utf8_to_utf16(const std::string_view value) {
    if (value.empty()) return std::wstring{};
    if (value.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const auto input_size = static_cast<int>(value.size());
    const int output_size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (output_size <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(output_size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size,
            wide.data(), output_size) != output_size) {
        return std::nullopt;
    }
    return wide;
}

bool write_json_string(std::wostream& output, const std::string_view value) {
    const auto wide = utf8_to_utf16(value);
    if (!wide) return false;
    write_json_string(output, *wide);
    return true;
}

int inspect_profile(const int argc, wchar_t** argv) {
    const auto program_data = option(argc, argv, L"--program-data");
    const auto profile_id = uuid_option(argc, argv, L"--profile-id");
    if (!program_data || !profile_id) {
        return fail(L"fixture inspect arguments are incomplete or invalid");
    }
    auto store = ProfileStore::open_existing(*program_data, *profile_id);
    if (!store.ok()) return fail(L"protected profile store did not open");
    auto loaded = store.value().load_current();
    if (!loaded.ok()) return fail(L"current profile did not load");
    const auto& record = loaded.value();
    if (!validate_profile(record).ok()
        || record.profile.profile_uuid != *profile_id) {
        return fail(L"current profile did not validate");
    }

    myrd::persistence::ImageInspectOptions inspect_options;
    inspect_options.manifest_relative_name = record.profile.manifest_name;
    inspect_options.recovery.limits =
        myrd::persistence::production_persistence_limits(
            record.profile.logical_capacity_bytes);
    const myrd::OperationContext inspect_context{
        {}, std::chrono::steady_clock::now() + std::chrono::minutes(2)};
    auto inspected = myrd::persistence::inspect_image(
        inspect_context, std::filesystem::path(record.profile.bundle_path),
        inspect_options);
    if (!inspected.ok() || !inspected.value().selected.has_value()) {
        return fail(L"bundle has no complete selected image");
    }
    const auto& selected = *inspected.value().selected;
    const auto selected_id = product_uuid(selected.image_uuid);
    if (!selected_id || *selected_id != record.profile.image_uuid
        || selected.geometry.virtual_bytes
            != record.profile.logical_capacity_bytes
        || selected.geometry.logical_sector_size
            != record.profile.logical_sector_size
        || selected.geometry.physical_sector_size
            != record.profile.physical_sector_size) {
        return fail(L"selected image does not match the profile binding");
    }

    // Decode before emitting any JSON so invalid UTF-8 cannot produce a
    // partial or falsely successful document.
    const auto profile_id_text = record.profile.profile_uuid.to_string();
    const auto image_id_text = selected_id->to_string();
    const auto manifest_name = utf8_to_utf16(record.profile.manifest_name);
    if (!manifest_name) return fail(L"profile manifest name is not valid UTF-8");

    std::wostringstream json;
    json << L"{\"profile_id\":";
    if (!write_json_string(json, profile_id_text)) {
        return fail(L"canonical profile UUID was not valid UTF-8");
    }
    json << L",\"revision\":" << record.revision
         << L",\"bundle_root\":";
    write_json_string(json, record.profile.bundle_path);
    json << L",\"manifest_name\":";
    write_json_string(json, *manifest_name);
    json << L",\"image_id\":";
    if (!write_json_string(json, image_id_text)) {
        return fail(L"canonical image UUID was not valid UTF-8");
    }
    json << L",\"selected_generation\":" << selected.checkpoint_generation
         << L",\"valid\":true}\n";
    if (!json) return fail(L"fixture failed to serialize inspect JSON");
    const auto document = json.str();
    std::wcout.write(document.data(),
                     static_cast<std::streamsize>(document.size()));
    std::wcout.flush();
    if (!std::wcout) return fail(L"fixture failed to write inspect JSON");
    return 0;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"inspect") {
        try {
            return inspect_profile(argc, argv);
        } catch (...) {
            return fail(L"fixture inspect failed with an exception");
        }
    }
    if (argc < 2 || std::wstring_view(argv[1]) != L"create") {
        return fail(L"usage: fixture create ... | fixture inspect "
                    L"--program-data PATH --profile-id UUID\n"
                    L"create: --program-data PATH --bundle-root "
                    L"PATH --profile-id UUID --disk-id UUID "
                    L"--partition-id UUID --volume-id UUID --capacity BYTES");
    }
    const auto program_data = option(argc, argv, L"--program-data");
    const auto bundle = option(argc, argv, L"--bundle-root");
    const auto profile_id = uuid_option(argc, argv, L"--profile-id");
    const auto disk_id = uuid_option(argc, argv, L"--disk-id");
    const auto partition_id = uuid_option(argc, argv, L"--partition-id");
    const auto volume_id = uuid_option(argc, argv, L"--volume-id");
    const auto capacity = unsigned_option(argc, argv, L"--capacity");
    if (!program_data || !bundle || !profile_id || !disk_id
        || !partition_id || !volume_id || !capacity || *capacity == 0) {
        return fail(L"fixture arguments are incomplete or invalid");
    }
    try {
        myrd::persistence::ImageInspectOptions inspect_options;
        inspect_options.manifest_relative_name = "disk.rdm";
        inspect_options.recovery.limits =
            myrd::persistence::production_persistence_limits(*capacity);
        const myrd::OperationContext inspect_context{
            {}, std::chrono::steady_clock::now() + std::chrono::minutes(2)};
        auto inspected = myrd::persistence::inspect_image(
            inspect_context, std::filesystem::path(*bundle), inspect_options);
        if (!inspected.ok() || !inspected.value().selected.has_value()) {
            return fail(L"bundle has no complete selected image");
        }
        const auto& selected = inspected.value().selected.value();
        auto image_id = product_uuid(selected.image_uuid);
        if (!image_id
            || selected.geometry.virtual_bytes != *capacity
            || selected.geometry.logical_sector_size != 512
            || selected.geometry.physical_sector_size == 0) {
            return fail(L"bundle identity or geometry is incompatible");
        }
        const auto root = std::filesystem::path(*program_data);
        const auto product_root = root / L"MyRamDisk";
        const auto profiles_root = product_root / L"profiles";
        const auto id = profile_id->to_string();
        const auto profile_root = profiles_root
            / std::wstring(id.begin(), id.end());
        if (std::filesystem::exists(product_root)
            || !create_protected_directory(product_root)
            || !create_protected_directory(profiles_root)
            || !create_protected_directory(profile_root)) {
            return fail(L"fixture refused an existing or unsafe profile root");
        }

        Profile profile{
            *profile_id,
            L"S-1-5-18",
            L"Task23 product worker probe",
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()),
            ProfileLifecycle::ready,
            std::wstring(*bundle),
            "disk.rdm",
            *image_id,
            *capacity,
            selected.geometry.logical_sector_size,
            selected.geometry.physical_sector_size,
            *disk_id,
            *partition_id,
            *volume_id,
            MountKind::drive_letter,
            L"R:",
            true,
            MemoryPolicy{
                MemoryMode::automatic,
                512ULL << 20,
                384ULL << 20,
                256ULL << 20,
                64ULL << 20},
            CheckpointPolicy{},
            CompactionPolicy{}};
        ProfileRecord record{1, std::move(profile)};
        const auto valid = validate_profile(record);
        if (!valid.ok()) return fail(L"generated profile did not validate");
        auto store = ProfileStore::open_existing(
            std::wstring(*program_data), *profile_id);
        if (!store.ok()) return fail(L"protected profile store did not open");
        auto written = store.value().write_next(0, record, false);
        if (!written.ok()) return fail(L"profile publication failed");
        const auto image_text = image_id->to_string();
        std::wcout << L"{\"profile_id\":\""
                   << std::wstring(id.begin(), id.end())
                   << L"\",\"image_id\":\""
                   << std::wstring(image_text.begin(), image_text.end())
                   << L"\",\"generation\":"
                   << selected.checkpoint_generation
                   << L",\"created\":true}" << std::endl;
        return 0;
    } catch (...) {
        return fail(L"fixture failed with an exception");
    }
}
