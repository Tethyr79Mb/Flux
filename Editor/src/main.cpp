#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include "SceneDocument.hpp"
#include "SceneEditor.hpp"

#include <cstdio>

int main() {
    // Déclarés avant le moteur, donc détruits après lui : les fonctions qui les référencent ne leur survivent pas.
    FluxEditor::SceneDocument document;
    FluxEditor::SceneEditor sceneEditor;
    Flux::Engine engine({.title = "Flux Editor", .windowSize = {1600, 900}});
    engine.SetEditorUpdate([&](Flux::Engine& e, double) {
        document.Update(e); // barre de menus d'abord : « Ouvrir » remplace la scène avant que les panneaux la décrivent
        sceneEditor.Update(e);
        if (sceneEditor.TakeModified()) {
            document.MarkModified();
        }
    });
    engine.SetCloseRequestHandler([&](Flux::Engine& e) { return document.ConfirmClose(e); });

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
