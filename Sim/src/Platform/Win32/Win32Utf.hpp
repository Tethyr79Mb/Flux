#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace Flux::Win32 {
    // Le moteur parle UTF-8 ; les API Win32 en W attendent de l'UTF-16.
    inline std::wstring ToWide(std::string_view utf8) {
        if (utf8.empty()) {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
        return wide;
    }

    inline std::string ToUtf8(std::wstring_view wide) {
        if (wide.empty()) {
            return {};
        }
        const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
        return utf8;
    }

    inline std::string LastErrorMessage() {
        const DWORD error = GetLastError();
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        std::string message = length ? ToUtf8({buffer, length}) : "erreur Win32 " + std::to_string(error);
        LocalFree(buffer);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
        return message;
    }
}
