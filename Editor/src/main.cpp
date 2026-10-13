#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Console.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/Scene.hpp>

#include <Flux/UI/Context.hpp>

#include "Document/PlayMode.hpp"
#include "Document/SceneDocument.hpp"
#include "Document/UndoHistory.hpp"
#include "EditorLayout.hpp"
#include "Interface/InterfaceEditor.hpp"
#include "Panels/CameraPanel.hpp"
#include "Panels/ConsolePanel.hpp"
#include "Panels/ContentBrowser.hpp"
#include "Panels/PlacePanel.hpp"
#include "Panels/SceneEditor.hpp"
#include "Panels/SceneViewport.hpp"
#include "Panels/SettingsWindow.hpp"

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

    // Menu « Édition » : celui de l'interface affichée, ou de la scène.
    void DrawEditMenu(Flux::Engine& engine, FluxEditor::UndoHistory& history, FluxEditor::SceneEditor& sceneEditor,
                      FluxEditor::InterfaceEditor& interfaceEditor) {
        Flux::UI::Context& ui = engine.GetUi();
        if (!ui.BeginMenu("Édition")) {
            return;
        }
        if (interfaceEditor.IsActive()) {
            interfaceEditor.DrawEditMenuItems(ui);
            ui.EndMenu();
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
        ui.Separator();
        if (ui.MenuItem("Supprimer", "Suppr", sceneEditor.CanDelete())) {
            sceneEditor.RequestDelete();
        }
        ui.EndMenu();
    }

    // Barre d'outils : fichier et annuler / rétablir à gauche (de l'interface affichée, ou de la scène), Jouer et Pause
    // au milieu, Paramètres à droite.
    void DrawToolBar(Flux::Engine& engine, FluxEditor::SceneDocument& document, FluxEditor::UndoHistory& history,
                     FluxEditor::SceneEditor& sceneEditor, FluxEditor::InterfaceEditor& interfaceEditor,
                     FluxEditor::PlayMode& playMode, FluxEditor::SettingsWindow& settings) {
        Flux::UI::Context& ui = engine.GetUi();
        if (!ui.BeginToolBar()) {
            return;
        }
        if (interfaceEditor.IsActive()) {
            interfaceEditor.DrawToolBarItems(ui, engine);
        } else {
            document.DrawToolBarItems(ui, engine);
            ui.ToolBarSeparator();
            const bool canUndo = history.IsEnabled() && history.CanUndo();
            const bool canRedo = history.IsEnabled() && history.CanRedo();
            ui.BeginDisabled(!canUndo);
            if (ui.MenuBarButton("##annuler", false, Flux::UI::Icon::Undo)) {
                Undo(engine, history, sceneEditor, false);
            }
            ui.SetItemTooltip(canUndo ? "Annuler : " + history.GetUndoLabel() + " (Ctrl+Z)" : std::string("Annuler (Ctrl+Z)"));
            ui.EndDisabled();
            ui.BeginDisabled(!canRedo);
            if (ui.MenuBarButton("##rétablir", false, Flux::UI::Icon::Redo)) {
                Undo(engine, history, sceneEditor, true);
            }
            ui.SetItemTooltip(canRedo ? "Rétablir : " + history.GetRedoLabel() + " (Ctrl+Y)" : std::string("Rétablir (Ctrl+Y)"));
            ui.EndDisabled();
        }
        playMode.DrawToolbar(ui, engine);
        const float right = static_cast<float>(engine.GetWindow().GetSize().x) - ui.GetStyle().panelPadding * 0.5f;
        ui.SetMenuBarCursor(right - ui.GetBarItemHeight());
        if (ui.MenuBarButton("##paramètres", false, Flux::UI::Icon::Settings)) {
            settings.Open();
        }
        ui.SetItemTooltip("Paramètres de l'éditeur");
        ui.EndToolBar();
    }

    // Droite de la barre d'état : mode, scène ouverte, nombre d'objets, sélection.
    std::string StatusDetails(Flux::Engine& engine, const FluxEditor::SceneDocument& document,
                              const FluxEditor::SceneEditor& sceneEditor, bool playing) {
        Flux::Scene& scene = engine.GetScene();
        const std::filesystem::path& path = document.GetPath();
        std::string details = playing ? "En jeu   ·   " : "";
        details += path.empty() ? std::string("Nouvelle scène") : Flux::Platform::ToUtf8(path.filename());
        details += "   ·   " + std::to_string(scene.GetEntityCount()) + " objets";
        if (const Flux::Entity selected = scene.FindById(sceneEditor.GetSelection()); selected != Flux::NullEntity) {
            details += "   ·   Sélection : " + scene.Get<Flux::NameComponent>(selected).name;
        }
        return details;
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
    Flux::Platform::UseUtf8Console();

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
    FluxEditor::InterfaceEditor interfaceEditor;
    FluxEditor::ConsolePanel console;
    // Pas de panneau « Moteur » (images/s, GPU...) : l'éditeur a son panneau Caméra et Fichier > Paramètres.
    Flux::Engine engine({.title = "Flux Editor", .windowSize = {1600, 900}, .maximized = true, .showDebugOverlay = false,
                          .drawScreenUi = false}); // la vue dessine l'interface de la scène
    engine.SetGameState(Flux::GameState::Stopped); // on édite : le jeu ne tourne qu'avec « Jouer »
    settings.Load(engine); // thème de l'interface choisi la dernière fois
    contentBrowser.SetInterfaceEditor(&interfaceEditor);
    engine.SetEditorUpdate([&](Flux::Engine& e, double deltaSeconds) {
        // Barre de menus d'abord : « Ouvrir » ou « Arrêter » remplacent la scène avant que les panneaux la décrivent.
        playMode.HandleShortcuts(e);
        if (playMode.IsPlaying()) {
            interfaceEditor.ShowScene(); // le jeu se joue dans la vue de la scène
        }
        document.SetLocked(playMode.IsPlaying());
        // Raccourcis de l'interface affichée, sinon ceux de la scène.
        if (interfaceEditor.IsActive()) {
            interfaceEditor.HandleShortcuts(e);
        } else {
            document.HandleShortcuts(e);
            HandleEditShortcuts(e, history, sceneEditor);
        }
        Flux::UI::Context& ui = e.GetUi();
        if (ui.BeginMainMenuBar()) {
            if (ui.BeginMenu("Fichier")) {
                if (interfaceEditor.IsActive()) {
                    interfaceEditor.DrawFileMenuItems(ui, e);
                    ui.Separator();
                }
                document.DrawSceneMenuItems(ui, e);
                ui.Separator();
                if (ui.MenuItem("Paramètres...")) {
                    settings.Open();
                }
                ui.Separator();
                document.DrawQuitMenuItem(ui, e);
                ui.EndMenu();
            }
            DrawEditMenu(e, history, sceneEditor, interfaceEditor);
            layout.DrawWindowMenu(ui);
            ui.EndMainMenuBar();
        }
        DrawToolBar(e, document, history, sceneEditor, interfaceEditor, playMode, settings);
        console.DrawStatusBar(e, StatusDetails(e, document, sceneEditor, playMode.IsPlaying()));
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

        // Onglets au-dessus de la vue : la scène, ou une interface ouverte (qui remplace alors la vue 3D).
        const Flux::UI::Rect view = interfaceEditor.DrawTabs(e, layout.Update(e));
        const bool editingInterface = interfaceEditor.IsActive();
        if (!editingInterface) {
            placePanel.Update(e, sceneEditor, history, view, contentBrowser.GetPrefabs()); // « Placer » pose dans la vue 3D
        }
        sceneEditor.Update(e, history, document, contentBrowser);
        if (!editingInterface) {
            viewport.Update(e, sceneEditor, view); // après l'inspecteur : le gizmo suit ses modifications
        }
        interfaceEditor.Update(e, view);
        contentBrowser.Update(e, document, deltaSeconds);
        FluxEditor::DrawCameraPanel(e);
        console.Update(e);
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
    // Bouton « Quitter le jeu » d'une interface : dans l'éditeur, on revient à la scène éditée.
    engine.SetGameQuitHandler([&](Flux::Engine& e) { playMode.Stop(e); });
    engine.SetCloseRequestHandler([&](Flux::Engine& e) {
        playMode.Stop(e); // la question porte sur la scène éditée, pas sur sa copie en jeu
        return document.ConfirmClose(e) && interfaceEditor.ConfirmClose(e);
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
