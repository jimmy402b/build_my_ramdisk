#include "dirty_bitmap.h"
#include <algorithm>
#include <cstring>

DirtyBitmap::DirtyBitmap(uint64_t totalSectors)
    : m_totalSectors(totalSectors), m_bits((totalSectors + 7) / 8, 0) {}

void DirtyBitmap::Set(uint64_t sector) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sector < m_totalSectors) m_bits[ByteIndex(sector)] |= BitMask(sector);
}
bool DirtyBitmap::Get(uint64_t sector) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sector >= m_totalSectors) return false;
    return (m_bits[ByteIndex(sector)] & BitMask(sector)) != 0;
}
void DirtyBitmap::Clear(uint64_t sector) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sector < m_totalSectors) m_bits[ByteIndex(sector)] &= ~BitMask(sector);
}
void DirtyBitmap::ClearAll() { std::lock_guard<std::mutex> lock(m_mutex); std::fill(m_bits.begin(), m_bits.end(), 0); }
uint64_t DirtyBitmap::CountDirty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t c = 0;
    for (uint8_t byte : m_bits) for (int b = 0; b < 8; b++) if (byte & (1 << b)) c++;
    return c;
}
void DirtyBitmap::ForEachDirty(std::function<void(uint64_t)> callback) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_bits.size(); i++) {
        if (m_bits[i] == 0) continue;
        for (int b = 0; b < 8; b++)
            if (m_bits[i] & (1 << b)) { uint64_t s = i * 8 + b; if (s < m_totalSectors) callback(s); }
    }
}
