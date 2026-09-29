#include "Flux/Platform/Console.hpp"

#include <windows.h>

namespace Flux::Platform {
    void UseUtf8Console() {
        SetConsoleOutputCP(CP_UTF8);
    }
}
