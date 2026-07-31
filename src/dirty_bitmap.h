#pragma once
#include <cstdint>
#include <vector>
#include <mutex>
#include <functional>

class DirtyBitmap {
public:
    explicit DirtyBitmap(uint64_t totalSectors);
    void Set(uint64_t sector);
    bool Get(uint64_t sector) const;
    void Clear(uint64_t sector);
    void ClearAll();
    uint64_t CountDirty() const;
    void ForEachDirty(std::function<void(uint64_t)> callback) const;
    uint64_t TotalSectors() const { return m_totalSectors; }
private:
    uint64_t m_totalSectors;
    std::vector<uint8_t> m_bits;
    mutable std::mutex m_mutex;
    size_t ByteIndex(uint64_t s) const { return s / 8; }
    uint8_t BitMask(uint64_t s) const { return 1 << (s % 8); }
};
