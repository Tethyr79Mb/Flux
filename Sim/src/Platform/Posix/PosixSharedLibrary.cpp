#include "Flux/Platform/SharedLibrary.hpp"

#include <dlfcn.h>

#include <utility>

namespace Flux {
    std::expected<SharedLibrary, std::string> SharedLibrary::Load(const std::filesystem::path& path) {
        // RTLD_LOCAL : les symboles d'un module de jeu ne remplacent pas ceux d'un autre au rechargement.
        void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            const char* reason = dlerror();
            return std::unexpected("chargement de " + path.string() + " : " + (reason ? reason : "raison inconnue"));
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
        return dlsym(m_handle, name);
    }

    void SharedLibrary::Unload() {
        if (m_handle) {
            dlclose(m_handle);
            m_handle = nullptr;
        }
    }
}
