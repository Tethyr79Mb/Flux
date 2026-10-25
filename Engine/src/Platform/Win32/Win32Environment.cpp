#include "Flux/Platform/Environment.hpp"

#include "Platform/Win32/Win32Utf.hpp"

#include <windows.h>

namespace Flux::Platform {
    std::optional<std::string> ReadEnvironment(std::string_view name) {
        const std::wstring wideName = Win32::ToWide(name);
        const DWORD length = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0); // avec le zéro final
        if (length == 0) {
            return std::nullopt;
        }
        std::wstring value(length, L'\0');
        const DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), length);
        value.resize(written < length ? written : 0);
        return Win32::ToUtf8(value);
    }
}
