#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include "SceneEditor.hpp"

#include <cstdio>

int main() {
    // Déclaré avant le moteur, donc détruit après lui : la fonction d'outils qui le référence ne lui survit pas.
    FluxEditor::SceneEditor sceneEditor;
    Flux::Engine engine({.title = "Flux Editor", .windowSize = {1600, 900}});
    engine.SetEditorUpdate([&](Flux::Engine& e, double) { sceneEditor.Update(e); });

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
