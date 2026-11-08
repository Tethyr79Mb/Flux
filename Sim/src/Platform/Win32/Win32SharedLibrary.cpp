#include "Flux/Platform/SharedLibrary.hpp"

#include "Platform/Win32/Win32Utf.hpp"

#include <utility>

namespace Flux {
    std::expected<SharedLibrary, std::string> SharedLibrary::Load(const std::filesystem::path& path) {
        HMODULE module = LoadLibraryW(path.c_str());
        if (!module) {
            return std::unexpected("chargement de " + path.string() + " : " + Win32::LastErrorMessage());
        }
        SharedLibrary library;
        library.m_handle = module;
        return library;
    }

    SharedLibrary::~SharedLibrary() {
        Unload();
    }

    SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)) {}

    SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
        if (this != &other) {
            Unload();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    void* SharedLibrary::GetSymbol(const char* name) const {
        if (!m_handle) {
            return nullptr;
        }
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
    }

    void SharedLibrary::Unload() {
        if (m_handle) {
            FreeLibrary(static_cast<HMODULE>(m_handle));
            m_handle = nullptr;
        }
    }
}
