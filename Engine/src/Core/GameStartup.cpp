#include "Flux/Core/GameStartup.hpp"

#include "Flux/Audio/AudioSystem.hpp"
#include "Flux/Core/Engine.hpp"
#include "Flux/Core/GameModule.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Core/Project.hpp"
#include "Flux/Platform/Console.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"

#include <utility>

namespace Flux {
    GameStartup::GameStartup(std::filesystem::path moduleFile) : m_moduleFile(std::move(moduleFile)) {}

    int GameStartup::Run() {
        Platform::UseUtf8Console();
        const std::filesystem::path directory = Platform::GetExecutableDirectory();

        // Le projet est copié à côté de l'exe par le build : ses chemins relatifs pointent vers bin/assets.
        auto project = ProjectFile::Load(directory / "Game.fluxproject");
        if (!project) {
            Log::Error("Projet : {}", project.error());
            return 1;
        }

        // Un fichier de réglages abîmé ne bloque pas le jeu : valeurs par défaut, et il sera réécrit à la fermeture.
        const std::filesystem::path settingsPath = GameSettingsFile::GetPath();
        GameSettings settings;
        if (auto loaded = GameSettingsFile::Load(settingsPath)) {
            settings = *loaded;
        } else {
            Log::Warning("Réglages : {} ; valeurs par défaut", loaded.error());
        }

        Engine engine({.title = project->name,
                       .windowSize = settings.windowSize,
                       .maximized = settings.maximized,
                       .showDebugOverlay = settings.showEngineStats,
                       .freeCamera = false,
                       .vsync = settings.vsync,
                       .volume = settings.volume});
        engine.SetProject(*project);

        int result = 0;
        {
            // Détruit avant le moteur : le jeu se désenregistre d'un moteur encore entier.
            auto game = GameModule::Load(directory / m_moduleFile, GameModule::LoadMode::Direct);
            if (!game) {
                Log::Error("Module de jeu : {}", game.error());
                return 1;
            }
            game->Register(engine); // enregistre les composants du jeu : la scène de démarrage peut les utiliser

            if (auto loaded = engine.LoadScene(project->GetStartupScenePath()); !loaded) {
                Log::Error("Scène de démarrage : {}", loaded.error());
                return 1;
            }
            result = engine.Run();
        }

        // Fenêtre encore ouverte (le moteur ne la ferme qu'en se détruisant) : sa taille et son état sont lisibles. Pas
        // si le moteur n'a pas démarré (fenêtre ou GPU indisponibles) : il n'y a rien à relire.
        if (result != 0) {
            return result;
        }
        if (auto saved = GameSettingsFile::Save(ReadBack(engine, settings), settingsPath); !saved) {
            Log::Warning("Réglages : {}", saved.error());
        }
        return result;
    }

    GameSettings GameStartup::ReadBack(Engine& engine, GameSettings settings) {
        const Window& window = engine.GetWindow();
        if (window.IsOpen()) {
            const Math::Vector2i size = window.GetRestoredSize();
            if (size.x > 0 && size.y > 0) {
                settings.windowSize = size; // réduite (taille 0) : on garde la précédente
            }
            settings.maximized = window.IsMaximized();
        }
        settings.vsync = engine.GetRenderer().IsVSyncEnabled();
        settings.volume = engine.GetAudio().GetMasterVolume();
        return settings;
    }
}
