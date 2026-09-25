#pragma once

#include "Flux/Export.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace Flux {
    class FLUX_API SharedLibrary {
        public:
        static std::expected<SharedLibrary, std::string> Load(const std::filesystem::path& path);

        SharedLibrary() = default;
        ~SharedLibrary();

        SharedLibrary(const SharedLibrary&) = delete;
        SharedLibrary& operator=(const SharedLibrary&) = delete;
        SharedLibrary(SharedLibrary&& other) noexcept;
        SharedLibrary& operator=(SharedLibrary&& other) noexcept;

        void* GetSymbol(const char* name) const;

        template <typename Fn>
        Fn GetFunction(const char* name) const { return reinterpret_cast<Fn>(GetSymbol(name)); }

        bool IsLoaded() const { return m_handle != nullptr; }
        void Unload();

        private:
        void* m_handle = nullptr;
    };
}
