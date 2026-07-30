#pragma once

#include <cstdint>
#include <string>
#include <memory>

class VhdEngine {
public:
    VhdEngine();
    ~VhdEngine();

    // Non-copyable
    VhdEngine(const VhdEngine&) = delete;
    VhdEngine& operator=(const VhdEngine&) = delete;

    // Create a new VHD file on disk (dynamic only for MVP)
    bool Create(const std::string& path, uint64_t sizeBytes, bool dynamic = true);

    // Open an existing VHD file
    bool Open(const std::string& path, bool writable = true);

    // Close and release resources
    void Close();

    // Sector-level I/O (sector = 512 bytes)
    bool ReadSectors(uint64_t sectorOffset, uint32_t sectorCount, void* buffer);
    bool WriteSectors(uint64_t sectorOffset, uint32_t sectorCount, const void* buffer);

    // Bulk I/O for Phase 1 (full load / full save)
    bool ReadAll(void* buffer);
    bool WriteAll(const void* buffer);

    // Metadata
    uint64_t GetTotalSectors() const;
    uint64_t GetSizeBytes() const;
    bool IsDynamic() const;
    bool IsOpen() const;
    const std::string& GetPath() const;

    // Error info
    std::string GetLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
