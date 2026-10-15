#pragma once

#include "Flux/Export.hpp"
#include "Flux/Platform/SharedLibrary.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace Flux {
    class Engine;

    // À incrémenter à chaque changement incompatible du contrat entre moteur et jeu.
    inline constexpr std::uint32_t GameModuleApiVersion = 28;

    // Points d'entrée qu'une DLL de jeu doit exporter (voir FLUX_GAME_MODULE_EXPORT).
    using GameApiVersionFn = std::uint32_t (*)();
    using RegisterGameFn   = void (*)(Engine&);
    using UnregisterGameFn = void (*)(Engine&);

    class FLUX_API GameModule {
        public:
        enum class LoadMode {
            Direct,     // charge le fichier tel quel (launcher)
            ShadowCopy, // charge une copie pour pouvoir recompiler l'original (éditeur, hot-reload)
        };

        static std::expected<GameModule, std::string> Load(const std::filesystem::path& path, LoadMode mode);

        GameModule() = default;
        ~GameModule();

        GameModule(const GameModule&) = delete;
        GameModule& operator=(const GameModule&) = delete;
        GameModule(GameModule&& other) noexcept;
        GameModule& operator=(GameModule&& other) noexcept;

        void Register(Engine& engine);
        // Appelé aussi par le destructeur : plus aucun objet du jeu ne doit survivre ensuite.
        // Vide la scène et retire les composants enregistrés par le jeu : leur code est dans la DLL.
        void Unregister();

        private:
        void Reset();

        SharedLibrary m_library;
        std::filesystem::path m_shadowCopy;
        RegisterGameFn m_register = nullptr;
        UnregisterGameFn m_unregister = nullptr;
        Engine* m_registeredTo = nullptr;
        std::size_t m_engineComponentCount = 0; // types de composants enregistrés avant le jeu
    };
}

#if defined(_WIN32)
    #define FLUX_GAME_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
    #define FLUX_GAME_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif
