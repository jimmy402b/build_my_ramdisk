#include "vhd_engine.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

int main() {
    const char* testPath = "test_smoke.vhd";
    uint64_t size = 10ULL * 1024 * 1024; // 10 MB

    // Test 1: Create dynamic VHD
    VhdEngine vhd;
    if (!vhd.Create(testPath, size, true)) {
        printf("FAIL CREATE: %s\n", vhd.GetLastError().c_str());
        return 1;
    }
    printf("PASS: Created %s (%llu bytes, %llu sectors)\n",
           testPath,
           (unsigned long long)vhd.GetSizeBytes(),
           (unsigned long long)vhd.GetTotalSectors());
    vhd.Close();

    // Test 2: Open it back
    if (!vhd.Open(testPath, true)) {
        printf("FAIL OPEN: %s\n", vhd.GetLastError().c_str());
        return 2;
    }
    printf("PASS: Opened %s\n", testPath);

    // Test 3: Write and read back
    std::vector<uint8_t> writeBuf(512, 0xAB);
    if (!vhd.WriteSectors(0, 1, writeBuf.data())) {
        printf("FAIL WRITE: %s\n", vhd.GetLastError().c_str());
        return 3;
    }
    printf("PASS: Wrote 512 bytes of 0xAB to sector 0\n");

    std::vector<uint8_t> readBuf(512, 0);
    if (!vhd.ReadSectors(0, 1, readBuf.data())) {
        printf("FAIL READ: %s\n", vhd.GetLastError().c_str());
        return 4;
    }
    printf("PASS: Read sector 0 back\n");

    if (readBuf != writeBuf) {
        printf("FAIL: DATA MISMATCH\n");
        return 5;
    }
    printf("PASS: Data round-trip matches\n");

    // Test 4: Write to sector 1000 (different block)
    std::vector<uint8_t> writeBuf2(512, 0xCD);
    if (!vhd.WriteSectors(1000, 1, writeBuf2.data())) {
        printf("FAIL WRITE2: %s\n", vhd.GetLastError().c_str());
        return 6;
    }
    printf("PASS: Wrote 0xCD to sector 1000\n");

    std::vector<uint8_t> readBuf2(512, 0);
    if (!vhd.ReadSectors(1000, 1, readBuf2.data())) {
        printf("FAIL READ2: %s\n", vhd.GetLastError().c_str());
        return 7;
    }
    if (readBuf2 != writeBuf2) {
        printf("FAIL: DATA MISMATCH at sector 1000\n");
        return 8;
    }
    printf("PASS: Sector 1000 round-trip matches\n");

    // Test 5: Sector 0 still has 0xAB (persistence across block boundaries)
    std::vector<uint8_t> readBuf3(512, 0);
    vhd.ReadSectors(0, 1, readBuf3.data());
    if (readBuf3 != writeBuf) {
        printf("FAIL: Sector 0 corrupted after writing sector 1000\n");
        return 9;
    }
    printf("PASS: Sector 0 still has 0xAB\n");

    // Test 6: Close, reopen, verify persistence
    vhd.Close();
    VhdEngine vhd2;
    if (!vhd2.Open(testPath, true)) {
        printf("FAIL REOPEN: %s\n", vhd2.GetLastError().c_str());
        return 10;
    }
    std::vector<uint8_t> readBuf4(512, 0);
    vhd2.ReadSectors(0, 1, readBuf4.data());
    if (readBuf4 != writeBuf) {
        printf("FAIL: Data NOT persisted after close/reopen\n");
        return 11;
    }
    printf("PASS: Data persists after close/reopen\n");
    vhd2.Close();

    // Cleanup
    remove(testPath);
    printf("\n=== ALL VHD TESTS PASSED ===\n");
    return 0;
}
