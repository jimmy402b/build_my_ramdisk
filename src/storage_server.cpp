#include "storage_server.h"
#include "vhd_engine.h"
#include "dirty_bitmap.h"
#include "sector_cache.h"
#include "utils.h"
#include <windows.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>

// AIM proxy pipe protocol constants (placeholder — should match Task 5 findings)
#pragma pack(push, 1)
struct AimPipeRequest { uint32_t op; uint64_t offset; uint32_t count; uint32_t dataLen; };
struct AimPipeResponse { uint32_t status; uint32_t dataLen; };
#pragma pack(pop)
enum { AIM_OP_READ = 1, AIM_OP_WRITE = 2, AIM_OP_FLUSH = 3, AIM_OP_INFO = 4 };
enum { AIM_OK = 0, AIM_ERR = 1 };

struct StorageServer::Impl {
    std::string pipeName, vhdPath;
    uint64_t diskSizeBytes = 0;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    std::thread serverThread;
    std::atomic<bool> running{false};
    Logger logger;
    VhdEngine vhd;
    std::unique_ptr<DirtyBitmap> dirty;
    std::unique_ptr<SectorCache> cache;
};

StorageServer::StorageServer() : pImpl(new Impl()) {}
StorageServer::~StorageServer() { Stop(); }

bool StorageServer::Start(const std::string& pipeName, const std::string& vhdPath, uint64_t diskSizeBytes) {
    if (pImpl->running) return false;
    pImpl->pipeName = pipeName; pImpl->vhdPath = vhdPath; pImpl->diskSizeBytes = diskSizeBytes;
    if (!pImpl->vhd.Open(vhdPath, true)) {
        if (pImpl->logger) pImpl->logger("VHD open failed: " + pImpl->vhd.GetLastError());
        return false;
    }
    uint64_t total = pImpl->vhd.GetTotalSectors();
    pImpl->dirty = std::unique_ptr<DirtyBitmap>(new DirtyBitmap(total));
    pImpl->cache = std::unique_ptr<SectorCache>(new SectorCache());

    pImpl->hPipe = CreateNamedPipeA(pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 64*1024, 64*1024, 0, nullptr);
    if (pImpl->hPipe == INVALID_HANDLE_VALUE) {
        if (pImpl->logger) pImpl->logger("CreateNamedPipe failed: " + GetLastWinErrorMessage());
        return false;
    }

    pImpl->running = true;
    pImpl->serverThread = std::thread([this]() {
        if (pImpl->logger) pImpl->logger("StorageServer waiting for AIM driver...");
        OVERLAPPED ol = {}; ol.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        ConnectNamedPipe(pImpl->hPipe, &ol);
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING && err != ERROR_PIPE_CONNECTED) {
            if (pImpl->logger) pImpl->logger("ConnectNamedPipe failed");
            CloseHandle(ol.hEvent); return;
        }
        while (pImpl->running && WaitForSingleObject(ol.hEvent, 100) == WAIT_TIMEOUT) {}
        CloseHandle(ol.hEvent);
        if (!pImpl->running) return;
        if (pImpl->logger) pImpl->logger("AIM driver connected");

        uint8_t buf[65536], resp[65536];
        DWORD rd = 0;
        while (pImpl->running) {
            if (!ReadFile(pImpl->hPipe, buf, sizeof(buf), &rd, nullptr)) break;
            if (rd < sizeof(AimPipeRequest)) continue;
            auto* req = (AimPipeRequest*)buf;
            auto* rsp = (AimPipeResponse*)resp;
            uint32_t rspLen = sizeof(AimPipeResponse);

            switch (req->op) {
            case AIM_OP_READ: {
                uint8_t* dst = resp + sizeof(AimPipeResponse);
                for (uint32_t i = 0; i < req->count; i++) {
                    uint64_t sec = req->offset + i;
                    const uint8_t* c = pImpl->cache->Get(sec);
                    if (c) { memcpy(dst + i*512, c, 512); }
                    else {
                        uint8_t tmp[512];
                        pImpl->vhd.ReadSectors(sec, 1, tmp);
                        pImpl->cache->Put(sec, tmp);
                        memcpy(dst + i*512, tmp, 512);
                    }
                }
                rsp->status = AIM_OK; rsp->dataLen = req->count * 512; rspLen += rsp->dataLen;
                break;
            }
            case AIM_OP_WRITE: {
                const uint8_t* src = buf + sizeof(AimPipeRequest);
                for (uint32_t i = 0; i < req->count; i++) {
                    uint64_t sec = req->offset + i;
                    pImpl->cache->Put(sec, src + i*512);
                    pImpl->dirty->Set(sec);
                }
                rsp->status = AIM_OK; rsp->dataLen = 0;
                break;
            }
            case AIM_OP_FLUSH: SaveDirty(); rsp->status = AIM_OK; rsp->dataLen = 0; break;
            case AIM_OP_INFO: {
                *(uint64_t*)(resp + sizeof(AimPipeResponse)) = pImpl->diskSizeBytes;
                rsp->status = AIM_OK; rsp->dataLen = 8; rspLen += 8;
                break;
            }
            default: rsp->status = AIM_ERR; rsp->dataLen = 0; break;
            }
            DWORD wr = 0; WriteFile(pImpl->hPipe, resp, rspLen, &wr, nullptr);
        }
    });
    return true;
}

void StorageServer::Stop() {
    pImpl->running = false;
    if (pImpl->hPipe != INVALID_HANDLE_VALUE) { DisconnectNamedPipe(pImpl->hPipe); CloseHandle(pImpl->hPipe); pImpl->hPipe = INVALID_HANDLE_VALUE; }
    if (pImpl->serverThread.joinable()) pImpl->serverThread.join();
    pImpl->vhd.Close();
}

uint64_t StorageServer::SaveDirty() {
    if (!pImpl->dirty) return 0;
    uint64_t total = pImpl->dirty->TotalSectors(), count = 0;
    if (pImpl->dirty->CountDirty() > total / 4) {
        std::vector<uint8_t> full(total * 512);
        for (uint64_t s = 0; s < total; s++) {
            const uint8_t* c = pImpl->cache->Get(s);
            if (c) memcpy(full.data() + s*512, c, 512);
            else { uint8_t tmp[512]; memset(tmp, 0, 512); pImpl->vhd.ReadSectors(s, 1, tmp); memcpy(full.data() + s*512, tmp, 512); }
        }
        pImpl->vhd.WriteAll(full.data());
        pImpl->dirty->ClearAll(); return total;
    }
    pImpl->dirty->ForEachDirty([&](uint64_t s) {
        const uint8_t* d = pImpl->cache->Get(s);
        if (d) { pImpl->vhd.WriteSectors(s, 1, d); count++; }
    });
    pImpl->dirty->ClearAll();
    if (pImpl->logger) pImpl->logger("Incremental save: " + std::to_string(count) + " sectors (" + FormatSize(count*512) + ")");
    return count;
}

uint64_t StorageServer::GetDirtyCount() const { return pImpl->dirty ? pImpl->dirty->CountDirty() : 0; }
bool StorageServer::IsRunning() const { return pImpl->running; }
void StorageServer::SetLogger(Logger logger) { pImpl->logger = logger; }
