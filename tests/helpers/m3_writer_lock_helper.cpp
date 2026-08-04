#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "src/common/unique_handle.h"
#include "src/persistence/writer_lock.h"

#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        return 64;
    }

    myrd::UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]));
    myrd::UniqueHandle release(OpenEventW(SYNCHRONIZE, FALSE, argv[3]));
    if (!ready || !release) {
        return 65;
    }

    auto lock = myrd::persistence::acquire_bundle_writer_lock(
        std::filesystem::path(argv[1]), "disk.rdm");
    if (!lock.ok()) {
        return 66;
    }
    if (!SetEvent(ready.get())) {
        return 67;
    }

    const auto wait_result = WaitForSingleObject(release.get(), 20'000);
    if (wait_result != WAIT_OBJECT_0) {
        return 68;
    }
    return 0;
}
