#include "Flux/Core/GameModule.hpp"

#include "Flux/Core/Engine.hpp"

#include <atomic>
#include <format>
#include <system_error>
#include <utility>

namespace Flux {
    namespace {
        std::expected<std::filesystem::path, std::string> MakeShadowCopy(const std::filesystem::path& source) {
            static std::atomic<unsigned> counter{0};

            const std::filesystem::path directory = source.parent_path() / ".hotreload";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                return std::unexpected(std::format("création de {} : {}", directory.string(), error.message()));
            }

            const std::filesystem::path copy = directory /
                std::format("{}-{}{}", source.stem().string(), counter++, source.extension().string());
            std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                return std::unexpected(std::format("copie de {} : {}", source.string(), error.message()));
            }
            return copy;
        }
    }

    std::expected<GameModule, std::string> GameModule::Load(const std::filesystem::path& path, LoadMode mode) {
        GameModule module;

        std::filesystem::path pathToLoad = path;
        if (mode == LoadMode::ShadowCopy) {
            auto copy = MakeShadowCopy(path);
            if (!copy) {
                return std::unexpected(copy.error());
            }
            module.m_shadowCopy = *copy;
            pathToLoad = *copy;
        }

        auto library = SharedLibrary::Load(pathToLoad);
        if (!library) {
            return std::unexpected(library.error());
        }
        module.m_library = std::move(*library);

        const auto apiVersion = module.m_library.GetFunction<GameApiVersionFn>("FluxGameApiVersion");
        module.m_register = module.m_library.GetFunction<RegisterGameFn>("FluxRegisterGame");
        module.m_unregister = module.m_library.GetFunction<UnregisterGameFn>("FluxUnregisterGame");
        if (!apiVersion || !module.m_register || !module.m_unregister) {
            return std::unexpected(std::format("{} n'exporte pas les points d'entrée d'un module de jeu", path.string()));
        }
        if (apiVersion() != GameModuleApiVersion) {
            return std::unexpected(std::format("{} vise l'API de jeu {}, le moteur fournit la {}",
                                               path.string(), apiVersion(), GameModuleApiVersion));
        }
        return module;
    }

    GameModule::~GameModule() {
        Reset();
    }

    void GameModule::Reset() {
        Unregister();
        m_library.Unload();
        if (!m_shadowCopy.empty()) {
            std::error_code ignored;
            std::filesystem::remove(m_shadowCopy, ignored);
            m_shadowCopy.clear();
        }
        m_register = nullptr;
        m_unregister = nullptr;
    }

    GameModule::GameModule(GameModule&& other) noexcept
        : m_library(std::move(other.m_library)),
          m_shadowCopy(std::exchange(other.m_shadowCopy, {})),
          m_register(std::exchange(other.m_register, nullptr)),
          m_unregister(std::exchange(other.m_unregister, nullptr)),
          m_registeredTo(std::exchange(other.m_registeredTo, nullptr)),
          m_engineComponentCount(std::exchange(other.m_engineComponentCount, 0)) {}

    GameModule& GameModule::operator=(GameModule&& other) noexcept {
        if (this != &other) {
            Reset();
            m_library = std::move(other.m_library);
            m_shadowCopy = std::exchange(other.m_shadowCopy, {});
            m_register = std::exchange(other.m_register, nullptr);
            m_unregister = std::exchange(other.m_unregister, nullptr);
            m_registeredTo = std::exchange(other.m_registeredTo, nullptr);
            m_engineComponentCount = std::exchange(other.m_engineComponentCount, 0);
        }
        return *this;
    }

    void GameModule::Register(Engine& engine) {
        if (m_register && !m_registeredTo) {
            m_engineComponentCount = engine.GetComponents().GetCount();
            m_register(engine);
            m_registeredTo = &engine;
        }
    }

    void GameModule::Unregister() {
        if (m_unregister && m_registeredTo) {
            m_unregister(*m_registeredTo);
            m_registeredTo->ClearInterfaceFunctions(); // leur code est dans la DLL, même si le jeu a oublié de les retirer
            m_registeredTo->ClearGameSystems();
            // Les stockages EnTT des composants du jeu ont leur code dans la DLL : ils doivent disparaître avant elle.
            m_registeredTo->GetScene().Clear();
            m_registeredTo->GetComponents().Truncate(m_engineComponentCount);
            m_registeredTo = nullptr;
        }
    }
}
