#include "vhd_engine.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

// ============================================================
// VHD Format constants (Microsoft VHD Specification)
// ============================================================

static const uint32_t VHD_SECTOR_SIZE    = 512;
static const uint32_t VHD_FOOTER_SIZE    = 512;
static const uint32_t VHD_DYNHDR_SIZE    = 1024;
static const uint32_t VHD_BLOCK_SIZE_DEF = 2 * 1024 * 1024;  // 2 MB default
static const uint32_t VHD_SECTORS_PER_BLOCK = VHD_BLOCK_SIZE_DEF / VHD_SECTOR_SIZE;

static const char     VHD_FOOTER_COOKIE[] = "conectix";
static const char     VHD_DYNHDR_COOKIE[] = "cxsparse";

enum VhdDiskType {
    VHD_TYPE_NONE        = 0,
    VHD_TYPE_FIXED       = 2,
    VHD_TYPE_DYNAMIC     = 3,
    VHD_TYPE_DIFFERENCING = 4,
};

#pragma pack(push, 1)

struct VhdFooter {
    char     cookie[8];         // "conectix"
    uint32_t features;         // bit 0=temp, bit 1=reserved
    uint32_t fileFormatVersion;
    uint64_t dataOffset;       // for dynamic: offset to dyn header (absolute)
    uint32_t timestamp;        // seconds since 2000-01-01
    char     creatorApp[4];
    uint32_t creatorVersion;
    char     creatorHostOS[4];
    uint64_t originalSize;     // bytes
    uint64_t currentSize;      // bytes
    uint32_t diskGeometry;     // CHS
    uint32_t diskType;         // 2=fixed, 3=dynamic, 4=differencing
    uint32_t checksum;
    uint8_t  uniqueId[16];     // UUID
    uint8_t  savedState;
    uint8_t  reserved[427];
};

struct VhdDynamicHeader {
    char     cookie[8];         // "cxsparse"
    uint64_t dataOffset;       // absolute offset to BAT (must be sector-aligned)
    uint64_t tableOffset;      // absolute offset to BAT
    uint32_t headerVersion;
    uint32_t maxTableEntries;  // number of BAT entries
    uint32_t blockSize;        // bytes per block (must be sector-aligned)
    uint32_t checksum;
    uint8_t  parentUniqueId[16];
    uint32_t parentTimestamp;
    uint32_t reserved1;
    uint8_t  parentUnicodeName[512];
    uint8_t  parentLocator[8][24];
    uint8_t  reserved2[256];
};

#pragma pack(pop)

// ============================================================
// Helpers
// ============================================================

// VHD format is big-endian on disk. Simple byte-level swap.
static uint32_t Swap32(uint32_t v) {
    uint8_t* b = (uint8_t*)&v;
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}
static uint64_t Swap64(uint64_t v) {
    uint8_t* b = (uint8_t*)&v;
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
           ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
           ((uint64_t)b[6] << 8)  |  (uint64_t)b[7];
}

static uint32_t CalcFooterChecksum(const VhdFooter& f) {
    VhdFooter tmp = f;
    tmp.checksum = 0;
    uint32_t sum = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&tmp);
    for (size_t i = 0; i < sizeof(VhdFooter); i++) {
        sum += p[i];
    }
    return ~sum;  // one's complement
}

static uint32_t CalcDynHdrChecksum(const VhdDynamicHeader& h) {
    VhdDynamicHeader tmp = h;
    tmp.checksum = 0;
    uint32_t sum = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&tmp);
    for (size_t i = 0; i < sizeof(VhdDynamicHeader); i++) {
        sum += p[i];
    }
    return ~sum;
}

static uint32_t TimeSince2000() {
    // VHD timestamp: seconds since Jan 1, 2000 00:00:00 GMT
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    // FILETIME = 100-ns intervals since 1601-01-01
    // 2000-01-01 in FILETIME
    static const uint64_t EPOCH_2000 = 125911584000000000ULL;
    uint64_t ft64 = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<uint32_t>((ft64 - EPOCH_2000) / 10000000ULL);
}

static void FillFooter(VhdFooter& f, uint64_t sizeBytes, uint32_t diskType) {
    memset(&f, 0, sizeof(f));
    memcpy(f.cookie, VHD_FOOTER_COOKIE, 8);
    f.features = 2;  // reserved bit always set
    f.fileFormatVersion = 0x00010000;
    f.dataOffset = (diskType == VHD_TYPE_DYNAMIC) ? VHD_SECTOR_SIZE : 0xFFFFFFFFFFFFFFFFULL;
    f.timestamp = TimeSince2000();
    memcpy(f.creatorApp, "RDSK", 4);  // RamDiSK
    f.creatorVersion = 0x00010000;
    memcpy(f.creatorHostOS, "Wi2k", 4);
    f.originalSize = sizeBytes;
    f.currentSize = sizeBytes;
    // CHS geometry: use 16 heads, 255 sectors/track
    uint64_t totalSectors = sizeBytes / VHD_SECTOR_SIZE;
    uint32_t cylinders = static_cast<uint32_t>(totalSectors / (16 * 255));
    if (cylinders > 65535) cylinders = 65535;
    uint32_t heads = 16;
    uint32_t spt = 255;
    f.diskGeometry = (cylinders << 16) | (heads << 8) | spt;
    f.diskType = diskType;
    f.savedState = 0;

    // Generate a simple pseudo-UUID from timestamp
    uint32_t ts = f.timestamp;
    for (int i = 0; i < 16; i++) {
        f.uniqueId[i] = static_cast<uint8_t>((ts >> ((i % 4) * 8)) & 0xFF);
        ts = ts * 1103515245 + 12345;
    }
    // Convert multi-byte fields to big-endian (VHD on-disk format)
    f.features = Swap32(f.features);
    f.fileFormatVersion = Swap32(f.fileFormatVersion);
    f.dataOffset = Swap64(f.dataOffset);
    f.timestamp = Swap32(f.timestamp);
    f.creatorVersion = Swap32(f.creatorVersion);
    f.originalSize = Swap64(f.originalSize);
    f.currentSize = Swap64(f.currentSize);
    f.diskGeometry = Swap32(f.diskGeometry);
    f.diskType = Swap32(f.diskType);

    f.checksum = CalcFooterChecksum(f);
}

// ============================================================
// Impl
// ============================================================

struct VhdEngine::Impl {
    std::string path;
    uint64_t sizeBytes = 0;
    uint64_t totalSectors = 0;
    uint32_t blockSize = VHD_BLOCK_SIZE_DEF;
    uint32_t sectorsPerBlock = VHD_SECTORS_PER_BLOCK;
    uint32_t maxTableEntries = 0;
    bool dynamic = false;
    bool open = false;
    bool writable = false;
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    std::string lastError;

    // Dynamic VHD: BAT — maps block index → sector offset (0xFFFFFFFF = unallocated)
    std::vector<uint32_t> bat;

    // For dynamic VHD: cached block data (0 = not cached)
    struct BlockCache {
        bool dirty = false;
        std::vector<uint8_t> data;
    };
    std::vector<BlockCache> blockCache;

    uint64_t batOffset = 0;  // file offset to BAT
};

VhdEngine::VhdEngine() : pImpl(new Impl()) {}

VhdEngine::~VhdEngine() { Close(); }

// ============================================================
// Create
// ============================================================

bool VhdEngine::Create(const std::string& path, uint64_t sizeBytes, bool dynamic) {
    if (pImpl->open) {
        pImpl->lastError = "VhdEngine already has an open file; close it first";
        return false;
    }

    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        pImpl->lastError = "Cannot create file: " + path;
        return false;
    }

    if (!dynamic) {
        // Fixed VHD: pre-allocate file to size+512, write footer at end
        LARGE_INTEGER li;
        li.QuadPart = sizeBytes + VHD_FOOTER_SIZE;
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        SetEndOfFile(h);

        VhdFooter footer;
        FillFooter(footer, sizeBytes, VHD_TYPE_FIXED);

        li.QuadPart = sizeBytes;
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        DWORD written = 0;
        WriteFile(h, &footer, sizeof(footer), &written, nullptr);

        pImpl->fileHandle = h;
        pImpl->path = path;
        pImpl->sizeBytes = sizeBytes;
        pImpl->totalSectors = sizeBytes / VHD_SECTOR_SIZE;
        pImpl->dynamic = false;
        pImpl->open = true;
        pImpl->writable = true;
        return true;
    }

    // Calculate layout
    uint64_t totalSectors = sizeBytes / VHD_SECTOR_SIZE;
    uint32_t blockSize = VHD_BLOCK_SIZE_DEF;
    uint32_t sectorsPerBlock = blockSize / VHD_SECTOR_SIZE;
    uint32_t maxTableEntries = static_cast<uint32_t>(
        (totalSectors + sectorsPerBlock - 1) / sectorsPerBlock);

    // Layout:
    // Sector 0: Footer copy (512 bytes)
    // Sector 1-2: Dynamic header (1024 bytes)
    // Next sectors: BAT (ceil(maxTableEntries * 4 / 512) sectors)
    uint32_t batSectors = (maxTableEntries * 4 + VHD_SECTOR_SIZE - 1) / VHD_SECTOR_SIZE;
    uint64_t batFileOffset = VHD_SECTOR_SIZE + VHD_DYNHDR_SIZE;  // bytes 512+1024 = 1536

    // Write initial file size: footer copy + dynhdr + BAT
    // We'll set EOF after writing these, then append data blocks as needed
    uint64_t metaSize = batFileOffset + static_cast<uint64_t>(batSectors) * VHD_SECTOR_SIZE;

    // Write footer copy at offset 0
    VhdFooter footer;
    FillFooter(footer, sizeBytes, VHD_TYPE_DYNAMIC);
    footer.dataOffset = Swap64(VHD_SECTOR_SIZE);  // big-endian, points to dyn header

    DWORD written = 0;
    if (!WriteFile(h, &footer, sizeof(footer), &written, nullptr) || written != sizeof(footer)) {
        pImpl->lastError = "Failed to write footer copy";
        CloseHandle(h); DeleteFileA(path.c_str()); return false;
    }

    // Write dynamic header
    VhdDynamicHeader dynHdr;
    memset(&dynHdr, 0, sizeof(dynHdr));
    memcpy(dynHdr.cookie, VHD_DYNHDR_COOKIE, 8);
    dynHdr.dataOffset = 0xFFFFFFFFFFFFFFFFULL;
    dynHdr.tableOffset = Swap64(batFileOffset);
    dynHdr.headerVersion = Swap32(0x00010000);
    dynHdr.maxTableEntries = Swap32(maxTableEntries);
    dynHdr.blockSize = Swap32(blockSize);
    dynHdr.checksum = 0;
    dynHdr.checksum = Swap32(CalcDynHdrChecksum(dynHdr)); // checksum computed on BE struct, result also in BE

    if (!WriteFile(h, &dynHdr, sizeof(dynHdr), &written, nullptr) || written != sizeof(dynHdr)) {
        pImpl->lastError = "Failed to write dynamic header";
        CloseHandle(h); DeleteFileA(path.c_str()); return false;
    }

    // Write BAT (all unallocated = 0xFFFFFFFF), padded to full sectors
    std::vector<uint32_t> bat(maxTableEntries, 0xFFFFFFFF);
    if (!WriteFile(h, bat.data(), static_cast<DWORD>(bat.size() * 4), &written, nullptr) ||
        written != bat.size() * 4) {
        pImpl->lastError = "Failed to write BAT";
        CloseHandle(h); DeleteFileA(path.c_str()); return false;
    }

    // Pad remaining BAT sectors with zeros
    DWORD batDataBytes = static_cast<DWORD>(bat.size() * 4);
    DWORD batTotalBytes = batSectors * VHD_SECTOR_SIZE;
    if (batTotalBytes > batDataBytes) {
        std::vector<uint8_t> pad(batTotalBytes - batDataBytes, 0);
        WriteFile(h, pad.data(), static_cast<DWORD>(pad.size()), &written, nullptr);
    }

    // Write the end footer at metaSize
    LARGE_INTEGER liMeta;
    liMeta.QuadPart = static_cast<LONGLONG>(metaSize);
    SetFilePointerEx(h, liMeta, nullptr, FILE_BEGIN);
    // For dynamic VHD, the footer must be at EOF which is currently at metaSize
    VhdFooter endFooter = footer;
    endFooter.dataOffset = Swap64(VHD_SECTOR_SIZE);
    endFooter.checksum = CalcFooterChecksum(endFooter);

    if (!WriteFile(h, &endFooter, sizeof(endFooter), &written, nullptr) || written != sizeof(endFooter)) {
        pImpl->lastError = "Failed to write end footer";
        CloseHandle(h); DeleteFileA(path.c_str()); return false;
    }

    // Initialize state
    pImpl->fileHandle = h;
    pImpl->path = path;
    pImpl->sizeBytes = sizeBytes;
    pImpl->totalSectors = totalSectors;
    pImpl->blockSize = blockSize;
    pImpl->sectorsPerBlock = sectorsPerBlock;
    pImpl->maxTableEntries = maxTableEntries;
    pImpl->batOffset = batFileOffset;
    pImpl->dynamic = true;
    pImpl->open = true;
    pImpl->writable = true;
    pImpl->bat = bat;
    pImpl->blockCache.resize(maxTableEntries);

    return true;
}

// ============================================================
// Open
// ============================================================

bool VhdEngine::Open(const std::string& path, bool writable) {
    if (pImpl->open) {
        pImpl->lastError = "VhdEngine already has an open file; close it first";
        return false;
    }

    DWORD access = GENERIC_READ;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    if (writable) {
        access |= GENERIC_WRITE;
    }

    HANDLE h = CreateFileA(path.c_str(), access, share,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        pImpl->lastError = "Cannot open file: " + path;
        return false;
    }

    // Read the footer from end of file
    LARGE_INTEGER li;
    li.QuadPart = -static_cast<LONGLONG>(VHD_FOOTER_SIZE);
    if (!SetFilePointerEx(h, li, nullptr, FILE_END)) {
        pImpl->lastError = "Seek failed";
        CloseHandle(h); return false;
    }

    VhdFooter footer;
    DWORD read = 0;
    if (!ReadFile(h, &footer, sizeof(footer), &read, nullptr) || read != sizeof(footer)) {
        pImpl->lastError = "Failed to read VHD footer";
        CloseHandle(h); return false;
    }

    // Validate footer
    if (memcmp(footer.cookie, VHD_FOOTER_COOKIE, 8) != 0) {
        pImpl->lastError = "Not a valid VHD file (bad cookie)";
        CloseHandle(h); return false;
    }

    // Swap footer fields from big-endian to host byte order
    footer.features = Swap32(footer.features);
    footer.fileFormatVersion = Swap32(footer.fileFormatVersion);
    footer.dataOffset = Swap64(footer.dataOffset);
    footer.timestamp = Swap32(footer.timestamp);
    footer.creatorVersion = Swap32(footer.creatorVersion);
    footer.originalSize = Swap64(footer.originalSize);
    footer.currentSize = Swap64(footer.currentSize);
    footer.diskGeometry = Swap32(footer.diskGeometry);
    footer.diskType = Swap32(footer.diskType);

    if (footer.diskType == VHD_TYPE_FIXED) {
        // Fixed VHD: raw data from offset 0, footer at EOF
        pImpl->fileHandle = h;
        pImpl->dynamic = false;
        pImpl->sizeBytes = footer.currentSize;
        pImpl->totalSectors = footer.currentSize / VHD_SECTOR_SIZE;
        pImpl->open = true;
        pImpl->writable = writable;
        pImpl->path = path;
        return true;
    }

    if (footer.diskType != VHD_TYPE_DYNAMIC) {
        pImpl->lastError = "Only dynamic and fixed VHD supported";
        CloseHandle(h); return false;
    }

    // Read dynamic header
    li.QuadPart = footer.dataOffset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        pImpl->lastError = "Seek to dynamic header failed";
        CloseHandle(h); return false;
    }

    VhdDynamicHeader dynHdr;
    read = 0;
    if (!ReadFile(h, &dynHdr, sizeof(dynHdr), &read, nullptr) || read != sizeof(dynHdr)) {
        pImpl->lastError = "Failed to read dynamic header";
        CloseHandle(h); return false;
    }

    if (memcmp(dynHdr.cookie, VHD_DYNHDR_COOKIE, 8) != 0) {
        pImpl->lastError = "Invalid dynamic header cookie";
        CloseHandle(h); return false;
    }

    // Swap dynhdr from big-endian
    dynHdr.dataOffset = Swap64(dynHdr.dataOffset);
    dynHdr.tableOffset = Swap64(dynHdr.tableOffset);
    dynHdr.headerVersion = Swap32(dynHdr.headerVersion);
    dynHdr.maxTableEntries = Swap32(dynHdr.maxTableEntries);
    dynHdr.blockSize = Swap32(dynHdr.blockSize);

    // Read BAT
    uint32_t maxEntries = dynHdr.maxTableEntries;
    std::vector<uint32_t> bat(maxEntries);
    li.QuadPart = dynHdr.tableOffset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        pImpl->lastError = "Seek to BAT failed";
        CloseHandle(h); return false;
    }

    read = 0;
    DWORD batBytes = maxEntries * 4;
    if (!ReadFile(h, bat.data(), batBytes, &read, nullptr) || read != batBytes) {
        pImpl->lastError = "Failed to read BAT";
        CloseHandle(h); return false;
    }
    // Swap BAT from big-endian
    for (uint32_t i = 0; i < maxEntries; i++)
        bat[i] = Swap32(bat[i]);

    // Initialize state
    pImpl->fileHandle = h;
    pImpl->path = path;
    pImpl->sizeBytes = footer.currentSize;
    pImpl->totalSectors = footer.currentSize / VHD_SECTOR_SIZE;
    pImpl->blockSize = dynHdr.blockSize;
    pImpl->sectorsPerBlock = dynHdr.blockSize / VHD_SECTOR_SIZE;
    pImpl->maxTableEntries = maxEntries;
    pImpl->batOffset = dynHdr.tableOffset;
    pImpl->dynamic = true;
    pImpl->open = true;
    pImpl->writable = writable;
    pImpl->bat = bat;
    pImpl->blockCache.resize(maxEntries);

    return true;
}

// ============================================================
// Close
// ============================================================

void VhdEngine::Close() {
    if (pImpl->fileHandle != INVALID_HANDLE_VALUE) {
        if (!pImpl->dynamic) {
            // Fixed VHD: nothing to flush, just close
            CloseHandle(pImpl->fileHandle);
            pImpl->fileHandle = INVALID_HANDLE_VALUE;
            pImpl->open = false;
            pImpl->path.clear();
            return;
        }

        // Get current end-of-file (where end footer sits)
        LARGE_INTEGER liZero;
        liZero.QuadPart = 0;
        LARGE_INTEGER eof;
        SetFilePointerEx(pImpl->fileHandle, liZero, &eof, FILE_END);

        // The end footer is at EOF - VHD_FOOTER_SIZE
        uint64_t dataAreaEnd = eof.QuadPart - VHD_FOOTER_SIZE;

        bool batModified = false;

        // Flush any dirty blocks
        for (size_t i = 0; i < pImpl->blockCache.size(); i++) {
            if (pImpl->blockCache[i].dirty && !pImpl->blockCache[i].data.empty()) {
                uint32_t batEntry = pImpl->bat[i];

                if (batEntry == 0xFFFFFFFF) {
                    // Allocate new block at end of data area
                    batEntry = static_cast<uint32_t>(dataAreaEnd / VHD_SECTOR_SIZE);
                    pImpl->bat[i] = batEntry;
                    batModified = true;
                    dataAreaEnd += pImpl->blockSize;
                }

                // Write block to file
                LARGE_INTEGER li;
                li.QuadPart = static_cast<uint64_t>(batEntry) * VHD_SECTOR_SIZE;
                SetFilePointerEx(pImpl->fileHandle, li, nullptr, FILE_BEGIN);
                DWORD written = 0;
                WriteFile(pImpl->fileHandle, pImpl->blockCache[i].data.data(),
                          pImpl->blockSize, &written, nullptr);
            }
        }

        // If BAT was modified, write it back
        if (batModified) {
            // Swap BAT to big-endian before writing
            std::vector<uint32_t> batBe(pImpl->bat);
            for (auto& v : batBe) v = Swap32(v);

            LARGE_INTEGER liBat;
            liBat.QuadPart = static_cast<LONGLONG>(pImpl->batOffset);
            SetFilePointerEx(pImpl->fileHandle, liBat, nullptr, FILE_BEGIN);
            DWORD written = 0;
            WriteFile(pImpl->fileHandle, batBe.data(),
                      static_cast<DWORD>(batBe.size() * 4), &written, nullptr);

            // Write new end footer at dataAreaEnd
            LARGE_INTEGER liFooter;
            liFooter.QuadPart = static_cast<LONGLONG>(dataAreaEnd);
            SetFilePointerEx(pImpl->fileHandle, liFooter, nullptr, FILE_BEGIN);

            VhdFooter footer;
            memset(&footer, 0, sizeof(footer));
            // Seek back from current file position: read from sector 0
            // Actually, just write a fresh footer
            // Read footer copy from start, swap to host, update, swap back
            LARGE_INTEGER liFtr;
            liFtr.QuadPart = 0;
            SetFilePointerEx(pImpl->fileHandle, liFtr, nullptr, FILE_BEGIN);
            DWORD read = 0;
            ReadFile(pImpl->fileHandle, &footer, sizeof(footer), &read, nullptr);

            footer.features = Swap32(footer.features);
            footer.fileFormatVersion = Swap32(footer.fileFormatVersion);
            footer.dataOffset = Swap64(footer.dataOffset);
            footer.timestamp = Swap32(footer.timestamp);
            footer.creatorVersion = Swap32(footer.creatorVersion);
            footer.originalSize = Swap64(footer.originalSize);
            footer.currentSize = Swap64(footer.currentSize);
            footer.diskGeometry = Swap32(footer.diskGeometry);
            footer.diskType = Swap32(footer.diskType);

            footer.dataOffset = VHD_SECTOR_SIZE;  // host value, will be swapped below

            footer.features = Swap32(footer.features);
            footer.fileFormatVersion = Swap32(footer.fileFormatVersion);
            footer.dataOffset = Swap64(footer.dataOffset);
            footer.timestamp = Swap32(footer.timestamp);
            footer.creatorVersion = Swap32(footer.creatorVersion);
            footer.originalSize = Swap64(footer.originalSize);
            footer.currentSize = Swap64(footer.currentSize);
            footer.diskGeometry = Swap32(footer.diskGeometry);
            footer.diskType = Swap32(footer.diskType);

            footer.checksum = CalcFooterChecksum(footer);

            liFooter.QuadPart = static_cast<LONGLONG>(dataAreaEnd);
            SetFilePointerEx(pImpl->fileHandle, liFooter, nullptr, FILE_BEGIN);
            WriteFile(pImpl->fileHandle, &footer, sizeof(footer), &written, nullptr);

            // Truncate file to new size
            LARGE_INTEGER newSize;
            newSize.QuadPart = static_cast<LONGLONG>(dataAreaEnd + VHD_FOOTER_SIZE);
            SetFilePointerEx(pImpl->fileHandle, newSize, nullptr, FILE_BEGIN);
            SetEndOfFile(pImpl->fileHandle);
        }

        CloseHandle(pImpl->fileHandle);
        pImpl->fileHandle = INVALID_HANDLE_VALUE;
    }
    pImpl->open = false;
    pImpl->path.clear();
    pImpl->sizeBytes = 0;
    pImpl->totalSectors = 0;
    pImpl->bat.clear();
    pImpl->blockCache.clear();
}

// ============================================================
// ReadSectors
// ============================================================

bool VhdEngine::ReadSectors(uint64_t sectorOffset, uint32_t sectorCount, void* buffer) {
    if (!pImpl->open) { pImpl->lastError = "VhdEngine not open"; return false; }
    if (sectorOffset + sectorCount > pImpl->totalSectors) { pImpl->lastError = "ReadSectors: out of bounds"; return false; }

    // Fixed VHD: direct file access, no BAT
    if (!pImpl->dynamic) {
        uint64_t fileOffset = sectorOffset * VHD_SECTOR_SIZE;
        LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(fileOffset);
        if (!SetFilePointerEx(pImpl->fileHandle, li, nullptr, FILE_BEGIN)) {
            pImpl->lastError = "Seek failed"; return false;
        }
        DWORD totalBytes = sectorCount * VHD_SECTOR_SIZE;
        DWORD rd = 0;
        if (!ReadFile(pImpl->fileHandle, buffer, totalBytes, &rd, nullptr) || rd != totalBytes) {
            pImpl->lastError = "ReadFile failed"; return false;
        }
        return true;
    }

    uint8_t* dst = static_cast<uint8_t*>(buffer);

    while (sectorCount > 0) {
        uint32_t blockIdx = static_cast<uint32_t>(sectorOffset / pImpl->sectorsPerBlock);
        uint32_t sectorInBlock = static_cast<uint32_t>(sectorOffset % pImpl->sectorsPerBlock);
        uint32_t sectorsInThisBlock = pImpl->sectorsPerBlock - sectorInBlock;
        uint32_t toRead = (sectorCount < sectorsInThisBlock) ? sectorCount : sectorsInThisBlock;
        uint32_t bytesToRead = toRead * VHD_SECTOR_SIZE;

        uint32_t batEntry = pImpl->bat[blockIdx];

        // Check cache first (may have data even for unallocated blocks)
        if (!pImpl->blockCache[blockIdx].data.empty()) {
            memcpy(dst, pImpl->blockCache[blockIdx].data.data() + sectorInBlock * VHD_SECTOR_SIZE,
                   bytesToRead);
        } else if (batEntry == 0xFFFFFFFF) {
            // Unallocated block, not in cache — return zeroes
            memset(dst, 0, bytesToRead);
        } else {
            // Read entire block from file (seek to block start, not sector offset)
            uint64_t blockStart = static_cast<uint64_t>(batEntry) * VHD_SECTOR_SIZE;
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(blockStart);
            if (!SetFilePointerEx(pImpl->fileHandle, li, nullptr, FILE_BEGIN)) {
                pImpl->lastError = "Seek failed in ReadSectors";
                return false;
            }
            // Load entire block into cache for future reads
            pImpl->blockCache[blockIdx].data.resize(pImpl->blockSize);
            pImpl->blockCache[blockIdx].dirty = false;

            DWORD read = 0;
            if (!ReadFile(pImpl->fileHandle, pImpl->blockCache[blockIdx].data.data(),
                          pImpl->blockSize, &read, nullptr) || read != pImpl->blockSize) {
                pImpl->lastError = "ReadFile failed in ReadSectors";
                pImpl->blockCache[blockIdx].data.clear();
                return false;
            }
            memcpy(dst, pImpl->blockCache[blockIdx].data.data() + sectorInBlock * VHD_SECTOR_SIZE,
                   bytesToRead);
        }

        dst += bytesToRead;
        sectorOffset += toRead;
        sectorCount -= toRead;
    }

    return true;
}

// ============================================================
// WriteSectors
// ============================================================

bool VhdEngine::WriteSectors(uint64_t sectorOffset, uint32_t sectorCount, const void* buffer) {
    if (!pImpl->open || !pImpl->writable) { pImpl->lastError = "VhdEngine not open or not writable"; return false; }
    if (sectorOffset + sectorCount > pImpl->totalSectors) { pImpl->lastError = "WriteSectors: out of bounds"; return false; }

    // Fixed VHD: direct file access
    if (!pImpl->dynamic) {
        uint64_t fileOffset = sectorOffset * VHD_SECTOR_SIZE;
        LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(fileOffset);
        if (!SetFilePointerEx(pImpl->fileHandle, li, nullptr, FILE_BEGIN)) {
            pImpl->lastError = "Seek failed"; return false;
        }
        DWORD totalBytes = sectorCount * VHD_SECTOR_SIZE;
        DWORD wr = 0;
        if (!WriteFile(pImpl->fileHandle, buffer, totalBytes, &wr, nullptr) || wr != totalBytes) {
            pImpl->lastError = "WriteFile failed"; return false;
        }
        return true;
    }

    const uint8_t* src = static_cast<const uint8_t*>(buffer);

    while (sectorCount > 0) {
        uint32_t blockIdx = static_cast<uint32_t>(sectorOffset / pImpl->sectorsPerBlock);
        uint32_t sectorInBlock = static_cast<uint32_t>(sectorOffset % pImpl->sectorsPerBlock);
        uint32_t sectorsInThisBlock = pImpl->sectorsPerBlock - sectorInBlock;
        uint32_t toWrite = (sectorCount < sectorsInThisBlock) ? sectorCount : sectorsInThisBlock;
        uint32_t bytesToWrite = toWrite * VHD_SECTOR_SIZE;

        // Ensure block is in cache
        if (pImpl->blockCache[blockIdx].data.empty()) {
            pImpl->blockCache[blockIdx].data.resize(pImpl->blockSize);
            // If block is allocated, read existing data first
            if (pImpl->bat[blockIdx] != 0xFFFFFFFF) {
                uint64_t fileOffset = static_cast<uint64_t>(pImpl->bat[blockIdx]) * VHD_SECTOR_SIZE;
                LARGE_INTEGER li;
                li.QuadPart = static_cast<LONGLONG>(fileOffset);
                SetFilePointerEx(pImpl->fileHandle, li, nullptr, FILE_BEGIN);
                DWORD read = 0;
                ReadFile(pImpl->fileHandle, pImpl->blockCache[blockIdx].data.data(),
                         pImpl->blockSize, &read, nullptr);
                // Partial reads OK — rest is zeroes
            } else {
                memset(pImpl->blockCache[blockIdx].data.data(), 0, pImpl->blockSize);
            }
        }

        // Write into cache
        memcpy(pImpl->blockCache[blockIdx].data.data() + sectorInBlock * VHD_SECTOR_SIZE,
               src, bytesToWrite);
        pImpl->blockCache[blockIdx].dirty = true;

        src += bytesToWrite;
        sectorOffset += toWrite;
        sectorCount -= toWrite;
    }

    return true;
}

// ============================================================
// Bulk I/O
// ============================================================

bool VhdEngine::ReadAll(void* buffer) {
    if (!pImpl->open || pImpl->totalSectors == 0) return false;
    return ReadSectors(0, static_cast<uint32_t>(pImpl->totalSectors), buffer);
}

bool VhdEngine::WriteAll(const void* buffer) {
    if (!pImpl->open || !pImpl->writable || pImpl->totalSectors == 0) return false;
    return WriteSectors(0, static_cast<uint32_t>(pImpl->totalSectors), buffer);
}

// ============================================================
// Metadata
// ============================================================

uint64_t VhdEngine::GetTotalSectors() const { return pImpl->totalSectors; }
uint64_t VhdEngine::GetSizeBytes() const { return pImpl->sizeBytes; }
bool     VhdEngine::IsDynamic() const { return pImpl->dynamic; }
bool     VhdEngine::IsOpen() const { return pImpl->open; }
const std::string& VhdEngine::GetPath() const { return pImpl->path; }
std::string VhdEngine::GetLastError() const { return pImpl->lastError; }
