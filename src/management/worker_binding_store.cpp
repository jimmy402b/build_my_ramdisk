#include "src/management/worker_binding_store.h"

#include "src/profile/profile_store.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace myrd::management {
namespace {

Error binding_error(const wchar_t* operation, const wchar_t* message,
                    const std::uint32_t native_code = 0) {
    return {ErrorDomain::internal, 1, operation, message, native_code};
}

// One record per line, six fields, and a fixed order.
//
// Text rather than a packed binary struct, for one reason that matters here: this
// file is read immediately after a crash, by a service that has to decide whether
// the process holding a device is its own.  When the answer is "no" an operator
// has to be able to look at the file and see why, and a struct they cannot read is
// an answer nobody can check.
//
// The format has no version field yet and that is deliberate rather than an
// oversight: `WorkerBindingStore::open` creates the directory, so a future format
// change has a natural place to add one, and inventing a version now would be a
// number nobody has ever had to branch on.  It is recorded here so the omission is
// a decision rather than a habit.
std::string encode(const WorkerFingerprintRecord& entry) {
    std::string line;
    line += "profile=" + entry.profile_id.to_string();
    line += " pid=" + std::to_string(entry.worker_pid);
    line += " created=" + std::to_string(entry.worker_process_creation_time);
    line += " generation=" + std::to_string(entry.generation);
    line += '\n';
    return line;
}

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
    out = value;
    return true;
}

std::optional<std::string_view> field(std::string_view line,
                                      std::string_view key) noexcept {
    // `key=` searched with its delimiter so `pid=` cannot match inside `created=`.
    std::string needle(key);
    needle += '=';
    const auto at = line.find(needle);
    if (at == std::string_view::npos) return std::nullopt;
    auto start = at + needle.size();
    // Trim both ends, and both ends are load-bearing.
    //
    // Each field is written as ` key=value`, so the value of every field except
    // the first is preceded by a separator space; and the line ends in a newline,
    // so the value of the *last* field is followed by one.  `from_chars` rejects
    // either, so a parser that trimmed only one end read back neither `pid` (which
    // the leading space broke) nor `generation` (which the trailing newline broke).
    //
    // Both were found by printing the captured bytes rather than by re-reading the
    // parser, which had looked correct three times.
    while (start < line.size() && line[start] == ' ') ++start;
    auto end = line.find(' ', start);
    if (end == std::string_view::npos) end = line.size();
    while (end > start && (line[end - 1] == '\n' || line[end - 1] == '\r'
                           || line[end - 1] == ' ')) {
        --end;
    }
    return line.substr(start, end - start);
}

Result<WorkerFingerprintRecord> decode(std::string_view line) {
    const auto profile = field(line, "profile");
    const auto pid = field(line, "pid");
    const auto created = field(line, "created");
    const auto generation = field(line, "generation");
    if (!profile.has_value() || !pid.has_value() || !created.has_value()
        || !generation.has_value()) {
        return Result<WorkerFingerprintRecord>::failure(binding_error(
            L"worker_binding.decode",
            L"a worker fingerprint record is missing a required field", 0));
    }

    auto parsed_profile = product::Uuid::parse(*profile);
    if (!parsed_profile.ok()) {
        return Result<WorkerFingerprintRecord>::failure(binding_error(
            L"worker_binding.decode",
            L"a worker fingerprint record names an unparseable profile", 0));
    }

    std::uint64_t pid_value = 0;
    std::uint64_t created_value = 0;
    std::uint64_t generation_value = 0;
    if (!parse_u64(*pid, pid_value) || !parse_u64(*created, created_value)
        || !parse_u64(*generation, generation_value)) {
        return Result<WorkerFingerprintRecord>::failure(binding_error(
            L"worker_binding.decode",
            L"a worker fingerprint record has a non-numeric field", 0));
    }

    WorkerFingerprintRecord record{
        parsed_profile.value(),
        static_cast<std::uint32_t>(pid_value),
        created_value,
        generation_value};
    return Result<WorkerFingerprintRecord>::success(std::move(record));
}

std::wstring file_name_for(const product::Uuid& profile_id) {
    // The canonical text with the hyphens removed: a legal file name that is still
    // one-to-one with the profile, so a bad profile cannot collide with a good one.
    //
    // **This exact form is the store's on-disk contract.**  A record written under
    // a dashed name is not found at all, and "not found" is a legitimate answer --
    // so a mismatch here would not fail, it would silently make every fingerprint
    // invisible and leave every worker unattributable.  It is stated here because
    // two of this store's own cases were written against the dashed form and passed
    // for the wrong reason before it was.
    std::string text = profile_id.to_string();
    text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
    std::wstring wide(text.begin(), text.end());
    wide += L".workers";
    return wide;
}

// `std::byte` is not `char`, and the conversions that look obvious do not
// compile, so the bytes-to-text step is stated once here rather than repeated at
// each read site.
std::string as_text(const std::vector<std::byte>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto value : bytes) {
        text.push_back(static_cast<char>(value));
    }
    return text;
}

} // namespace

class WorkerBindingStore::Impl final {
public:
    std::wstring path;
    std::unique_ptr<profile::IAtomicFileDirectory> directory;
};

WorkerBindingStore::WorkerBindingStore(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

WorkerBindingStore::WorkerBindingStore(WorkerBindingStore&&) noexcept = default;
WorkerBindingStore& WorkerBindingStore::operator=(WorkerBindingStore&&) noexcept =
    default;
WorkerBindingStore::~WorkerBindingStore() noexcept = default;

Result<WorkerBindingStore> WorkerBindingStore::open(
    std::wstring_view trusted_program_data_root) {
    // `profiles` is already an ACL-protected managed directory created by the
    // installer, and the fingerprints are the same class of state as the profile
    // records -- so they live inside it rather than in a second protected root
    // with its own ACL to keep in step.
    constexpr std::array<std::wstring_view, 3> components{
        L"MyRamDisk", L"profiles", L"workers"};
    auto managed = profile::open_managed_atomic_directory(
        trusted_program_data_root, components);
    if (!managed.ok()) {
        return Result<WorkerBindingStore>::failure(managed.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->path = managed.value().path;
    impl->directory = std::move(managed.value().directory);
    return Result<WorkerBindingStore>::success(
        WorkerBindingStore(std::move(impl)));
}

WorkerBindingStore WorkerBindingStore::over_directory(
    std::unique_ptr<profile::IAtomicFileDirectory> directory) noexcept {
    auto impl = std::make_unique<Impl>();
    impl->directory = std::move(directory);
    return WorkerBindingStore(std::move(impl));
}

Result<void> WorkerBindingStore::record(const WorkerFingerprintRecord& entry) {
    if (impl_ == nullptr || impl_->directory == nullptr) {
        return Result<void>::failure(binding_error(
            L"worker_binding.record", L"the fingerprint store is not open"));
    }

    // The four-step atomic publish, stated directly rather than through the
    // "current + previous" helper the profile records use.
    //
    // A fingerprint is a *hint*, not the durable authority: it says which process
    // a profile last had, and if the file is lost the sweep reports that profile's
    // worker as unattributable instead of acting on it.  Keeping a previous copy
    // would therefore buy nothing that matters and would mean two files that can
    // disagree about which process a profile owns -- the one reading a sweep must
    // never have to arbitrate.
    const std::string bytes = encode(entry);
    const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
    const std::span<const std::byte> payload(begin, bytes.size());

    auto token = impl_->directory->create_unique_temp(L"worker-fingerprint");
    if (!token.ok()) return Result<void>::failure(token.error());
    auto written = impl_->directory->write_temp(token.value(), payload);
    if (!written.ok()) {
        impl_->directory->discard_temp(token.value());
        return Result<void>::failure(written.error());
    }
    auto flushed = impl_->directory->flush_temp(token.value());
    if (!flushed.ok()) {
        impl_->directory->discard_temp(token.value());
        return Result<void>::failure(flushed.error());
    }
    auto published = impl_->directory->publish_temp(
        token.value(), file_name_for(entry.profile_id), /*replace_existing=*/true);
    if (!published.ok()) {
        impl_->directory->discard_temp(token.value());
        return Result<void>::failure(published.error());
    }
    return Result<void>::success();
}

Result<void> WorkerBindingStore::forget(const product::Uuid& profile_id) {
    if (impl_ == nullptr || impl_->directory == nullptr) {
        return Result<void>::failure(binding_error(
            L"worker_binding.forget", L"the fingerprint store is not open"));
    }
    return impl_->directory->remove_file(file_name_for(profile_id));
}

Result<std::optional<WorkerFingerprintRecord>> WorkerBindingStore::find(
    const product::Uuid& profile_id) const {
    if (impl_ == nullptr || impl_->directory == nullptr) {
        return Result<std::optional<WorkerFingerprintRecord>>::failure(
            binding_error(L"worker_binding.find",
                          L"the fingerprint store is not open"));
    }
    auto bytes = impl_->directory->read_file(file_name_for(profile_id));
    if (!bytes.ok()) {
        // Absent is a legitimate answer and the one the type is shaped around.
        // Distinguished from a read *failure* by asking the directory whether the
        // name exists at all, because "I have no record" and "I could not read the
        // record" lead the sweep to different places.
        auto names = impl_->directory->list_files();
        if (!names.ok()) {
            return Result<std::optional<WorkerFingerprintRecord>>::failure(
                names.error());
        }
        const auto wanted = file_name_for(profile_id);
        const bool present = std::any_of(
            names.value().begin(), names.value().end(),
            [&wanted](const std::wstring& name) { return name == wanted; });
        if (!present) {
            return Result<std::optional<WorkerFingerprintRecord>>::success(
                std::nullopt);
        }
        return Result<std::optional<WorkerFingerprintRecord>>::failure(
            bytes.error());
    }
    const std::string text = as_text(bytes.value());
    auto decoded = decode(text);
    if (!decoded.ok()) {
        return Result<std::optional<WorkerFingerprintRecord>>::failure(
            decoded.error());
    }
    return Result<std::optional<WorkerFingerprintRecord>>::success(
        decoded.move_value());
}

Result<std::vector<WorkerFingerprintRecord>> WorkerBindingStore::list() const {
    if (impl_ == nullptr || impl_->directory == nullptr) {
        return Result<std::vector<WorkerFingerprintRecord>>::failure(
            binding_error(L"worker_binding.list",
                          L"the fingerprint store is not open"));
    }
    auto names = impl_->directory->list_files();
    if (!names.ok()) {
        return Result<std::vector<WorkerFingerprintRecord>>::failure(
            names.error());
    }

    std::vector<WorkerFingerprintRecord> records;
    for (const auto& name : names.value()) {
        // Only this store's own files.  The directory is the service's, but a
        // name filter is cheaper than a parse failure on something that was never
        // a fingerprint.
        if (name.size() < 8 || name.compare(name.size() - 8, 8, L".workers") != 0) {
            continue;
        }
        auto bytes = impl_->directory->read_file(name);
        if (!bytes.ok()) {
            return Result<std::vector<WorkerFingerprintRecord>>::failure(
                bytes.error());
        }
        const std::string text = as_text(bytes.value());
        auto decoded = decode(text);
        if (!decoded.ok()) {
            // One unreadable record fails the whole listing.  Dropping it would
            // make its profile silently unattributable, which is the state the
            // operator most needs to be told about.
            return Result<std::vector<WorkerFingerprintRecord>>::failure(
                decoded.error());
        }
        records.push_back(decoded.move_value());
    }
    return Result<std::vector<WorkerFingerprintRecord>>::success(
        std::move(records));
}

} // namespace myrd::management
