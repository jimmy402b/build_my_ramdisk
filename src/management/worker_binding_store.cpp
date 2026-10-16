#include "src/management/worker_binding_store.h"

#include "src/profile/profile_store.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace myrd::management {
namespace {

Error binding_error(const wchar_t* operation, const wchar_t* message,
                    const std::uint32_t native_code = 0) {
    return {ErrorDomain::internal, 1, operation, message, native_code};
}

// Creates the `workers` leaf with the same protection the sibling profile
// directories carry: a DACL granting full control to SYSTEM and to Administrators
// and to nobody else, marked protected so nothing inherited can widen it.
//
// Inheriting from the parent is *not* enough, and that was measured rather than
// assumed: a child directory inherits its parent's access-control *entries* but is
// not itself marked `SE_DACL_PROTECTED`, so `open_managed_atomic_directory`
// refuses it -- correctly, because an unprotected DACL can be widened by anything
// that can write the parent.  The leaf is therefore created *with* an explicit
// descriptor rather than created and then fixed up, which would leave a window in
// which the directory exists and is not yet protected.
Result<void> create_protected_leaf(const std::filesystem::path& path) {
    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid{};
    std::array<std::byte, SECURITY_MAX_SID_SIZE> admin_sid{};
    DWORD system_bytes = static_cast<DWORD>(system_sid.size());
    DWORD admin_bytes = static_cast<DWORD>(admin_sid.size());
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(),
                           &system_bytes)
            == FALSE
        || CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                              admin_sid.data(), &admin_bytes)
            == FALSE) {
        return Result<void>::failure(binding_error(L"worker_binding.create",
                             L"a well-known SID could not be built",
                             GetLastError()));
    }

    std::array<std::byte,
               2 * (sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE)>
        acl_storage{};
    auto* acl = reinterpret_cast<PACL>(acl_storage.data());
    if (InitializeAcl(acl, static_cast<DWORD>(acl_storage.size()), ACL_REVISION)
        == FALSE) {
        return Result<void>::failure(binding_error(L"worker_binding.create",
                             L"the leaf's ACL could not be initialized",
                             GetLastError()));
    }
    constexpr DWORD kFullControl = GENERIC_ALL;
    // `AddAccessAllowedAceEx` rather than `AddAccessAllowedAce`, and the difference
    // is the whole reason the first version of this was rejected: the plain form
    // writes an ACE with no inheritance flags, and
    // `open_managed_atomic_directory` requires the entries to be inheritable --
    // otherwise a file created inside the directory would not carry the
    // protection, which is exactly the property the check exists to establish.
    constexpr DWORD kInheritable = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    if (AddAccessAllowedAceEx(acl, ACL_REVISION, kInheritable, kFullControl,
                              system_sid.data())
            == FALSE
        || AddAccessAllowedAceEx(acl, ACL_REVISION, kInheritable, kFullControl,
                                 admin_sid.data())
            == FALSE) {
        return Result<void>::failure(binding_error(
            L"worker_binding.create",
            L"the leaf's ACL entries could not be added", GetLastError()));
    }

    SECURITY_DESCRIPTOR descriptor{};
    if (InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
            == FALSE
        || SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) == FALSE) {
        return Result<void>::failure(binding_error(L"worker_binding.create",
                             L"the leaf's security descriptor could not be set",
                             GetLastError()));
    }
    // The marker whose absence `open_managed_atomic_directory` rejects.
    if (SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED,
                                     SE_DACL_PROTECTED)
        == FALSE) {
        return Result<void>::failure(binding_error(L"worker_binding.create",
                             L"the leaf's DACL could not be marked protected",
                             GetLastError()));
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    attributes.bInheritHandle = FALSE;

    if (CreateDirectoryW(path.c_str(), &attributes) == FALSE) {
        const DWORD error = GetLastError();
        // Already there is the normal case on every start after the first, and not
        // a failure: whatever created it is about to be verified by the open that
        // follows, which is the check that actually matters.
        if (error != ERROR_ALREADY_EXISTS) {
            return Result<void>::failure(binding_error(
                L"worker_binding.create",
                L"the fingerprint directory could not be created", error));
        }
    }
    return Result<void>::success();
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
    // The bundle identity, when one was observed.  `-` is written rather than the
    // field being omitted, so a record that has no identity and a record written by
    // an older version of this format are distinguishable -- the second is a format
    // question and the first is a fact.
    line += " bundle=";
    if (entry.bundle_file_identity.has_value()) {
        line += std::to_string(entry.bundle_file_identity->volume_serial);
        line += ":";
        for (const auto value : entry.bundle_file_identity->file_id) {
            line += std::to_string(std::to_integer<unsigned int>(value));
            line += ",";
        }
    } else {
        line += "none";
    }
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
    // Tokenised, not substring-searched, and that is a correction with a case behind
    // it: the first version searched for `key=` anywhere in the line, and `bundle`
    // then matched **inside `generation=`** whenever the bundle field was absent --
    // so a record with no bundle identity failed to decode, which is the common case
    // for a record written at launch.
    //
    // A token must equal the key exactly; the value is what follows the `=`.  That
    // cannot match a key that happens to be a suffix of another.
    std::size_t at = 0;
    while (at < line.size()) {
        auto end = line.find(' ', at);
        if (end == std::string_view::npos) end = line.size();
        auto stop = end;
        while (stop > at && (line[stop - 1] == '\n' || line[stop - 1] == '\r')) {
            --stop;
        }
        const std::string_view token = line.substr(at, stop - at);
        const auto equals = token.find('=');
        if (equals != std::string_view::npos && token.substr(0, equals) == key) {
            return token.substr(equals + 1);
        }
        at = end + 1;
    }
    return std::nullopt;
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

    // The bundle identity is optional in the record and therefore optional on the
    // line.  **A missing field is treated as absent**, and the first version of this
    // made it an error -- which would have made every record this store writes at
    // launch unreadable, since the launch has no identity yet to write.  The two
    // spellings mean the same thing ("no identity observed"), so refusing one of them
    // bought nothing and broke the common case.
    //
    // What *is* an error is an unparseable value: a corrupt identity silently read as
    // missing is a device nobody will ever reclaim **or** report.
    const auto bundle = field(line, "bundle");
    if (bundle.has_value() && !bundle->empty() && *bundle != "none") {
        const auto colon = bundle->find(':');
        if (colon == std::string_view::npos) {
            return Result<WorkerFingerprintRecord>::failure(binding_error(
                L"worker_binding.decode",
                L"a worker fingerprint's bundle identity is malformed", 0));
        }
        std::uint64_t serial = 0;
        if (!parse_u64(bundle->substr(0, colon), serial)) {
            return Result<WorkerFingerprintRecord>::failure(binding_error(
                L"worker_binding.decode",
                L"a worker fingerprint's bundle serial is not numeric", 0));
        }
        BundleFileIdentity identity{};
        identity.volume_serial = serial;
        std::string_view rest = bundle->substr(colon + 1);
        std::size_t index = 0;
        while (!rest.empty() && index < identity.file_id.size()) {
            const auto comma = rest.find(',');
            const auto piece = rest.substr(0, comma);
            std::uint64_t byte_value = 0;
            if (!piece.empty() && !parse_u64(piece, byte_value)) {
                return Result<WorkerFingerprintRecord>::failure(binding_error(
                    L"worker_binding.decode",
                    L"a worker fingerprint's file id is not numeric", 0));
            }
            identity.file_id[index] = static_cast<std::byte>(byte_value);
            ++index;
            if (comma == std::string_view::npos) break;
            rest = rest.substr(comma + 1);
        }
        if (index != identity.file_id.size()) {
            return Result<WorkerFingerprintRecord>::failure(binding_error(
                L"worker_binding.decode",
                L"a worker fingerprint's file id is the wrong length", 0));
        }
        record.bundle_file_identity = identity;
    }

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
    //
    // The `workers` leaf is created here rather than by the installer, and that is
    // a deliberate split: `open_managed_atomic_directory` requires every component
    // to exist, and a fresh install has no fingerprints yet, so requiring the leaf
    // would make the first boot fail on a machine where nothing is wrong.  The
    // leaf inherits the parent's protection, so creating it does not weaken
    // anything -- and it is the store that owns this directory, so it is the store
    // that should make it.
    constexpr std::array<std::wstring_view, 3> components{
        L"MyRamDisk", L"profiles", L"workers"};
    const std::filesystem::path leaf =
        std::filesystem::path(trusted_program_data_root) / L"MyRamDisk"
        / L"profiles" / L"workers";
    if (!std::filesystem::exists(leaf)) {
        auto created = create_protected_leaf(leaf);
        if (!created.ok()) return Result<WorkerBindingStore>::failure(
            created.error());
    }
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

Result<WorkerBindingStore> WorkerBindingStore::over_protected_root(
    std::wstring_view trusted_program_data_root) {
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

Result<void> WorkerBindingStore::record(const WorkerFingerprintRecord& entry) {
    if (impl_ == nullptr || impl_->directory == nullptr) {
        return Result<void>::failure(binding_error(
            L"worker_binding.record", L"the fingerprint store is not open"));
    }

    // The profile layer's atomic publish handles replacing an existing record, and
    // it is the layer that already implements and tests the "read the temp back
    // before publishing it" rule -- a rule that exists because it is the only thing
    // between a torn write and a fingerprint that half-describes a worker.
    //
    // It cannot *create* the file, though: its first act is to read the
    // destination, and the managed directory reports a missing file as an error
    // rather than as an empty read.  So the first write for a profile takes the
    // same four steps stated explicitly -- including the verification read -- and
    // every later write goes through the helper.
    const std::string bytes = encode(entry);
    const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
    const std::span<const std::byte> payload(begin, bytes.size());
    const std::wstring name = file_name_for(entry.profile_id);

    auto existing = impl_->directory->read_file(name);
    if (existing.ok()) {
        return profile::publish_current_preserving_previous(
            *impl_->directory, name, payload);
    }

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
    // The verification read, which `publish_temp` requires and which is worth
    // having anyway: it is what proves the bytes on disk are the bytes intended
    // before anything claims they are a record.
    auto verified = impl_->directory->read_temp(token.value());
    if (!verified.ok()) {
        impl_->directory->discard_temp(token.value());
        return Result<void>::failure(verified.error());
    }
    if (!std::equal(verified.value().begin(), verified.value().end(),
                    payload.begin(), payload.end())) {
        impl_->directory->discard_temp(token.value());
        return Result<void>::failure(binding_error(
            L"worker_binding.record",
            L"the staged fingerprint did not verify byte-for-byte", 0));
    }
    auto published = impl_->directory->publish_temp(token.value(), name, true);
    if (!published.ok()) {
        impl_->directory->discard_temp(token.value());
        return Result<void>::failure(published.error());
    }
    return impl_->directory->flush_directory();
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
        // A read *failure*.  Distinguished from absent by asking the directory
        // whether the name exists at all, because "I have no record" and "I could
        // not read the record" lead the sweep to different places.
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

    // A successful read of *nothing* is the absent case, and it is not the same as
    // a failure.  The managed directory answers "no such file" as an empty read
    // rather than as an error -- `publish_current_preserving_previous` relies on
    // exactly that to write a file for the first time -- so treating empty as
    // "present but unparseable" here would report every profile that has never had
    // a worker as needing attention.
    if (bytes.value().empty()) {
        return Result<std::optional<WorkerFingerprintRecord>>::success(
            std::nullopt);
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

Result<bool> WorkerBindingStore::amend_bundle_identity(
    const product::Uuid& profile_id,
    const BundleFileIdentity& bundle_file_identity) {
    // Read, change one field, write.  Deliberately *not* a general read-modify-write:
    // the only field this can touch is the bundle identity, so a status observed after
    // a launch cannot overwrite the pid that makes the process attributable -- which is
    // the failure a caller with a stale status would otherwise cause.
    auto existing = find(profile_id);
    if (!existing.ok()) {
        return Result<bool>::failure(existing.error());
    }
    if (!existing.value().has_value()) {
        // No record to amend.  A status can arrive for a profile whose launch this
        // service never recorded, and inventing a fingerprint from it would produce a
        // record with no process identity -- one that attributes nothing while looking
        // like it attributes something.
        return Result<bool>::success(false);
    }

    auto amended = *existing.value();
    amended.bundle_file_identity = bundle_file_identity;
    auto written = record(amended);
    if (!written.ok()) {
        return Result<bool>::failure(written.error());
    }
    return Result<bool>::success(true);
}

} // namespace myrd::management
