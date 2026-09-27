#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>

#include <Flux/UI/Context.hpp>

#include "CameraPanel.hpp"
#include "ContentBrowser.hpp"
#include "EditorLayout.hpp"
#include "PlacePanel.hpp"
#include "PlayMode.hpp"
#include "SceneDocument.hpp"
#include "SceneEditor.hpp"
#include "SceneViewport.hpp"
#include "SettingsWindow.hpp"
#include "UndoHistory.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
    // FluxEditor.exe [projet.fluxproject] : sans argument, le projet du dépôt (à défaut, celui copié dans bin/).
    std::filesystem::path ProjectPath(int argc, char** argv) {
        if (argc > 1) {
            return argv[1];
        }
        const std::filesystem::path source(u8"" FLUX_DEFAULT_PROJECT);
        std::error_code error;
        return std::filesystem::exists(source, error) ? source
                                                      : Flux::Platform::GetExecutableDirectory() / "Game.fluxproject";
    }

    // Annule ou refait un pas, puis sélectionne l'entité concernée (ou rien si elle n'existe plus).
    void Undo(Flux::Engine& engine, FluxEditor::UndoHistory& history, FluxEditor::SceneEditor& sceneEditor, bool redo) {
        const Flux::Uuid entity = redo ? history.Redo(engine) : history.Undo(engine);
        Flux::Scene& scene = engine.GetScene();
        sceneEditor.Select(scene, entity ? scene.FindById(entity) : Flux::NullEntity);
    }

    // Menu « Édition ».
    void DrawEditMenu(Flux::Engine& engine, FluxEditor::UndoHistory& history, FluxEditor::SceneEditor& sceneEditor) {
        Flux::UI::Context& ui = engine.GetUi();
        if (!ui.BeginMenu("Édition")) {
            return;
        }
        const bool canUndo = history.IsEnabled() && history.CanUndo();
        const bool canRedo = history.IsEnabled() && history.CanRedo();
        // Libellés à identifiant fixe : l'élément reste le même quand le nom du pas change.
        if (ui.MenuItem((canUndo ? "Annuler : " + history.GetUndoLabel() : std::string("Annuler")) + "##annuler", "Ctrl+Z",
                        canUndo)) {
            Undo(engine, history, sceneEditor, false);
        }
        if (ui.MenuItem((canRedo ? "Rétablir : " + history.GetRedoLabel() : std::string("Rétablir")) + "##retablir",
                        "Ctrl+Y", canRedo)) {
            Undo(engine, history, sceneEditor, true);
        }
        ui.EndMenu();
    }

    // Ctrl+Z annule, Ctrl+Y ou Ctrl+Maj+Z rétablit (par la lettre de la touche : Z reste Z en AZERTY).
    void HandleEditShortcuts(Flux::Engine& engine, FluxEditor::UndoHistory& history, FluxEditor::SceneEditor& sceneEditor) {
        const Flux::Input& input = engine.GetWindow().GetInput();
        // Pendant une saisie, Ctrl+Z appartient au champ : la scène ne doit pas changer sous lui.
        if (!history.IsEnabled() || engine.GetUi().WantsKeyboard() || !input.IsControlDown()) {
            return;
        }
        if (input.WasLetterPressed('Z')) {
            Undo(engine, history, sceneEditor, input.IsShiftDown());
        } else if (input.WasLetterPressed('Y')) {
            Undo(engine, history, sceneEditor, true);
        }
    }
}

int main(int argc, char** argv) {
    // Déclarés avant le moteur, donc détruits après lui : les fonctions qui les référencent ne leur survivent pas.
    FluxEditor::SceneDocument document;
    FluxEditor::SceneEditor sceneEditor;
    FluxEditor::SceneViewport viewport;
    FluxEditor::ContentBrowser contentBrowser;
    FluxEditor::PlayMode playMode;
    FluxEditor::EditorLayout layout;
    FluxEditor::SettingsWindow settings;
    FluxEditor::UndoHistory history;
    FluxEditor::PlacePanel placePanel;
    // Pas de panneau « Moteur » (images/s, GPU...) : l'éditeur a son panneau Caméra et Fichier > Paramètres.
    Flux::Engine engine({.title = "Flux Editor", .windowSize = {1600, 900}, .maximized = true, .showDebugOverlay = false,
                          .drawScreenUi = false}); // la vue dessine l'interface de la scène
    engine.SetGameState(Flux::GameState::Stopped); // on édite : le jeu ne tourne qu'avec « Jouer »
    engine.SetEditorUpdate([&](Flux::Engine& e, double deltaSeconds) {
        // Barre de menus d'abord : « Ouvrir » ou « Arrêter » remplacent la scène avant que les panneaux la décrivent.
        playMode.HandleShortcuts(e);
        document.SetLocked(playMode.IsPlaying());
        document.HandleShortcuts(e);
        HandleEditShortcuts(e, history, sceneEditor);
        Flux::UI::Context& ui = e.GetUi();
        if (ui.BeginMainMenuBar()) {
            if (ui.BeginMenu("Fichier")) {
                document.DrawSceneMenuItems(ui, e);
                ui.Separator();
                if (ui.MenuItem("Paramètres...")) {
                    settings.Open();
                }
                ui.Separator();
                document.DrawQuitMenuItem(ui, e);
                ui.EndMenu();
            }
            DrawEditMenu(e, history, sceneEditor);
            layout.DrawWindowMenu(ui);
            playMode.DrawToolbar(ui, e);
            ui.EndMainMenuBar();
        }
        document.SetLocked(playMode.IsPlaying());
        if (document.TakeReplaced()) {
            history.Clear(); // l'historique parlait de l'ancienne scène
        }
        // Pendant le jeu, les modifications disparaissent à l'arrêt : ni historique, ni fichier concernés.
        if (playMode.IsPlaying() && history.IsEnabled()) {
            history.Commit(e); // la scène du jeu est encore identique à la scène éditée
        }
        history.SetEnabled(!playMode.IsPlaying());
        history.BeginFrame(e, sceneEditor.GetSelection());

        const Flux::UI::Rect view = layout.Update(e);
        placePanel.Update(e, sceneEditor, history, view);
        sceneEditor.Update(e, history);
        viewport.Update(e, sceneEditor, view); // après l'inspecteur : le gizmo suit ses modifications
        contentBrowser.Update(e, document, deltaSeconds);
        FluxEditor::DrawCameraPanel(e);
        settings.Draw(e);

        if (sceneEditor.TakeModified()) {
            history.NoteModified();
        }
        if (viewport.TakeModified()) {
            history.NoteModified("Déplacer");
        }
        history.EndFrame(e, ui.IsInteracting() || viewport.IsDragging());
        if (history.TakeChanged()) {
            document.MarkModified();
        }
        document.UpdateTitle(e);
    });
    engine.SetCloseRequestHandler([&](Flux::Engine& e) {
        playMode.Stop(e); // la question porte sur la scène éditée, pas sur sa copie en jeu
        return document.ConfirmClose(e);
    });

    // Sans projet, l'éditeur démarre quand même, sur une scène vide : on peut toujours ouvrir un fichier.
    auto project = Flux::ProjectFile::Load(ProjectPath(argc, argv));
    if (project) {
        engine.SetProject(*project);
    } else {
        Flux::Platform::ShowError(engine.GetWindow(), "Projet", project.error());
    }

    // Copie fantôme : la DLL d'origine reste libre pour être recompilée pendant que l'éditeur tourne.
    auto game = Flux::GameModule::Load(Flux::Platform::GetExecutableDirectory() / FLUX_GAME_MODULE_FILE,
                                       Flux::GameModule::LoadMode::ShadowCopy);
    if (!game) {
        std::fprintf(stderr, "Module de jeu : %s\n", game.error().c_str());
        return 1;
    }
    game->Register(engine); // composants du jeu d'abord : la scène de démarrage peut les utiliser
    if (project) {
        document.Load(engine, project->GetStartupScenePath());
    }

    const int exitCode = engine.Run();
    layout.Save(engine);
    return exitCode;
}
