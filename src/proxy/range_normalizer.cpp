#include "range_normalizer.h"

#include "imdproxy_protocol.h"

#include <algorithm>
#include <cstdint>

namespace myrd::proxy {

Result<std::vector<DataRange>> decode_and_normalize_ranges(
    std::span<const std::byte> data_area,
    std::uint64_t encoded_length,
    std::uint64_t device_size,
    std::uint32_t sector_size) {

    // 1. Reject encoded_length > data_area.size()
    if (encoded_length > data_area.size()) {
        return Result<std::vector<DataRange>>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"decode_and_normalize_ranges",
            L"encoded length exceeds data area"});
    }

    // 2. Reject non-multiple of sizeof(DataSetRangeWire)
    if (encoded_length % sizeof(DataSetRangeWire) != 0) {
        return Result<std::vector<DataRange>>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"decode_and_normalize_ranges",
            L"encoded length not a multiple of range wire size"});
    }

    const std::size_t count = static_cast<std::size_t>(encoded_length)
                              / sizeof(DataSetRangeWire);

    std::vector<DataRange> ranges;
    ranges.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        // 3. Copy each wire entry with read_wire
        auto wire = read_wire<DataSetRangeWire>(
            data_area, i * sizeof(DataSetRangeWire));
        if (!wire) {
            return Result<std::vector<DataRange>>::failure(wire.error());
        }

        // 4. Ignore zero-length entries
        if (wire.value().length == 0) continue;

        // 5. Validate sector alignment
        if ((wire.value().offset % sector_size) != 0 ||
            (wire.value().length % sector_size) != 0) {
            return Result<std::vector<DataRange>>::failure({
                ErrorDomain::protocol,
                static_cast<std::uint32_t>(OperationErrorCode::misaligned),
                L"decode_and_normalize_ranges",
                L"range not sector-aligned"});
        }

        // 6. Reject offset > device_size or length > device_size - offset
        if (wire.value().offset > device_size ||
            wire.value().length > device_size - wire.value().offset) {
            return Result<std::vector<DataRange>>::failure({
                ErrorDomain::protocol,
                static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
                L"decode_and_normalize_ranges",
                L"range exceeds device size"});
        }

        ranges.push_back({wire.value().offset, wire.value().length});
    }

    if (ranges.empty()) {
        return Result<std::vector<DataRange>>::success(std::vector<DataRange>{});
    }

    // 7. Sort by (offset, length)
    std::sort(ranges.begin(), ranges.end(),
              [](const DataRange& a, const DataRange& b) {
                  if (a.offset != b.offset) return a.offset < b.offset;
                  return a.length < b.length;
              });

    // 8. Merge overlap/adjacency using subtraction-safe comparisons
    std::vector<DataRange> merged;
    merged.push_back(ranges[0]);

    for (std::size_t i = 1; i < ranges.size(); ++i) {
        DataRange& last = merged.back();
        // Check if ranges[i] overlaps with or is adjacent to last
        // last.offset + last.length >= ranges[i].offset means overlap/adjacent
        std::uint64_t last_end = last.offset + last.length;
        if (last_end >= ranges[i].offset) {
            // Merge: extend last to cover both
            std::uint64_t new_end = ranges[i].offset + ranges[i].length;
            if (new_end > last_end) {
                last.length = new_end - last.offset;
            }
        } else {
            merged.push_back(ranges[i]);
        }
    }

    return Result<std::vector<DataRange>>::success(std::move(merged));
}

} // namespace myrd::proxy
