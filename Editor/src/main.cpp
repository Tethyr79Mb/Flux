#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include <cstdio>

int main() {
    Flux::Engine engine({.title = "Flux Editor", .windowSize = {1600, 900}});

    // Copie fantôme : la DLL d'origine reste libre pour être recompilée pendant que l'éditeur tourne.
    auto game = Flux::GameModule::Load(Flux::Platform::GetExecutableDirectory() / FLUX_GAME_MODULE_FILE,
                                       Flux::GameModule::LoadMode::ShadowCopy);
    if (!game) {
        std::fprintf(stderr, "Module de jeu : %s\n", game.error().c_str());
        return 1;
    }
    game->Register(engine);

    return engine.Run();
}
