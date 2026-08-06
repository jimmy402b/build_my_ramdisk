#include "src/persistence/journal_index.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <list>
#include <limits>
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

Result<std::optional<std::uint64_t>> JournalIndexReader::next_block_index(
    const OperationContext& context,
    const JournalIndexView& view,
    const std::uint64_t first_block) {
    constexpr auto operation = L"JournalIndexReader::next_block_index";
    if (!impl_) {
        return index_failure<std::optional<std::uint64_t>>(
            operation, L"reader is moved from",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    auto context_result = check_operation_context(context);
    if (!context_result.ok()) {
        return Result<std::optional<std::uint64_t>>::failure(
            context_result.error());
    }
    const auto view_validation = validate_view(view);
    if (!view_validation.ok()) {
        return Result<std::optional<std::uint64_t>>::failure(
            view_validation.error());
    }
    if (empty_root(view.root)) {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }

    std::array<std::uint64_t, kMaxIndexDepth> visited{};
    std::size_t visited_count = 0;
    auto node_offset = view.root.offset;
    ExpectedNode expected{view.root.crc32c, view.root_generation,
                          std::nullopt, std::nullopt, std::nullopt};

    for (std::uint32_t depth = 0; depth < kMaxIndexDepth; ++depth) {
        context_result = check_operation_context(context);
        if (!context_result.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                context_result.error());
        }
        const auto cycle = validate_visited_offset(
            std::span<const std::uint64_t>{visited}.first(visited_count),
            node_offset);
        if (!cycle.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                cycle.error());
        }
        visited[visited_count++] = node_offset;

        auto loaded = impl_->load_node(context, view, node_offset);
        if (!loaded.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                loaded.error());
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
            return index_failure<std::optional<std::uint64_t>>(
                operation, L"index descriptor does not match decoded node");
        }
        if (first_block > decoded.maximum_key) {
            return Result<std::optional<std::uint64_t>>::success(
                std::nullopt);
        }
        if (node.level == 0) {
            const auto entry = std::lower_bound(
                node.leaf_entries.begin(), node.leaf_entries.end(),
                first_block,
                [](const JournalIndexLeafEntry& candidate,
                   const std::uint64_t key) {
                    return candidate.block_index < key;
                });
            if (entry == node.leaf_entries.end()) {
                return Result<std::optional<std::uint64_t>>::success(
                    std::nullopt);
            }
            if (!lookup_kind(entry->record_type).has_value()) {
                return index_failure<std::optional<std::uint64_t>>(
                    operation, L"leaf descriptor type is invalid");
            }
            return Result<std::optional<std::uint64_t>>::success(
                entry->block_index);
        }

        const auto child = std::find_if(
            node.internal_entries.begin(), node.internal_entries.end(),
            [first_block](const JournalIndexInternalEntry& candidate) {
                return candidate.maximum_key >= first_block;
            });
        if (child == node.internal_entries.end()) {
            return Result<std::optional<std::uint64_t>>::success(
                std::nullopt);
        }
        const auto depth_validation = validate_descent_depth(depth, node.level);
        if (!depth_validation.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                depth_validation.error());
        }
        node_offset = child->child_offset;
        expected = {child->child_crc32c, child->child_generation,
                    static_cast<std::uint16_t>(node.level - 1),
                    child->minimum_key, child->maximum_key};
    }
    return index_failure<std::optional<std::uint64_t>>(
        operation, L"index exceeds maximum depth");
}

Result<DecodedJournalIndexNode> JournalIndexReader::read_node(
    const OperationContext& context,
    const JournalIndexView& view,
    const std::uint64_t node_offset,
    const std::optional<std::uint32_t> expected_crc32c) {
    if (!impl_) {
        return index_failure<DecodedJournalIndexNode>(
            L"JournalIndexReader::read_node", L"reader is moved from",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    const auto view_validation = validate_view(view);
    if (!view_validation.ok()) {
        return Result<DecodedJournalIndexNode>::failure(
            view_validation.error());
    }
    auto loaded = impl_->load_node(context, view, node_offset);
    if (!loaded.ok()) {
        return Result<DecodedJournalIndexNode>::failure(loaded.error());
    }
    if (expected_crc32c.has_value() &&
        loaded.value()->record_crc32c != expected_crc32c.value()) {
        return index_failure<DecodedJournalIndexNode>(
            L"JournalIndexReader::read_node",
            L"node CRC does not match descriptor");
    }
    return Result<DecodedJournalIndexNode>::success(*loaded.value());
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

struct JournalIndexBuilder::Impl final {
    Impl(JournalIndexBuilderOptions source_options,
         std::unique_ptr<JournalIndexReader> source_reader)
        : options(std::move(source_options)), reader(std::move(source_reader)) {}
    JournalIndexBuilderOptions options{};
    std::unique_ptr<JournalIndexReader> reader{};
};

namespace {

Result<void> validate_builder_context(const OperationContext& context,
                                      std::wstring_view operation) {
    const auto checked = check_operation_context(context);
    if (checked.ok()) return Result<void>::success();
    auto error = checked.error();
    error.operation = std::wstring{operation};
    return Result<void>::failure(std::move(error));
}

bool valid_update_descriptor(const JournalIndexLeafEntry& entry,
                             const CheckpointGeneration generation) {
    const bool type_ok = entry.record_type == JournalRecordType::Data ||
                         entry.record_type == JournalRecordType::Zero ||
                         entry.record_type == JournalRecordType::Discard;
    const auto expected_length = entry.record_type == JournalRecordType::Data
        ? kDataRecordBytes : kFixedRecordBytes;
    return type_ok && entry.record_generation == generation &&
           entry.record_offset != 0 &&
           entry.record_offset % kJournalAlignment == 0 &&
           entry.record_length == expected_length &&
           entry.record_offset <=
               (std::numeric_limits<std::uint64_t>::max)() -
                   entry.record_length &&
           (entry.record_type == JournalRecordType::Data ||
            entry.payload_crc32c == 0);
}

JournalIndexPlanChild physical_child(
    const JournalIndexInternalEntry& entry,
    const std::uint16_t level) {
    return {false, 0, level, entry.minimum_key, entry.maximum_key,
            entry.child_offset, entry.child_length, entry.child_crc32c,
            entry.child_generation};
}

}  // namespace

class JournalIndexPlanBuilderDetail final {
public:
    JournalIndexPlanBuilderDetail(
        JournalIndexReader& source_reader,
        const JournalIndexBuilderOptions& source_options,
        const OperationContext& source_context,
        const JournalIndexView& source_view,
        const CheckpointGeneration generation,
        const std::uint64_t operation_id)
        : reader_(source_reader), options_(source_options), context_(source_context),
          view_(source_view) {
        plan_.generation = generation;
        plan_.operation_id = operation_id;
        plan_.base = {source_view.file_identity, source_view.journal_identity,
                      source_view.epoch, source_view.root_generation,
                      source_view.committed_length, source_view.root};
    }

    Result<JournalIndexBuildPlan> build(
        std::span<const JournalIndexLeafEntry> source_updates) {
        try {
            updates_.assign(source_updates.begin(), source_updates.end());
            std::sort(updates_.begin(), updates_.end(),
                      [](const auto& left, const auto& right) {
                          return left.block_index < right.block_index;
                      });
        } catch (const std::bad_alloc&) {
            return allocation_failure<JournalIndexBuildPlan>(
                L"JournalIndexBuilder::plan");
        }
        for (std::size_t index = 0; index < updates_.size(); ++index) {
            if (!valid_update_descriptor(updates_[index], plan_.generation) ||
                updates_[index].record_offset < view_.committed_length) {
                return index_failure<JournalIndexBuildPlan>(
                    L"JournalIndexBuilder::plan",
                    L"update descriptor is invalid",
                    PersistenceErrorCode::InvalidFormat);
            }
            if (index != 0 &&
                updates_[index - 1].block_index == updates_[index].block_index) {
                return index_failure<JournalIndexBuildPlan>(
                    L"JournalIndexBuilder::plan",
                    L"duplicate block update in checkpoint",
                    PersistenceErrorCode::InvalidFormat);
            }
        }
        try {
            std::vector<const JournalIndexLeafEntry*> by_offset;
            by_offset.reserve(updates_.size());
            for (const auto& update : updates_) by_offset.push_back(&update);
            std::sort(by_offset.begin(), by_offset.end(),
                      [](const auto* left, const auto* right) {
                          return left->record_offset < right->record_offset;
                      });
            std::uint64_t previous_end = view_.committed_length;
            for (const auto* update : by_offset) {
                if (update->record_offset < previous_end) {
                    return index_failure<JournalIndexBuildPlan>(
                        L"JournalIndexBuilder::plan",
                        L"checkpoint record ranges overlap",
                        PersistenceErrorCode::InvalidFormat);
                }
                previous_end = update->record_offset + update->record_length;
            }
        } catch (const std::bad_alloc&) {
            return allocation_failure<JournalIndexBuildPlan>(
                L"JournalIndexBuilder::plan");
        }

        auto context = validate_builder_context(
            context_, L"JournalIndexBuilder::plan");
        if (!context.ok()) {
            return Result<JournalIndexBuildPlan>::failure(context.error());
        }

        // A clean checkpoint over an empty image needs no index node. For a
        // non-empty image, re-home the root in the new generation while its
        // unchanged children/leaf records continue to reference older,
        // already committed records.
        if (updates_.empty()) {
            if (empty_root(view_.root)) {
                return Result<JournalIndexBuildPlan>::success(
                    std::move(plan_));
            }
            auto decoded = reader_.read_node(
                context_, view_, view_.root.offset, view_.root.crc32c);
            if (!decoded.ok()) {
                return Result<JournalIndexBuildPlan>::failure(
                    decoded.error());
            }
            if (decoded.value().node.common.checkpoint_generation !=
                view_.root_generation) {
                return index_failure<JournalIndexBuildPlan>(
                    L"JournalIndexBuilder::plan",
                    L"root generation does not match immutable view");
            }
            JournalIndexPlanNode cloned{};
            cloned.level = decoded.value().node.level;
            try {
                if (cloned.level == 0) {
                    cloned.leaf_entries =
                        decoded.value().node.leaf_entries;
                } else {
                    cloned.children.reserve(
                        decoded.value().node.internal_entries.size());
                    for (const auto& child :
                         decoded.value().node.internal_entries) {
                        cloned.children.push_back(physical_child(
                            child,
                            static_cast<std::uint16_t>(cloned.level - 1)));
                    }
                }
            } catch (const std::bad_alloc&) {
                return allocation_failure<JournalIndexBuildPlan>(
                    L"JournalIndexBuilder::plan");
            }
            auto cloned_root = add_node(std::move(cloned));
            if (!cloned_root.ok()) {
                return Result<JournalIndexBuildPlan>::failure(
                    cloned_root.error());
            }
            plan_.root = cloned_root.value();
            return Result<JournalIndexBuildPlan>::success(std::move(plan_));
        }

        std::vector<JournalIndexPlanChild> roots;
        if (empty_root(view_.root)) {
            auto leaves = make_leaf_nodes(updates_);
            if (!leaves.ok()) {
                return Result<JournalIndexBuildPlan>::failure(leaves.error());
            }
            roots = leaves.move_value();
        } else {
            auto decoded = reader_.read_node(context_, view_, view_.root.offset,
                                             view_.root.crc32c);
            if (!decoded.ok()) {
                return Result<JournalIndexBuildPlan>::failure(decoded.error());
            }
            if (decoded.value().node.common.checkpoint_generation !=
                    view_.root_generation) {
                return index_failure<JournalIndexBuildPlan>(
                    L"JournalIndexBuilder::plan",
                    L"root generation does not match immutable view");
            }
            JournalIndexPlanChild old_root{
                false, 0, decoded.value().node.level,
                decoded.value().minimum_key, decoded.value().maximum_key,
                view_.root.offset, view_.root.length, view_.root.crc32c,
                view_.root_generation};
            auto changed = update_node(old_root, updates_);
            if (!changed.ok()) {
                return Result<JournalIndexBuildPlan>::failure(changed.error());
            }
            roots = changed.move_value();
        }

        auto root = build_upper(std::move(roots));
        if (!root.ok()) {
            return Result<JournalIndexBuildPlan>::failure(root.error());
        }
        plan_.root = root.value();
        return Result<JournalIndexBuildPlan>::success(std::move(plan_));
    }

private:
    template <typename T>
    Result<T> allocation_failure(std::wstring_view operation) const {
        return Result<T>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            std::wstring{operation}, L"allocating index plan failed", 0});
    }

    Result<JournalIndexPlanChild> add_node(JournalIndexPlanNode node) {
        if (plan_.nodes.size() >= options_.max_new_nodes ||
            plan_.nodes.size() >=
                options_.max_plan_image_bytes / kIndexNodeBytes ||
            plan_.nodes.size() >=
                (std::numeric_limits<std::uint32_t>::max)()) {
            return index_failure<JournalIndexPlanChild>(
                L"JournalIndexBuilder::plan", L"index plan limit exceeded",
                PersistenceErrorCode::SnapshotResourceExhausted);
        }
        JournalIndexPlanChild result{};
        result.planned = true;
        result.plan_index = static_cast<std::uint32_t>(plan_.nodes.size());
        result.level = node.level;
        if (node.level == 0) {
            result.minimum_key = node.leaf_entries.front().block_index;
            result.maximum_key = node.leaf_entries.back().block_index;
        } else {
            result.minimum_key = node.children.front().minimum_key;
            result.maximum_key = node.children.back().maximum_key;
        }
        result.length = kIndexNodeBytes;
        result.generation = plan_.generation;
        try {
            plan_.nodes.push_back(std::move(node));
        } catch (const std::bad_alloc&) {
            return allocation_failure<JournalIndexPlanChild>(
                L"JournalIndexBuilder::plan");
        }
        return Result<JournalIndexPlanChild>::success(result);
    }

    Result<std::vector<JournalIndexPlanChild>> make_leaf_nodes(
        std::span<const JournalIndexLeafEntry> entries) {
        std::vector<JournalIndexPlanChild> output;
        try {
            output.reserve((entries.size() + kJournalIndexMaxEntries - 1) /
                           kJournalIndexMaxEntries);
            for (std::size_t begin = 0; begin < entries.size();
                 begin += kJournalIndexMaxEntries) {
                const auto count = (std::min)(
                    static_cast<std::size_t>(kJournalIndexMaxEntries),
                    entries.size() - begin);
                JournalIndexPlanNode node{};
                node.level = 0;
                node.leaf_entries.assign(entries.begin() + begin,
                                         entries.begin() + begin + count);
                auto added = add_node(std::move(node));
                if (!added.ok()) {
                    return Result<std::vector<JournalIndexPlanChild>>::failure(
                        added.error());
                }
                output.push_back(added.value());
            }
        } catch (const std::bad_alloc&) {
            return allocation_failure<std::vector<JournalIndexPlanChild>>(
                L"JournalIndexBuilder::plan");
        }
        return Result<std::vector<JournalIndexPlanChild>>::success(
            std::move(output));
    }

    Result<std::vector<JournalIndexPlanChild>> make_internal_nodes(
        const std::uint16_t level,
        std::span<const JournalIndexPlanChild> children) {
        if (level == 0 || level >= kMaxIndexDepth) {
            return index_failure<std::vector<JournalIndexPlanChild>>(
                L"JournalIndexBuilder::plan", L"index depth limit exceeded",
                PersistenceErrorCode::SnapshotResourceExhausted);
        }
        std::vector<JournalIndexPlanChild> output;
        try {
            output.reserve((children.size() + kJournalIndexMaxEntries - 1) /
                           kJournalIndexMaxEntries);
            for (std::size_t begin = 0; begin < children.size();
                 begin += kJournalIndexMaxEntries) {
                const auto count = (std::min)(
                    static_cast<std::size_t>(kJournalIndexMaxEntries),
                    children.size() - begin);
                JournalIndexPlanNode node{};
                node.level = level;
                node.children.assign(children.begin() + begin,
                                     children.begin() + begin + count);
                auto added = add_node(std::move(node));
                if (!added.ok()) {
                    return Result<std::vector<JournalIndexPlanChild>>::failure(
                        added.error());
                }
                output.push_back(added.value());
            }
        } catch (const std::bad_alloc&) {
            return allocation_failure<std::vector<JournalIndexPlanChild>>(
                L"JournalIndexBuilder::plan");
        }
        return Result<std::vector<JournalIndexPlanChild>>::success(
            std::move(output));
    }

    Result<std::vector<JournalIndexPlanChild>> update_node(
        const JournalIndexPlanChild& old,
        std::span<const JournalIndexLeafEntry> updates) {
        auto context = validate_builder_context(
            context_, L"JournalIndexBuilder::plan");
        if (!context.ok()) {
            return Result<std::vector<JournalIndexPlanChild>>::failure(
                context.error());
        }
        auto decoded = reader_.read_node(context_, view_, old.offset,
                                         old.crc32c);
        if (!decoded.ok()) {
            return Result<std::vector<JournalIndexPlanChild>>::failure(
                decoded.error());
        }
        const auto& source = decoded.value();
        if (source.node.level != old.level ||
            source.node.common.checkpoint_generation != old.generation ||
            source.minimum_key != old.minimum_key ||
            source.maximum_key != old.maximum_key) {
            return index_failure<std::vector<JournalIndexPlanChild>>(
                L"JournalIndexBuilder::plan",
                L"old child descriptor does not match node");
        }
        if (old.level == 0) return update_leaf(old, source, updates);
        return update_internal(old, source, updates);
    }

    Result<std::vector<JournalIndexPlanChild>> update_leaf(
        const JournalIndexPlanChild& old,
        const DecodedJournalIndexNode& source,
        std::span<const JournalIndexLeafEntry> updates) {
        std::vector<JournalIndexLeafEntry> merged;
        try {
            merged.reserve(source.node.leaf_entries.size() + updates.size());
            std::size_t left = 0;
            std::size_t right = 0;
            while (left < source.node.leaf_entries.size() ||
                   right < updates.size()) {
                if (right == updates.size() ||
                    (left < source.node.leaf_entries.size() &&
                     source.node.leaf_entries[left].block_index <
                         updates[right].block_index)) {
                    merged.push_back(source.node.leaf_entries[left++]);
                } else if (left == source.node.leaf_entries.size() ||
                           updates[right].block_index <
                               source.node.leaf_entries[left].block_index) {
                    merged.push_back(updates[right++]);
                } else {
                    merged.push_back(updates[right++]);
                    ++left;
                }
            }
        } catch (const std::bad_alloc&) {
            return allocation_failure<std::vector<JournalIndexPlanChild>>(
                L"JournalIndexBuilder::plan");
        }
        if (merged == source.node.leaf_entries) {
            return Result<std::vector<JournalIndexPlanChild>>::success({old});
        }
        return make_leaf_nodes(merged);
    }

    Result<std::vector<JournalIndexPlanChild>> update_internal(
        const JournalIndexPlanChild& old,
        const DecodedJournalIndexNode& source,
        std::span<const JournalIndexLeafEntry> updates) {
        const auto& entries = source.node.internal_entries;
        std::vector<std::vector<JournalIndexLeafEntry>> groups;
        try {
            groups.resize(entries.size());
            for (const auto& update : updates) {
                const auto next = std::upper_bound(
                    entries.begin(), entries.end(), update.block_index,
                    [](const std::uint64_t key,
                       const JournalIndexInternalEntry& entry) {
                        return key < entry.minimum_key;
                    });
                const auto target = next == entries.begin()
                    ? std::size_t{0}
                    : static_cast<std::size_t>((next - entries.begin()) - 1);
                groups[target].push_back(update);
            }
        } catch (const std::bad_alloc&) {
            return allocation_failure<std::vector<JournalIndexPlanChild>>(
                L"JournalIndexBuilder::plan");
        }

        std::vector<JournalIndexPlanChild> children;
        bool changed = false;
        try {
            children.reserve(entries.size() + updates.size());
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto child = physical_child(
                    entries[index], static_cast<std::uint16_t>(old.level - 1));
                if (groups[index].empty()) {
                    children.push_back(child);
                    continue;
                }
                auto replacements = update_node(child, groups[index]);
                if (!replacements.ok()) {
                    return Result<std::vector<JournalIndexPlanChild>>::failure(
                        replacements.error());
                }
                if (replacements.value().size() != 1 ||
                    replacements.value().front() != child) {
                    changed = true;
                }
                children.insert(children.end(), replacements.value().begin(),
                                replacements.value().end());
            }
        } catch (const std::bad_alloc&) {
            return allocation_failure<std::vector<JournalIndexPlanChild>>(
                L"JournalIndexBuilder::plan");
        }
        if (!changed) {
            return Result<std::vector<JournalIndexPlanChild>>::success({old});
        }
        return make_internal_nodes(old.level, children);
    }

    Result<JournalIndexPlanChild> build_upper(
        std::vector<JournalIndexPlanChild> current) {
        if (current.empty()) {
            return index_failure<JournalIndexPlanChild>(
                L"JournalIndexBuilder::plan", L"index plan has no root",
                PersistenceErrorCode::FatalInternalInvariant);
        }
        while (current.size() > 1) {
            const auto context = validate_builder_context(
                context_, L"JournalIndexBuilder::plan");
            if (!context.ok()) {
                return Result<JournalIndexPlanChild>::failure(context.error());
            }
            const auto level = static_cast<std::uint16_t>(
                current.front().level + 1);
            for (const auto& child : current) {
                if (child.level + 1 != level) {
                    return index_failure<JournalIndexPlanChild>(
                        L"JournalIndexBuilder::plan",
                        L"index plan child levels are inconsistent",
                        PersistenceErrorCode::FatalInternalInvariant);
                }
            }
            auto parents = make_internal_nodes(level, current);
            if (!parents.ok()) {
                return Result<JournalIndexPlanChild>::failure(parents.error());
            }
            current = parents.move_value();
        }
        return Result<JournalIndexPlanChild>::success(current.front());
    }

    JournalIndexReader& reader_;
    const JournalIndexBuilderOptions& options_;
    const OperationContext& context_;
    const JournalIndexView& view_;
    JournalIndexBuildPlan plan_{};
    std::vector<JournalIndexLeafEntry> updates_{};
};

namespace {

std::uint32_t encoded_record_crc(
    const std::array<std::byte, kIndexNodeBytes>& bytes) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[32 + index]))
                 << (index * 8U);
    }
    return value;
}

std::pair<std::uint64_t, std::uint64_t> plan_node_bounds(
    const JournalIndexPlanNode& node) {
    if (node.level == 0 && !node.leaf_entries.empty()) {
        return {node.leaf_entries.front().block_index,
                node.leaf_entries.back().block_index};
    }
    if (node.level != 0 && !node.children.empty()) {
        return {node.children.front().minimum_key,
                node.children.back().maximum_key};
    }
    return {};
}

}  // namespace

JournalIndexBuilder::JournalIndexBuilder(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
JournalIndexBuilder::~JournalIndexBuilder() = default;
JournalIndexBuilder::JournalIndexBuilder(JournalIndexBuilder&&) noexcept =
    default;
JournalIndexBuilder& JournalIndexBuilder::operator=(
    JournalIndexBuilder&&) noexcept = default;

Result<std::unique_ptr<JournalIndexBuilder>> JournalIndexBuilder::create(
    const JournalIndexBuilderOptions options) {
    if (options.max_new_nodes == 0 || options.max_plan_image_bytes == 0 ||
        options.max_new_nodes ==
            (std::numeric_limits<std::uint64_t>::max)() ||
        options.max_plan_image_bytes ==
            (std::numeric_limits<std::uint64_t>::max)() ||
        options.max_new_nodes >
            options.max_plan_image_bytes / kIndexNodeBytes) {
        return index_failure<std::unique_ptr<JournalIndexBuilder>>(
            L"JournalIndexBuilder::create", L"invalid builder limits",
            PersistenceErrorCode::InvalidFormat);
    }
    try {
        auto reader = JournalIndexReader::create(options.reader_cache);
        if (!reader.ok()) {
            return Result<std::unique_ptr<JournalIndexBuilder>>::failure(
                reader.error());
        }
        auto impl = std::make_unique<Impl>(options, reader.move_value());
        return Result<std::unique_ptr<JournalIndexBuilder>>::success(
            std::unique_ptr<JournalIndexBuilder>(
                new JournalIndexBuilder(std::move(impl))));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<JournalIndexBuilder>>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"JournalIndexBuilder::create", L"allocating builder failed", 0});
    }
}

Result<JournalIndexBuildPlan> JournalIndexBuilder::plan(
    const OperationContext& context,
    const JournalIndexView& base_view,
    const std::span<const JournalIndexLeafEntry> updates,
    const CheckpointGeneration new_generation,
    const std::uint64_t operation_id) {
    if (!impl_) {
        return index_failure<JournalIndexBuildPlan>(
            L"JournalIndexBuilder::plan", L"builder is moved from",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    const auto view = validate_view(base_view);
    if (!view.ok()) {
        return Result<JournalIndexBuildPlan>::failure(view.error());
    }
    auto expected_generation = base_view.root_generation;
    if (!checked_increment(expected_generation) ||
        new_generation != expected_generation || operation_id == 0) {
        return index_failure<JournalIndexBuildPlan>(
            L"JournalIndexBuilder::plan",
            L"new checkpoint identity is invalid",
            PersistenceErrorCode::InvalidFormat);
    }
    JournalIndexPlanBuilderDetail builder(
        *impl_->reader, impl_->options, context, base_view,
        new_generation, operation_id);
    return builder.build(updates);
}

Result<JournalIndexAssignedPlan> JournalIndexBuilder::assign_offsets(
    const JournalIndexBuildPlan& plan,
    const JournalIndexView& expected_base_view,
    const std::uint64_t first_offset,
    const std::uint64_t first_sequence,
    const std::uint64_t committed_bound) {
    if (!impl_) {
        return index_failure<JournalIndexAssignedPlan>(
            L"JournalIndexBuilder::assign_offsets", L"builder is moved from",
            PersistenceErrorCode::FatalInternalInvariant);
    }
    const auto expected_view_validation = validate_view(expected_base_view);
    if (!expected_view_validation.ok()) {
        return Result<JournalIndexAssignedPlan>::failure(
            expected_view_validation.error());
    }
    const JournalIndexPlanBaseIdentity expected_base{
        expected_base_view.file_identity,
        expected_base_view.journal_identity,
        expected_base_view.epoch,
        expected_base_view.root_generation,
        expected_base_view.committed_length,
        expected_base_view.root};
    if (plan.base != expected_base) {
        return index_failure<JournalIndexAssignedPlan>(
            L"JournalIndexBuilder::assign_offsets",
            L"plan base identity does not match expected immutable view",
            PersistenceErrorCode::GenerationConflict);
    }
    auto assigned_generation = plan.base.root_generation;
    const bool generation_is_next = checked_increment(assigned_generation) &&
                                    assigned_generation == plan.generation;
    if (plan.generation == 0 || plan.generation == kInvalidGeneration ||
        plan.operation_id == 0 ||
        all_zero(plan.base.file_identity.file_id) ||
        all_zero(plan.base.journal_identity.journal_uuid) ||
        plan.base.epoch == 0 || plan.base.epoch == kInvalidGeneration ||
        plan.base.root_generation == kInvalidGeneration ||
        !generation_is_next ||
        plan.base.committed_length < kJournalAlignment ||
        plan.base.committed_length % kJournalAlignment != 0 ||
        plan.nodes.size() > impl_->options.max_new_nodes ||
        plan.nodes.size() >
            impl_->options.max_plan_image_bytes / kIndexNodeBytes ||
        first_offset == 0 || first_offset % kJournalAlignment != 0 ||
        first_offset < plan.base.committed_length ||
        committed_bound % kJournalAlignment != 0 ||
        first_offset > committed_bound ||
        plan.nodes.size() >
            (committed_bound - first_offset) / kIndexNodeBytes ||
        (!plan.nodes.empty() && plan.nodes.size() - 1 >
            (std::numeric_limits<std::uint64_t>::max)() - first_sequence)) {
        return index_failure<JournalIndexAssignedPlan>(
            L"JournalIndexBuilder::assign_offsets",
            L"offset assignment input is invalid",
            PersistenceErrorCode::InvalidFormat);
    }
    if (!empty_root(plan.base.root) &&
        (plan.base.root_generation == 0 || plan.base.root.offset == 0 ||
         plan.base.root.length != kIndexNodeBytes ||
         plan.base.root.offset % kJournalAlignment != 0 ||
         plan.base.root.offset > plan.base.committed_length ||
         plan.base.root.length >
             plan.base.committed_length - plan.base.root.offset)) {
        return index_failure<JournalIndexAssignedPlan>(
            L"JournalIndexBuilder::assign_offsets",
            L"base root identity is invalid",
            PersistenceErrorCode::InvalidFormat);
    }
    if (plan.nodes.empty()) {
        if (!empty_root(plan.base.root)) {
            return index_failure<JournalIndexAssignedPlan>(
                L"JournalIndexBuilder::assign_offsets",
                L"non-empty root lacks a replacement node",
                PersistenceErrorCode::FatalInternalInvariant);
        }
        JournalIndexAssignedPlan assigned{};
        assigned.root = plan.base.root;
        assigned.next_offset = first_offset;
        return Result<JournalIndexAssignedPlan>::success(
            std::move(assigned));
    }

    for (const auto& node : plan.nodes) {
        for (const auto& entry : node.leaf_entries) {
            if (entry.record_offset >
                (std::numeric_limits<std::uint64_t>::max)() -
                    entry.record_length) {
                return index_failure<JournalIndexAssignedPlan>(
                    L"JournalIndexBuilder::assign_offsets",
                    L"leaf record range overflows",
                    PersistenceErrorCode::InvalidFormat);
            }
            const auto end = entry.record_offset + entry.record_length;
            const bool new_record = entry.record_generation == plan.generation;
            if ((new_record &&
                 (entry.record_offset < plan.base.committed_length ||
                  end > first_offset)) ||
                (!new_record &&
                 (entry.record_generation > plan.base.root_generation ||
                  end > plan.base.committed_length))) {
                return index_failure<JournalIndexAssignedPlan>(
                    L"JournalIndexBuilder::assign_offsets",
                    L"leaf record provenance is outside its committed range",
                    PersistenceErrorCode::InvalidFormat);
            }
        }
    }

    try {
        if (!plan.root.planned ||
            plan.root.plan_index != plan.nodes.size() - 1) {
            return index_failure<JournalIndexAssignedPlan>(
                L"JournalIndexBuilder::assign_offsets",
                L"plan root is not canonical",
                PersistenceErrorCode::InvalidFormat);
        }
        std::vector<std::uint8_t> reached(plan.nodes.size(), 0);
        std::vector<std::uint32_t> pending{plan.root.plan_index};
        std::size_t reached_count = 0;
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (index >= plan.nodes.size() || reached[index] != 0) {
                return index_failure<JournalIndexAssignedPlan>(
                    L"JournalIndexBuilder::assign_offsets",
                    L"plan graph is not a canonical tree",
                    PersistenceErrorCode::InvalidFormat);
            }
            reached[index] = 1;
            ++reached_count;
            for (const auto& child : plan.nodes[index].children) {
                if (!child.planned) continue;
                if (child.plan_index >= index) {
                    return index_failure<JournalIndexAssignedPlan>(
                        L"JournalIndexBuilder::assign_offsets",
                        L"plan graph is not strict postorder",
                        PersistenceErrorCode::InvalidFormat);
                }
                pending.push_back(child.plan_index);
            }
        }
        if (reached_count != plan.nodes.size()) {
            return index_failure<JournalIndexAssignedPlan>(
                L"JournalIndexBuilder::assign_offsets",
                L"plan contains unreachable nodes",
                PersistenceErrorCode::InvalidFormat);
        }
    } catch (const std::bad_alloc&) {
        return Result<JournalIndexAssignedPlan>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"JournalIndexBuilder::assign_offsets",
            L"allocating plan validation state failed", 0});
    }

    JournalIndexAssignedPlan assigned{};
    try {
        assigned.nodes.reserve(plan.nodes.size());
        std::uint64_t offset = first_offset;
        for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
            const auto& planned = plan.nodes[index];
            const auto count = planned.level == 0
                ? planned.leaf_entries.size() : planned.children.size();
            if (planned.level >= kMaxIndexDepth || count == 0 ||
                count > kJournalIndexMaxEntries) {
                return index_failure<JournalIndexAssignedPlan>(
                    L"JournalIndexBuilder::assign_offsets",
                    L"planned node shape is invalid",
                    PersistenceErrorCode::InvalidFormat);
            }
            JournalIndexNode node{};
            node.common = {JournalRecordType::IndexNode, plan.generation,
                           plan.operation_id, first_sequence + index, 0};
            node.level = planned.level;
            if (planned.level == 0) {
                if (planned.leaf_entries.empty() ||
                    !planned.children.empty()) {
                    return index_failure<JournalIndexAssignedPlan>(
                        L"JournalIndexBuilder::assign_offsets",
                        L"leaf plan is invalid",
                        PersistenceErrorCode::InvalidFormat);
                }
                node.leaf_entries = planned.leaf_entries;
            } else {
                if (planned.children.empty() ||
                    !planned.leaf_entries.empty()) {
                    return index_failure<JournalIndexAssignedPlan>(
                        L"JournalIndexBuilder::assign_offsets",
                        L"internal plan is invalid",
                        PersistenceErrorCode::InvalidFormat);
                }
                node.internal_entries.reserve(planned.children.size());
                for (const auto& child : planned.children) {
                    JournalIndexInternalEntry entry{};
                    entry.minimum_key = child.minimum_key;
                    entry.maximum_key = child.maximum_key;
                    entry.child_length = kIndexNodeBytes;
                    if (child.planned) {
                        if (child.plan_index >= index ||
                            child.plan_index >= assigned.nodes.size() ||
                            child.level + 1 != planned.level ||
                            child.offset != 0 || child.crc32c != 0) {
                            return index_failure<JournalIndexAssignedPlan>(
                                L"JournalIndexBuilder::assign_offsets",
                                L"logical child reference is invalid",
                                PersistenceErrorCode::InvalidFormat);
                        }
                        const auto& physical = assigned.nodes[child.plan_index];
                        const auto& child_plan = plan.nodes[child.plan_index];
                        const auto bounds = plan_node_bounds(child_plan);
                        if (child.level != child_plan.level ||
                            child.minimum_key != bounds.first ||
                            child.maximum_key != bounds.second ||
                            child.length != kIndexNodeBytes ||
                            child.generation != plan.generation) {
                            return index_failure<JournalIndexAssignedPlan>(
                                L"JournalIndexBuilder::assign_offsets",
                                L"logical child metadata is invalid",
                                PersistenceErrorCode::InvalidFormat);
                        }
                        entry.child_offset = physical.offset;
                        entry.child_crc32c = physical.crc32c;
                        entry.child_generation = plan.generation;
                    } else {
                        if (child.plan_index != 0 ||
                            child.level + 1 != planned.level ||
                            child.offset == 0 ||
                            child.length != kIndexNodeBytes ||
                            child.generation == 0 ||
                            child.generation > plan.base.root_generation ||
                            child.offset % kJournalAlignment != 0 ||
                            child.offset > plan.base.committed_length ||
                            child.length >
                                plan.base.committed_length - child.offset) {
                            return index_failure<JournalIndexAssignedPlan>(
                                L"JournalIndexBuilder::assign_offsets",
                                L"physical child reference is invalid",
                                PersistenceErrorCode::InvalidFormat);
                        }
                        entry.child_offset = child.offset;
                        entry.child_crc32c = child.crc32c;
                        entry.child_generation = child.generation;
                    }
                    node.internal_entries.push_back(entry);
                }
            }
            auto encoded = encode_index_node(node, offset, committed_bound);
            if (!encoded.ok()) {
                return Result<JournalIndexAssignedPlan>::failure(
                    encoded.error());
            }
            assigned.nodes.push_back(
                {offset, encoded_record_crc(encoded.value()), planned.level,
                 encoded.move_value()});
            offset += kIndexNodeBytes;
        }
        if (!plan.root.planned ||
            plan.root.plan_index >= assigned.nodes.size() ||
            plan.root.plan_index != plan.nodes.size() - 1 ||
            assigned.nodes[plan.root.plan_index].level != plan.root.level ||
            plan.root.length != kIndexNodeBytes ||
            plan.root.generation != plan.generation ||
            plan.root.offset != 0 || plan.root.crc32c != 0) {
            return index_failure<JournalIndexAssignedPlan>(
                L"JournalIndexBuilder::assign_offsets",
                L"planned root reference is invalid",
                PersistenceErrorCode::InvalidFormat);
        }
        const auto root_bounds = plan_node_bounds(plan.nodes.back());
        if (plan.root.minimum_key != root_bounds.first ||
            plan.root.maximum_key != root_bounds.second) {
            return index_failure<JournalIndexAssignedPlan>(
                L"JournalIndexBuilder::assign_offsets",
                L"planned root metadata is invalid",
                PersistenceErrorCode::InvalidFormat);
        }
        const auto& root = assigned.nodes[plan.root.plan_index];
        assigned.root = {root.offset, kIndexNodeBytes, root.crc32c};
        assigned.next_offset = offset;
    } catch (const std::bad_alloc&) {
        return Result<JournalIndexAssignedPlan>::failure({
            ErrorDomain::internal,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"JournalIndexBuilder::assign_offsets",
            L"allocating assigned index nodes failed", 0});
    }
    return Result<JournalIndexAssignedPlan>::success(std::move(assigned));
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
