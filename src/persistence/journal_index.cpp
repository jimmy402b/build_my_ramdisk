#include "src/persistence/journal_index.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace myrd::persistence {
namespace {

#if defined(MYRD_ENABLE_TEST_SEAMS)
std::atomic<std::uint64_t> g_visited_guard_calls{0};
std::atomic<std::uint64_t> g_depth_guard_calls{0};
#endif

template <typename Buffer>
bool all_zero(const Buffer& bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{0};
    });
}

template <typename T>
Result<T> index_failure(std::wstring_view operation,
                        std::wstring_view message,
                        const PersistenceErrorCode code =
                            PersistenceErrorCode::JournalCorrupt) {
    return Result<T>::failure({
        ErrorDomain::persistence, static_cast<std::uint32_t>(code),
        std::wstring{operation}, std::wstring{message}, 0});
}

bool empty_root(const PersistentIndexRootDescriptor& root) noexcept {
    return root.offset == 0 && root.length == 0 && root.crc32c == 0;
}

Result<void> validate_view(const JournalIndexView& view) {
    if (!view.file || all_zero(view.file_identity.file_id) ||
        all_zero(view.journal_identity.journal_uuid) || view.epoch == 0 ||
        view.epoch == kInvalidGeneration ||
        view.root_generation == kInvalidGeneration ||
        view.committed_length < kJournalAlignment ||
        view.committed_length % kJournalAlignment != 0) {
        return index_failure<void>(L"JournalIndexReader::lookup",
                                   L"immutable index view is invalid");
    }
    if (empty_root(view.root)) return Result<void>::success();
    if (view.root.offset == 0 || view.root.length != kIndexNodeBytes ||
        view.root_generation == 0 ||
        view.root.offset % kJournalAlignment != 0 ||
        view.root.offset > view.committed_length ||
        view.root.length > view.committed_length - view.root.offset) {
        return index_failure<void>(L"JournalIndexReader::lookup",
                                   L"index root range is invalid");
    }
    return Result<void>::success();
}

struct CacheKey final {
    std::uint64_t volume_serial_number{0};
    std::array<std::byte, 16> file_id{};
    std::array<std::byte, 16> journal_uuid{};
    JournalEpoch epoch{0};
    CheckpointGeneration root_generation{0};
    std::uint64_t committed_length{0};
    std::uint64_t root_offset{0};
    std::uint32_t root_crc32c{0};
    std::uint64_t node_offset{0};

    bool operator==(const CacheKey&) const noexcept = default;
};

void hash_mix(std::size_t& value, const std::uint64_t input) noexcept {
    constexpr auto constant = static_cast<std::size_t>(
        0x9E3779B97F4A7C15ULL);
    value ^= static_cast<std::size_t>(input) + constant +
             (value << 6U) + (value >> 2U);
}

struct CacheKeyHash final {
    std::size_t operator()(const CacheKey& key) const noexcept {
        std::size_t value = 0;
        hash_mix(value, key.volume_serial_number);
        for (const auto byte : key.file_id) {
            hash_mix(value, std::to_integer<std::uint8_t>(byte));
        }
        for (const auto byte : key.journal_uuid) {
            hash_mix(value, std::to_integer<std::uint8_t>(byte));
        }
        hash_mix(value, key.epoch);
        hash_mix(value, key.root_generation);
        hash_mix(value, key.committed_length);
        hash_mix(value, key.root_offset);
        hash_mix(value, key.root_crc32c);
        hash_mix(value, key.node_offset);
        return value;
    }
};

CacheKey make_cache_key(const JournalIndexView& view,
                        const std::uint64_t node_offset) noexcept {
    return {view.file_identity.volume_serial_number,
            view.file_identity.file_id,
            view.journal_identity.journal_uuid,
            view.epoch,
            view.root_generation,
            view.committed_length,
            view.root.offset,
            view.root.crc32c,
            node_offset};
}

std::optional<JournalIndexLookupKind> lookup_kind(
    const JournalRecordType type) noexcept {
    switch (type) {
    case JournalRecordType::Data:
        return JournalIndexLookupKind::Data;
    case JournalRecordType::Zero:
        return JournalIndexLookupKind::Zero;
    case JournalRecordType::Discard:
        return JournalIndexLookupKind::Discard;
    default:
        return std::nullopt;
    }
}

struct ExpectedNode final {
    std::uint32_t crc32c{0};
    CheckpointGeneration generation{0};
    std::optional<std::uint16_t> level{};
    std::optional<std::uint64_t> minimum_key{};
    std::optional<std::uint64_t> maximum_key{};
};

Result<void> validate_visited_offset(
    const std::span<const std::uint64_t> visited,
    const std::uint64_t candidate) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    g_visited_guard_calls.fetch_add(1, std::memory_order_relaxed);
#endif
    if (std::find(visited.begin(), visited.end(), candidate) !=
        visited.end()) {
        return index_failure<void>(L"JournalIndexReader::lookup",
                                   L"index cycle detected");
    }
    return Result<void>::success();
}

Result<void> validate_descent_depth(const std::uint32_t depth,
                                    const std::uint16_t node_level) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    g_depth_guard_calls.fetch_add(1, std::memory_order_relaxed);
#endif
    if (node_level != 0 && depth >= kMaxIndexDepth - 1) {
        return index_failure<void>(L"JournalIndexReader::lookup",
                                   L"index exceeds maximum depth");
    }
    return Result<void>::success();
}

}  // namespace

struct JournalIndexReader::Impl final {
    struct CacheEntry final {
        std::shared_ptr<const DecodedJournalIndexNode> node{};
        std::list<CacheKey>::iterator lru_iterator{};
    };

    explicit Impl(const JournalIndexCacheOptions source_options)
        : options(source_options) {}

    Result<std::shared_ptr<const DecodedJournalIndexNode>> load_node(
        const OperationContext& context,
        const JournalIndexView& view,
        const std::uint64_t node_offset) {
        auto context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<
                std::shared_ptr<const DecodedJournalIndexNode>>::failure(
                context_result.error());
        }
        const auto key = make_cache_key(view, node_offset);
        {
            std::lock_guard lock(mutex);
            context_result = check_operation_context(context);
            if (!context_result.ok()) {
                return Result<
                    std::shared_ptr<const DecodedJournalIndexNode>>::failure(
                    context_result.error());
            }
            const auto cached = cache.find(key);
            if (cached != cache.end()) {
                lru.splice(lru.begin(), lru, cached->second.lru_iterator);
                ++stats.hits;
                return Result<std::shared_ptr<const DecodedJournalIndexNode>>::
                    success(cached->second.node);
            }
            ++stats.misses;
            ++stats.read_calls;
        }

        std::array<std::byte, kIndexNodeBytes> bytes{};
        auto read = view.file->read_exact(context, node_offset, bytes);
        if (!read.ok()) {
            return Result<std::shared_ptr<const DecodedJournalIndexNode>>::
                failure(read.error());
        }
        context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<
                std::shared_ptr<const DecodedJournalIndexNode>>::failure(
                context_result.error());
        }
        auto decoded = decode_index_node(bytes, node_offset,
                                         view.committed_length);
        if (!decoded.ok()) {
            if (decoded.error().domain == ErrorDomain::internal) {
                return Result<
                    std::shared_ptr<const DecodedJournalIndexNode>>::failure(
                    decoded.error());
            }
            return index_failure<
                std::shared_ptr<const DecodedJournalIndexNode>>(
                L"JournalIndexReader::lookup",
                L"index node failed strict decoding");
        }

        std::shared_ptr<const DecodedJournalIndexNode> shared;
        try {
            shared = std::make_shared<const DecodedJournalIndexNode>(
                decoded.move_value());
        } catch (const std::bad_alloc&) {
            return Result<
                std::shared_ptr<const DecodedJournalIndexNode>>::failure({
                ErrorDomain::internal,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"JournalIndexReader::lookup",
                L"allocating decoded node failed", 0});
        }

        context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<
                std::shared_ptr<const DecodedJournalIndexNode>>::failure(
                context_result.error());
        }
        try {
            std::lock_guard lock(mutex);
            context_result = check_operation_context(context);
            if (!context_result.ok()) {
                return Result<
                    std::shared_ptr<const DecodedJournalIndexNode>>::failure(
                    context_result.error());
            }
            const auto existing = cache.find(key);
            if (existing != cache.end()) {
                lru.splice(lru.begin(), lru,
                           existing->second.lru_iterator);
                return Result<
                    std::shared_ptr<const DecodedJournalIndexNode>>::success(
                    existing->second.node);
            }
            while (!lru.empty() &&
                   (cache.size() >= options.max_entries ||
                    stats.current_bytes + kIndexNodeBytes >
                        options.max_bytes)) {
                cache.erase(lru.back());
                lru.pop_back();
                --stats.current_entries;
                stats.current_bytes -= kIndexNodeBytes;
                ++stats.evictions;
            }
            lru.push_front(key);
            try {
                cache.emplace(key, CacheEntry{shared, lru.begin()});
            } catch (const std::bad_alloc&) {
                lru.pop_front();
                throw;
            }
            ++stats.current_entries;
            stats.current_bytes += kIndexNodeBytes;
            stats.peak_entries = (std::max)(stats.peak_entries,
                                            stats.current_entries);
            stats.peak_bytes = (std::max)(stats.peak_bytes,
                                          stats.current_bytes);
        } catch (const std::bad_alloc&) {
            return Result<
                std::shared_ptr<const DecodedJournalIndexNode>>::failure({
                ErrorDomain::internal,
                static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                L"JournalIndexReader::lookup",
                L"allocating index cache entry failed", 0});
        }
        return Result<std::shared_ptr<const DecodedJournalIndexNode>>::success(
            std::move(shared));
    }

    JournalIndexCacheOptions options{};
    mutable std::mutex mutex{};
    std::list<CacheKey> lru{};
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> cache{};
    JournalIndexCacheStats stats{};
};

JournalIndexReader::JournalIndexReader(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
JournalIndexReader::~JournalIndexReader() = default;
JournalIndexReader::JournalIndexReader(JournalIndexReader&&) noexcept = default;
JournalIndexReader& JournalIndexReader::operator=(
    JournalIndexReader&&) noexcept = default;

Result<std::unique_ptr<JournalIndexReader>> JournalIndexReader::create(
    const JournalIndexCacheOptions options) {
    if (options.max_bytes < kIndexNodeBytes || options.max_entries == 0 ||
        options.max_entries > options.max_bytes / kIndexNodeBytes) {
        return index_failure<std::unique_ptr<JournalIndexReader>>(
            L"JournalIndexReader::create", L"invalid cache limits",
            PersistenceErrorCode::InvalidFormat);
    }
    try {
        auto impl = std::make_unique<Impl>(options);
        return Result<std::unique_ptr<JournalIndexReader>>::success(
            std::unique_ptr<JournalIndexReader>(
                new JournalIndexReader(std::move(impl))));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<JournalIndexReader>>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"JournalIndexReader::create", L"allocating reader failed", 0});
    }
}

Result<JournalIndexLookupResult> JournalIndexReader::lookup(
    const OperationContext& context,
    const JournalIndexView& view,
    const std::uint64_t block_index) {
    if (!impl_) {
        return index_failure<JournalIndexLookupResult>(
            L"JournalIndexReader::lookup", L"reader is moved from",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    auto context_result = check_operation_context(context);
    if (!context_result.ok()) {
        return Result<JournalIndexLookupResult>::failure(
            context_result.error());
    }
    const auto view_validation = validate_view(view);
    if (!view_validation.ok()) {
        return Result<JournalIndexLookupResult>::failure(
            view_validation.error());
    }
    if (empty_root(view.root)) {
        return Result<JournalIndexLookupResult>::success({});
    }

    std::array<std::uint64_t, kMaxIndexDepth> visited{};
    std::size_t visited_count = 0;
    auto node_offset = view.root.offset;
    ExpectedNode expected{view.root.crc32c, view.root_generation,
                          std::nullopt, std::nullopt, std::nullopt};

    for (std::uint32_t depth = 0; depth < kMaxIndexDepth; ++depth) {
        context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<JournalIndexLookupResult>::failure(
                context_result.error());
        }
        const auto cycle = validate_visited_offset(
            std::span<const std::uint64_t>{visited}.first(visited_count),
            node_offset);
        if (!cycle.ok()) {
            return Result<JournalIndexLookupResult>::failure(cycle.error());
        }
        visited[visited_count++] = node_offset;

        auto loaded = impl_->load_node(context, view, node_offset);
        if (!loaded.ok()) {
            return Result<JournalIndexLookupResult>::failure(loaded.error());
        }
        const auto& decoded = *loaded.value();
        const auto& node = decoded.node;
        if (decoded.record_crc32c != expected.crc32c ||
            node.common.checkpoint_generation != expected.generation ||
            (expected.level.has_value() &&
             node.level != expected.level.value()) ||
            (expected.minimum_key.has_value() &&
             decoded.minimum_key != expected.minimum_key.value()) ||
            (expected.maximum_key.has_value() &&
             decoded.maximum_key != expected.maximum_key.value())) {
            return index_failure<JournalIndexLookupResult>(
                L"JournalIndexReader::lookup",
                L"index descriptor does not match decoded node");
        }

        if (block_index < decoded.minimum_key ||
            block_index > decoded.maximum_key) {
            return Result<JournalIndexLookupResult>::success({});
        }
        if (node.level == 0) {
            const auto entry = std::lower_bound(
                node.leaf_entries.begin(), node.leaf_entries.end(),
                block_index,
                [](const JournalIndexLeafEntry& candidate,
                   const std::uint64_t key) {
                    return candidate.block_index < key;
                });
            if (entry == node.leaf_entries.end() ||
                entry->block_index != block_index) {
                return Result<JournalIndexLookupResult>::success({});
            }
            const auto kind = lookup_kind(entry->record_type);
            if (!kind.has_value()) {
                return index_failure<JournalIndexLookupResult>(
                    L"JournalIndexReader::lookup",
                    L"leaf descriptor type is invalid");
            }
            return Result<JournalIndexLookupResult>::success(
                {kind.value(), *entry});
        }

        const auto child = std::find_if(
            node.internal_entries.begin(), node.internal_entries.end(),
            [block_index](const JournalIndexInternalEntry& candidate) {
                return candidate.minimum_key <= block_index &&
                       block_index <= candidate.maximum_key;
            });
        if (child == node.internal_entries.end()) {
            return Result<JournalIndexLookupResult>::success({});
        }
        const auto depth_validation = validate_descent_depth(depth, node.level);
        if (!depth_validation.ok()) {
            return Result<JournalIndexLookupResult>::failure(
                depth_validation.error());
        }
        node_offset = child->child_offset;
        expected = {child->child_crc32c, child->child_generation,
                    static_cast<std::uint16_t>(node.level - 1),
                    child->minimum_key, child->maximum_key};
    }
    return index_failure<JournalIndexLookupResult>(
        L"JournalIndexReader::lookup", L"index exceeds maximum depth");
}

Result<JournalIndexLookupResult> JournalIndexReader::lookup(
    const JournalIndexView& view,
    const std::uint64_t block_index) {
    return lookup(OperationContext{}, view, block_index);
}

JournalIndexCacheStats JournalIndexReader::cache_stats() const {
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

void JournalIndexReader::clear_cache() {
    if (!impl_) return;
    std::lock_guard lock(impl_->mutex);
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->stats.current_entries = 0;
    impl_->stats.current_bytes = 0;
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<void> JournalIndexReader::hold_cache_lock_for_test(
    std::atomic<bool>& entered,
    const std::chrono::milliseconds duration) {
    if (!impl_) {
        return index_failure<void>(
            L"JournalIndexReader::hold_cache_lock_for_test",
            L"reader is moved from", PersistenceErrorCode::FatalInternalInvariant);
    }
    std::lock_guard lock(impl_->mutex);
    entered.store(true, std::memory_order_release);
    std::this_thread::sleep_for(duration);
    return Result<void>::success();
}

namespace test {

JournalIndexGuardTestCounters journal_index_guard_test_counters() {
    return {g_visited_guard_calls.load(std::memory_order_relaxed),
            g_depth_guard_calls.load(std::memory_order_relaxed)};
}

void reset_journal_index_guard_test_counters() {
    g_visited_guard_calls.store(0, std::memory_order_relaxed);
    g_depth_guard_calls.store(0, std::memory_order_relaxed);
}

Result<void> journal_index_test_rejects_visited_offset(
    const std::span<const std::uint64_t> visited,
    const std::uint64_t candidate) {
    return validate_visited_offset(visited, candidate);
}

Result<void> journal_index_test_rejects_depth_overflow(
    const std::uint32_t depth,
    const std::uint16_t node_level) {
    return validate_descent_depth(depth, node_level);
}

}  // namespace test
#endif

}  // namespace myrd::persistence
