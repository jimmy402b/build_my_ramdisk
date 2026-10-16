// The worker fingerprint store's encoding and its fail-closed reads.
//
// This is the state a sweep reads immediately after a crash, to decide whether the
// process holding a device is its own.  Getting the encoding wrong is not a
// cosmetic bug: a fingerprint that cannot be read back makes its worker
// unattributable, and a fingerprint that can be read back *wrongly* makes an
// unrelated process look like the service's own.
//
// So the round trip is tested, and so are the two failure answers the type exists
// to distinguish -- "never recorded" and "recorded but unreadable" -- because the
// sweep goes to a different place for each.

#include "tests/support/test_framework.h"

#include "src/management/worker_binding_store.h"
#include "src/profile/profile_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace myrd;
using namespace myrd::management;

namespace {

product::Uuid uuid(std::string_view text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

product::Uuid profile_a() {
    return uuid("aaaaaaaa-1111-2222-3333-444444444444");
}

product::Uuid profile_b() {
    return uuid("bbbbbbbb-1111-2222-3333-444444444444");
}

// An in-memory `IAtomicFileDirectory`.  The encoding is what is under test, and
// the real directory needs the ACL-protected ProgramData root, which a development
// machine cannot create -- so the store is given this instead.
class MemoryDirectory final : public profile::IAtomicFileDirectory {
public:
    std::map<std::wstring, std::vector<std::byte>> files;
    // Test switches.  A read failure has to be distinguishable from an absent
    // file, which is the distinction the store's `find` exists to preserve.
    bool fail_reads{false};
    bool fail_listing{false};

    [[nodiscard]] Result<std::vector<std::byte>> read_file(
        std::wstring_view name) override {
        if (fail_reads) {
            return Result<std::vector<std::byte>>::failure(
                {ErrorDomain::internal, 1, L"read_file", L"read refused", 0});
        }
        const auto found = files.find(std::wstring(name));
        if (found == files.end()) {
            // An ABSENT file is a successful read of nothing, not a failure.
            //
            // The real managed directory answers this way, and
            // `publish_current_preserving_previous` depends on it: its first act is
            // to read the destination, so a fake that failed here would make the
            // first write of any record impossible.  The absent-versus-unreadable
            // distinction the store's `find` preserves is drawn from the *listing*,
            // not from this call, which is why `fail_reads` above is a separate
            // switch.
            return Result<std::vector<std::byte>>::success({});
        }
        return Result<std::vector<std::byte>>::success(found->second);
    }

    // `publish_temp` on the real directory refuses a temp that was not read back
    // first -- the check that stops a torn write becoming a half-written record --
    // so the fake has to keep its temps readable, exactly as the real one does.
    [[nodiscard]] Result<profile::AtomicTempFile> create_unique_temp(
        std::wstring_view) override {
        return Result<profile::AtomicTempFile>::success(
            profile::AtomicTempFile{next_temp_++});
    }
    [[nodiscard]] Result<void> write_temp(profile::AtomicTempFile token,
                                          std::span<const std::byte> bytes)
        override {
        temps_[token.value] = std::vector<std::byte>(bytes.begin(), bytes.end());
        return Result<void>::success();
    }
    [[nodiscard]] Result<void> flush_temp(profile::AtomicTempFile) override {
        return Result<void>::success();
    }
    [[nodiscard]] Result<std::vector<std::byte>> read_temp(
        profile::AtomicTempFile token) override {
        const auto found = temps_.find(token.value);
        if (found == temps_.end()) {
            return Result<std::vector<std::byte>>::failure(
                {ErrorDomain::internal, 3, L"read_temp", L"no such temp", 0});
        }
        return Result<std::vector<std::byte>>::success(found->second);
    }
    [[nodiscard]] Result<void> publish_temp(profile::AtomicTempFile token,
                                            std::wstring_view destination,
                                            bool) override {
        const auto found = temps_.find(token.value);
        if (found == temps_.end()) {
            return Result<void>::failure(
                {ErrorDomain::internal, 4, L"publish_temp", L"no such temp", 0});
        }
        files[std::wstring(destination)] = found->second;
        return Result<void>::success();
    }
    [[nodiscard]] Result<void> remove_file(std::wstring_view name) override {
        files.erase(std::wstring(name));
        return Result<void>::success();
    }
    [[nodiscard]] Result<std::vector<std::wstring>> list_files() override {
        if (fail_listing) {
            return Result<std::vector<std::wstring>>::failure(
                {ErrorDomain::internal, 5, L"list_files", L"listing refused", 0});
        }
        std::vector<std::wstring> names;
        for (const auto& entry : files) names.push_back(entry.first);
        return Result<std::vector<std::wstring>>::success(std::move(names));
    }
    [[nodiscard]] Result<void> flush_directory() override {
        return Result<void>::success();
    }
    void discard_temp(profile::AtomicTempFile) noexcept override {}

private:
    std::map<std::uint64_t, std::vector<std::byte>> temps_;
    std::uint64_t next_temp_{1};
};

} // namespace

TEST_CASE(m5_fingerprint_round_trips_through_the_store) {
    // The whole point: what was recorded is what is read back, exactly.  A pid
    // without the creation time is a number Windows recycles, so every field has
    // to survive.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const WorkerFingerprintRecord written{profile_a(), 4242, 133'700'000'000'000ULL, 7};
    REQUIRE(store.record(written).ok());

    // Written through the directory, so a store that only kept it in memory would
    // fail here rather than passing and losing it on restart.
    REQUIRE_EQ(raw->files.size(), 1U);

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(read.value().has_value());
    REQUIRE(*read.value() == written);
}

TEST_CASE(m5_fingerprint_absent_is_not_a_read_failure) {
    // The distinction the store's return type exists for.  "I have no fingerprint
    // for this profile" and "I could not read the fingerprint" send the sweep to
    // different places, and a store that collapsed them would make a corrupt file
    // look like a profile that never had a worker.
    auto directory = std::make_unique<MemoryDirectory>();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    auto absent = store.find(profile_a());
    REQUIRE(absent.ok());
    REQUIRE(!absent.value().has_value());
}

TEST_CASE(m5_fingerprint_that_cannot_be_read_is_a_failure) {
    // Present but unreadable.  The file exists -- the listing says so -- so this is
    // not "never recorded", and it must not be reported as such.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    REQUIRE(store.record(WorkerFingerprintRecord{profile_a(), 1, 2, 3}).ok());
    raw->fail_reads = true;

    auto read = store.find(profile_a());
    REQUIRE(!read.ok());
    REQUIRE(read.error().operation == L"read_file");
}

TEST_CASE(m5_fingerprint_records_are_replaced_not_accumulated) {
    // A fingerprint is "the last worker this profile had".  Accumulating them
    // would leave an old pid attribututable to the profile long after Windows
    // recycled it.
    auto directory = std::make_unique<MemoryDirectory>();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    REQUIRE(store.record(WorkerFingerprintRecord{profile_a(), 111, 1, 1}).ok());
    REQUIRE(store.record(WorkerFingerprintRecord{profile_a(), 222, 2, 2}).ok());

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(read.value().has_value());
    REQUIRE_EQ(read.value()->worker_pid, 222U);
    REQUIRE_EQ(read.value()->worker_process_creation_time, 2U);
    REQUIRE_EQ(read.value()->generation, 2U);
}

TEST_CASE(m5_fingerprint_forget_removes_the_record) {
    // Called when a profile is removed or forgotten: a fingerprint left behind
    // would attribute a later, unrelated process to a profile that no longer
    // exists.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    REQUIRE(store.record(WorkerFingerprintRecord{profile_a(), 1, 2, 3}).ok());
    REQUIRE(store.forget(profile_a()).ok());
    REQUIRE_EQ(raw->files.size(), 0U);

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(!read.value().has_value());
}

TEST_CASE(m5_fingerprint_listing_covers_every_profile_and_only_ours) {
    // `list` is what the sweep turns into expected bindings, so a profile missing
    // from it becomes a worker the sweep cannot attribute.  Files that are not
    // fingerprints are ignored rather than parsed.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    // Something else in the same directory.
    raw->files[L"not-a-fingerprint.json"] = {std::byte{'x'}};

    REQUIRE(store.record(WorkerFingerprintRecord{profile_a(), 10, 20, 1}).ok());
    REQUIRE(store.record(WorkerFingerprintRecord{profile_b(), 30, 40, 2}).ok());

    auto listed = store.list();
    REQUIRE(listed.ok());
    REQUIRE_EQ(listed.value().size(), 2U);
}

TEST_CASE(m5_fingerprint_listing_fails_on_a_corrupt_record) {
    // Dropping it would make its profile silently unattributable -- and the
    // operator would never learn that the file they need to read is broken.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    REQUIRE(store.record(WorkerFingerprintRecord{profile_a(), 10, 20, 1}).ok());
    // A record shaped like ours by name but missing its fields.
    raw->files[L"bbbbbbbb-1111-2222-3333-444444444444.workers"] = {
        std::byte{'p'}, std::byte{'r'}, std::byte{'o'}, std::byte{'f'},
        std::byte{'i'}, std::byte{'l'}, std::byte{'e'}, std::byte{'='}};

    auto listed = store.list();
    REQUIRE(!listed.ok());
    REQUIRE(listed.error().operation == L"worker_binding.decode");
}

TEST_CASE(m5_fingerprint_with_a_bad_profile_id_is_refused) {
    // A record that *is* found for this profile but names an unparseable one is an
    // error, not an absence: it could attribute a worker to no profile at all, and
    // reporting "no record" would hide that.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const std::string text = "profile=not-a-uuid pid=1 created=2 generation=3\n";
    std::vector<std::byte> bytes;
    for (const char value : text) bytes.push_back(static_cast<std::byte>(value));
    // Under this profile's own key, which is the canonical uuid with the hyphens
    // removed -- a dashed name would simply not be found, and the case would pass
    // for the wrong reason.
    raw->files[L"aaaaaaaa111122223333444444444444.workers"] = std::move(bytes);

    auto read = store.find(profile_a());
    REQUIRE(!read.ok());
    REQUIRE(read.error().operation == L"worker_binding.decode");
}

TEST_CASE(m5_fingerprint_under_a_foreign_name_is_not_this_profiles_record) {
    // A file whose *name* is not this profile's is not this profile's record, even
    // if its content claims to be.  The store must answer "no record" rather than
    // reading a neighbour's file -- otherwise one profile's fingerprint could be
    // used to attribute another profile's worker, which is the mis-attribution the
    // whole identity scheme exists to prevent.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const std::string text =
        "profile=aaaaaaaa-1111-2222-3333-444444444444 pid=1 created=2 generation=3\n";
    std::vector<std::byte> bytes;
    for (const char value : text) bytes.push_back(static_cast<std::byte>(value));
    // Stored under profile B's name (hyphens removed, as the store keys them)
    // while claiming to be profile A.
    raw->files[L"bbbbbbbb111122223333444444444444.workers"] = std::move(bytes);

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(!read.value().has_value());
}

// ---------------------------------------------------------------------------
// The bundle identity, which is what decides whether a leftover may be reclaimed.
// ---------------------------------------------------------------------------

namespace {

BundleFileIdentity bundle_identity(const std::uint64_t serial,
                                   const unsigned int first_byte) {
    BundleFileIdentity identity{};
    identity.volume_serial = serial;
    identity.file_id[0] = static_cast<std::byte>(first_byte);
    identity.file_id[15] = std::byte{0x7F};
    return identity;
}

} // namespace

TEST_CASE(m5_fingerprint_carries_the_bundle_identity) {
    // The field that makes a leftover reclaimable.  It has to survive the file, not
    // just the struct: a store that dropped it would make every profile unprovable and
    // the reclamation would never act -- which is the state before this field existed.
    auto directory = std::make_unique<MemoryDirectory>();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    WorkerFingerprintRecord written{profile_a(), 4242, 133'700'000'000'000ULL, 7};
    written.bundle_file_identity = bundle_identity(0xDEAD, 0x11);
    REQUIRE(store.record(written).ok());

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(read.value().has_value());
    REQUIRE(*read.value() == written);
    REQUIRE(read.value()->bundle_file_identity.has_value());
    REQUIRE_EQ(read.value()->bundle_file_identity->volume_serial, 0xDEADU);
    REQUIRE_EQ(read.value()->bundle_file_identity->file_id[15],
               std::byte{0x7F});
}

TEST_CASE(m5_fingerprint_without_a_bundle_identity_round_trips_as_absent) {
    // A record written at launch legitimately has no bundle identity yet -- the worker
    // has not opened the bundle.  Absent must read back as absent rather than as a
    // zero identity: a zero identity would be *present* and would therefore pass
    // `complete()`, authorising a reclamation against an identity nobody established.
    auto directory = std::make_unique<MemoryDirectory>();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const WorkerFingerprintRecord written{profile_a(), 1, 2, 3};
    REQUIRE(written.bundle_file_identity == std::nullopt);
    REQUIRE(store.record(written).ok());

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(read.value().has_value());
    REQUIRE(read.value()->bundle_file_identity == std::nullopt);
}

TEST_CASE(m5_fingerprint_amends_only_the_bundle_identity) {
    // The two facts arrive at different times: the process at launch, the bundle
    // identity once the worker reports it.  Amending must keep the pid and creation
    // time, because those are what make the *process* attributable -- a status that
    // overwrote them would silently detach the fingerprint from its worker.
    auto directory = std::make_unique<MemoryDirectory>();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    REQUIRE(store.record(WorkerFingerprintRecord{profile_a(), 4242, 99, 7}).ok());
    const auto amended =
        store.amend_bundle_identity(profile_a(), bundle_identity(0x1234, 0xAB));
    REQUIRE(amended.ok());
    REQUIRE(amended.value());

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(read.value().has_value());
    REQUIRE_EQ(read.value()->worker_pid, 4242U);
    REQUIRE_EQ(read.value()->worker_process_creation_time, 99U);
    REQUIRE_EQ(read.value()->generation, 7U);
    REQUIRE(read.value()->bundle_file_identity.has_value());
    REQUIRE_EQ(read.value()->bundle_file_identity->volume_serial, 0x1234U);
}

TEST_CASE(m5_fingerprint_amend_without_a_record_reports_so_rather_than_inventing_one) {
    // A worker's status can arrive for a profile whose launch this service never
    // recorded -- after a restart, or for a profile another session created.  Inventing
    // a fingerprint from that status would produce a record with no process identity:
    // one that attributes nothing while looking like it attributes something.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const auto amended =
        store.amend_bundle_identity(profile_a(), bundle_identity(1, 2));
    REQUIRE(amended.ok());
    REQUIRE(!amended.value());
    // And nothing was written.
    REQUIRE_EQ(raw->files.size(), 0U);
}

TEST_CASE(m5_fingerprint_reads_a_record_without_the_bundle_field) {
    // This store writes the bundle field from now on, but a record can legitimately
    // lack it for two reasons: it was written at *launch*, before any worker had
    // opened the bundle, or it was written by a version of this format that had no
    // such field.  Both mean the same thing -- no identity observed -- so both must
    // read as absent.
    //
    // An earlier version of the decoder rejected the missing field, which would have
    // made **every record this store writes at launch unreadable**.  It is kept as a
    // case because that is exactly the kind of change that looks like added strictness
    // and is actually a regression.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const std::string no_bundle_field =
        "profile=aaaaaaaa-1111-2222-3333-444444444444 pid=7 created=8 "
        "generation=9\n";
    std::vector<std::byte> bytes;
    for (const char value : no_bundle_field) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    raw->files[L"aaaaaaaa111122223333444444444444.workers"] =
        std::move(bytes);

    auto read = store.find(profile_a());
    REQUIRE(read.ok());
    REQUIRE(read.value().has_value());
    REQUIRE(read.value()->bundle_file_identity == std::nullopt);
    // The other fields still came through, so the record is usable for attribution.
    REQUIRE_EQ(read.value()->worker_pid, 7U);
    REQUIRE_EQ(read.value()->worker_process_creation_time, 8U);
}

TEST_CASE(m5_fingerprint_refuses_a_malformed_bundle_identity) {
    // An unparseable identity is an error rather than being read as absent: a corrupt
    // identity silently treated as missing is a device nobody will ever reclaim **or**
    // report.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const std::string malformed =
        "profile=aaaaaaaa-1111-2222-3333-444444444444 pid=7 created=8 "
        "generation=9 bundle=notanumber:1,2,3,\n";
    std::vector<std::byte> bytes;
    for (const char value : malformed) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    raw->files[L"aaaaaaaa111122223333444444444444.workers"] =
        std::move(bytes);

    auto read = store.find(profile_a());
    REQUIRE(!read.ok());
    REQUIRE(read.error().operation == L"worker_binding.decode");
}

TEST_CASE(m5_fingerprint_refuses_a_truncated_file_id) {
    // Sixteen bytes is the identity.  A short one is not a weaker identity, it is no
    // identity -- the same rule `WorkerVolumeIdentity::complete` enforces a layer up.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));

    const std::string truncated =
        "profile=aaaaaaaa-1111-2222-3333-444444444444 pid=7 created=8 "
        "generation=9 bundle=5:1,2,3,\n";
    std::vector<std::byte> bytes;
    for (const char value : truncated) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    raw->files[L"aaaaaaaa111122223333444444444444.workers"] =
        std::move(bytes);

    auto read = store.find(profile_a());
    REQUIRE(!read.ok());
    REQUIRE(read.error().operation == L"worker_binding.decode");
}

TEST_CASE(m5_fingerprint_listing_failure_is_reported) {    // A directory that cannot be listed must not look like a profile set of zero,
    // because the sweep would then treat every live worker as a stranger.
    auto directory = std::make_unique<MemoryDirectory>();
    auto* raw = directory.get();
    auto store = WorkerBindingStore::over_directory(std::move(directory));
    raw->fail_listing = true;

    auto listed = store.list();
    REQUIRE(!listed.ok());
    REQUIRE(listed.error().operation == L"list_files");
}
