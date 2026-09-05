#include "src/management/windows_service.h"

int wmain(int argc, wchar_t** argv) {
    return myrd::management::windows::run_service_program(argc, argv);
}
