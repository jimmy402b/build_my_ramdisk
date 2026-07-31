#pragma once
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <vector>

class SectorCache {
public:
    static const uint32_t SECTOR_SIZE = 512;
    void Put(uint64_t sector, const void* data);
    const uint8_t* Get(uint64_t sector) const;
    bool Has(uint64_t sector) const;
    void Evict(uint64_t sector);
    size_t Size() const;
    uint8_t* GetForWrite(uint64_t sector);
private:
    std::unordered_map<uint64_t, std::vector<uint8_t>> m_cache;
    mutable std::mutex m_mutex;
};
