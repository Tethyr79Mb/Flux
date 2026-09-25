#include "Flux/Platform/Filesystem.hpp"

#include <windows.h>

#include <string>

namespace Flux::Platform {
    std::filesystem::path GetExecutableDirectory() {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return std::filesystem::current_path();
            }
            if (length < buffer.size()) {
                buffer.resize(length);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    }
}
