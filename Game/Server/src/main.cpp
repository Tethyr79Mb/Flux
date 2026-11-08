#include "GameServer.hpp"

#include <Flux/Core/Log.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Console.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

// Serveur de jeu sans rendu : une zone d'Ars Solaris simulée à 60 pas par seconde, jusqu'à Ctrl+C.
//     ArsSolarisServer [--projet Game.fluxproject] [--scene scenes/Demo.scene.json] [--pas 600]
namespace {
    constexpr std::string_view Usage = "ArsSolarisServer [--projet fichier.fluxproject] [--scene scène relative aux assets] "
                                       "[--pas nombre (simule ce nombre de pas sans attendre, puis s'arrête)]";

    struct Options
    {
        std::filesystem::path project;
        std::filesystem::path scene;
        std::optional<std::uint64_t> steps;
    };

    std::optional<Options> ReadOptions(int argc, char** argv) {
        Options options;
        options.project = Flux::Platform::GetExecutableDirectory() / "Game.fluxproject";
        for (int i = 1; i < argc; ++i) {
            const std::string_view name = argv[i];
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            // Arguments dans la page de code du système sous Windows, en UTF-8 sous Linux : path les lit ainsi.
            const char* value = argv[++i];
            if (name == "--projet") {
                options.project = value;
            } else if (name == "--scene") {
                options.scene = value;
            } else if (name == "--pas") {
                const std::string_view text = value;
                std::uint64_t steps = 0;
                if (std::from_chars(text.data(), text.data() + text.size(), steps).ec != std::errc{}) {
                    return std::nullopt;
                }
                options.steps = steps;
            } else {
                return std::nullopt;
            }
        }
        return options;
    }
}

int main(int argc, char** argv) {
    using namespace Flux;
    Platform::UseUtf8Console();
    Platform::CatchConsoleClose();
#ifdef _WIN32
    // Sans ça, Windows ne réveille un fil qu'au tic de 15,6 ms : les pas de 16,7 ms arriveraient deux par deux.
    timeBeginPeriod(1);
#endif

    const std::optional<Options> options = ReadOptions(argc, argv);
    if (!options) {
        Log::Error("Usage : {}", Usage);
        return 2;
    }
    auto project = ProjectFile::Load(options->project);
    if (!project) {
        Log::Error("Projet {} : {}", Platform::ToUtf8(options->project), project.error());
        return 1;
    }

    int result = 0;
    {
        Game::GameServer server;
        if (auto opened = server.Open(std::move(*project), options->scene); !opened) {
            Log::Error("{}", opened.error());
            result = 1;
        } else if (options->steps) {
            server.RunSteps(*options->steps);
            Log::Info("{} pas simulés", *options->steps);
        } else {
            Log::Info("Serveur lancé (Ctrl+C pour l'arrêter)");
            server.Run([] { return Platform::ConsoleCloseRequested(); });
            Log::Info("Arrêt du serveur");
        }
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    Platform::ConsoleCloseHandled();
    return result;
}
