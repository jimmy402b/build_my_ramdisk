#include "sector_cache.h"
#include <cstring>

void SectorCache::Put(uint64_t sector, const void* data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& buf = m_cache[sector]; buf.resize(SECTOR_SIZE);
    memcpy(buf.data(), data, SECTOR_SIZE);
}
const uint8_t* SectorCache::Get(uint64_t sector) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(sector); return (it != m_cache.end()) ? it->second.data() : nullptr;
}
bool SectorCache::Has(uint64_t sector) const { std::lock_guard<std::mutex> lock(m_mutex); return m_cache.find(sector) != m_cache.end(); }
void SectorCache::Evict(uint64_t sector) { std::lock_guard<std::mutex> lock(m_mutex); m_cache.erase(sector); }
size_t SectorCache::Size() const { std::lock_guard<std::mutex> lock(m_mutex); return m_cache.size(); }
uint8_t* SectorCache::GetForWrite(uint64_t sector) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& buf = m_cache[sector]; if (buf.size() < SECTOR_SIZE) buf.resize(SECTOR_SIZE, 0);
    return buf.data();
}
