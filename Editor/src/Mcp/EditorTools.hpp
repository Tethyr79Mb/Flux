#pragma once

#include <Flux/UI/Context.hpp>

#include <filesystem>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    class InterfaceEditor;
    class McpServer;
    class SceneDocument;
    class SceneEditor;
    class UndoHistory;

    // Ce que les outils de l'éditeur touchent. Les champs du bas sont remis à jour à chaque image par main.cpp.
    struct EditorToolContext
    {
        Flux::Engine& engine;
        SceneEditor& sceneEditor;
        UndoHistory& history;
        Flux::UI::Rect view{};                                // vue 3D, en pixels de la fenêtre
        bool dropToGround = false;                            // bouton « Poser au sol » de la vue
        bool playing = false;                                 // mode Jouer : rien ne se modifie
        const std::vector<std::filesystem::path>* prefabs = nullptr; // ContentBrowser::GetPrefabs
        SceneDocument* document = nullptr;                    // fichier de la scène (open_scene)
    };

    // Outils de l'éditeur pour Claude : mesures du panneau Moteur, objets de la scène (lire, modifier, créer, dupliquer,
    // supprimer, sélectionner, annuler), composants, caméra, panneau Placer, capture d'écran, ouverture d'une scène,
    // vues de débogage du rendu.
    void RegisterEditorTools(McpServer& server, EditorToolContext& context);
    // Interfaces (.fluxui) dans l'éditeur d'interface : lister, ouvrir, lire, ajouter, régler, déplacer, supprimer
    // (annulable), enregistrer (InterfaceTools.cpp).
    void RegisterInterfaceTools(McpServer& server, EditorToolContext& context, InterfaceEditor& editor);
}
