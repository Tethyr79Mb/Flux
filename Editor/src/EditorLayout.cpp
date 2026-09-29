#include "EditorLayout.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Graphics/Renderer.hpp>
#include <Flux/UI/Context.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace FluxEditor {
    using namespace Flux;

    void EditorLayout::Load(UI::Context& ui) {
        m_loaded = true;
        std::ifstream file(FilePath(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (!file.is_open() || !ui.LoadDockLayout(text)) {
            BuildDefault(ui); // premier lancement, ou fichier abîmé
        }
    }

    void EditorLayout::Save(Engine& engine) const {
        if (!m_loaded) {
            return; // l'éditeur n'a pas démarré : rien à retenir, et l'ancien fichier reste
        }
        std::ofstream file(FilePath(), std::ios::binary | std::ios::trunc);
        file << engine.GetUi().SaveDockLayout();
        if (!file) {
            std::fprintf(stderr, "Disposition de l'éditeur non enregistrée\n");
        }
    }

    void EditorLayout::DrawWindowMenu(UI::Context& ui) {
        if (ui.BeginMenu("Fenêtre")) {
            if (ui.MenuItem("Réinitialiser la disposition")) {
                BuildDefault(ui);
            }
            ui.EndMenu();
        }
    }

    UI::Rect EditorLayout::Update(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        if (!m_loaded) {
            Load(ui);
        }
        const Math::Vector2i size = engine.GetWindow().GetSize();
        const UI::Rect view = ui.DockSpace({0.0f, ui.GetMenuBarHeight()},
                                           {static_cast<float>(size.x), static_cast<float>(size.y)});
        engine.GetRenderer().SetSceneViewport(view.min, view.max);
        return view;
    }

    // Placer et hiérarchie à gauche, inspecteur à droite, contenu et caméra en bas, la vue 3D au centre.
    // Les panneaux du jeu (« Démo ») et les Paramètres flottent dans la vue.
    void EditorLayout::BuildDefault(UI::Context& ui) {
        const UI::DockId root = ui.ClearDockLayout();
        const auto [left, rest] = ui.SplitDock(root, UI::DockSide::Left, 0.2f);
        const auto [right, middle] = ui.SplitDock(rest, UI::DockSide::Right, 0.3f);
        const UI::DockId bottom = ui.SplitDock(middle, UI::DockSide::Bottom, 0.3f).side; // le reste : la vue 3D
        const auto [place, hierarchy] = ui.SplitDock(left, UI::DockSide::Top, 0.35f);
        ui.DockPanel("Placer", place);
        ui.DockPanel("Hiérarchie", hierarchy);
        ui.DockPanel("Inspecteur", right);
        ui.DockPanel("Contenu", bottom);
        ui.DockPanel("Caméra", bottom);
    }

    std::filesystem::path EditorLayout::FilePath() {
        // Numéro changé quand la disposition par défaut gagne un panneau (v2 : « Placer ») : l'ancienne n'est plus
        // relue, sinon le nouveau panneau flotterait au milieu de la vue.
        return Platform::GetExecutableDirectory() / "FluxEditor.layout.v2.json";
    }
}
