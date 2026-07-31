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

// IMDPROXY protocol — from Arsenal Image Mounter imdproxy.h / Enums.cs
// All fields are ULONGLONG (uint64_t). Driver sends request_code first.
#pragma pack(push, 1)
enum IMDPROXY_REQ : uint64_t {
    REQ_NULL, REQ_INFO=1, REQ_READ=2, REQ_WRITE=3,
    REQ_CONNECT=4, REQ_CLOSE=5, REQ_UNMAP=6, REQ_ZERO=7, REQ_SCSI=8, REQ_SHARED=9
};
struct IMDPROXY_INFO_RESP  { uint64_t file_size, req_alignment, flags; };
struct IMDPROXY_READ_REQ   { uint64_t request_code, offset, length; };
struct IMDPROXY_READ_RESP  { uint64_t errorno, length; };
struct IMDPROXY_WRITE_REQ  { uint64_t request_code, offset, length; };
struct IMDPROXY_WRITE_RESP { uint64_t errorno, length; };
struct IMDPROXY_CLOSE_REQ  { uint64_t request_code; };
static const uint64_t IMDPROXY_REQ_ALIGNMENT = 512;
#pragma pack(pop)

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
            uint64_t reqCode = (rd >= 8) ? *(uint64_t*)buf : 0;

            if (reqCode == REQ_INFO) {
                // Driver requests disk info — respond with size, alignment, flags
                IMDPROXY_INFO_RESP infoResp = { pImpl->diskSizeBytes, IMDPROXY_REQ_ALIGNMENT, 0 };
                DWORD wr = 0;
                WriteFile(pImpl->hPipe, &infoResp, sizeof(infoResp), &wr, nullptr);
                continue;
            }

            if (reqCode == REQ_CLOSE) {
                if (pImpl->logger) pImpl->logger("Driver closed connection");
                break;
            }

            if (reqCode == REQ_READ) {
                auto* req = (IMDPROXY_READ_REQ*)buf;
                uint64_t offset = req->offset, length = req->length;
                uint64_t sectorCount = length / 512;
                uint64_t startSector = offset / 512;

                // Build response: header + data
                IMDPROXY_READ_RESP* rsp = (IMDPROXY_READ_RESP*)resp;
                rsp->errorno = 0;
                rsp->length = length;
                uint8_t* dst = resp + sizeof(IMDPROXY_READ_RESP);

                for (uint64_t i = 0; i < sectorCount; i++) {
                    uint64_t sec = startSector + i;
                    const uint8_t* c = pImpl->cache->Get(sec);
                    if (c) { memcpy(dst + i*512, c, 512); }
                    else {
                        uint8_t tmp[512] = {};
                        pImpl->vhd.ReadSectors(sec, 1, tmp);
                        pImpl->cache->Put(sec, tmp);
                        memcpy(dst + i*512, tmp, 512);
                    }
                }
                DWORD wr = 0;
                WriteFile(pImpl->hPipe, resp, (DWORD)(sizeof(IMDPROXY_READ_RESP) + length), &wr, nullptr);
                continue;
            }

            if (reqCode == REQ_WRITE) {
                auto* req = (IMDPROXY_WRITE_REQ*)buf;
                uint64_t offset = req->offset, length = req->length;
                uint64_t sectorCount = length / 512;
                uint64_t startSector = offset / 512;

                const uint8_t* src = buf + sizeof(IMDPROXY_WRITE_REQ);
                for (uint64_t i = 0; i < sectorCount; i++) {
                    uint64_t sec = startSector + i;
                    pImpl->cache->Put(sec, src + i*512);
                    pImpl->dirty->Set(sec);
                }
                IMDPROXY_WRITE_RESP wrResp = { 0, length };
                DWORD wr = 0;
                WriteFile(pImpl->hPipe, &wrResp, sizeof(wrResp), &wr, nullptr);
                continue;
            }

            // Unknown request — send error
            uint64_t errResp[2] = { 1, 0 };
            DWORD wr = 0;
            WriteFile(pImpl->hPipe, errResp, sizeof(errResp), &wr, nullptr);
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
