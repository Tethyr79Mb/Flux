#include "Panels/TextTools.hpp"

#include "Document/UndoHistory.hpp"
#include "Panels/AssetPicker.hpp"
#include "Panels/FontList.hpp"
#include "Panels/SceneEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/UI/Context.hpp>

#include <array>
#include <format>
#include <optional>
#include <string>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        // Tailles proposées, en pixels d'un écran de 1080 lignes : du petit texte d'interface au grand titre.
        // Une autre valeur se tape dans l'inspecteur.
        constexpr std::array<float, 13> Sizes{12.0f, 16.0f, 20.0f, 24.0f, 32.0f, 40.0f, 48.0f, 64.0f, 80.0f, 96.0f, 128.0f,
                                              160.0f, 200.0f};
    }

    void TextTools::DrawToolBarItems(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, bool readOnly) {
        UI::Context& ui = engine.GetUi();
        Scene& scene = engine.GetScene();
        const Entity selected = scene.FindById(sceneEditor.GetSelection());
        ScreenTextComponent* text = selected != NullEntity ? scene.TryGet<ScreenTextComponent>(selected) : nullptr;
        const ScreenTextComponent& shown = text ? *text : m_next;
        const char* target = text ? " du texte sélectionné" : " du prochain texte";

        // Le changement s'applique au texte sélectionné (un pas d'annulation), sinon au prochain texte.
        const auto apply = [&](const char* label, auto&& change) {
            if (!text) {
                change(m_next);
                return;
            }
            history.Commit(engine); // un geste en cours dans l'inspecteur est un pas à part
            const Uuid id = scene.GetId(selected);
            std::string before = UndoHistory::Capture(engine, id);
            change(*text);
            history.Push(label, id, std::move(before), UndoHistory::Capture(engine, id));
        };

        ui.BeginDisabled(readOnly);
        if (ui.MenuBarButton("##placer un texte", false, UI::Icon::Text)) {
            Place(engine, sceneEditor, history);
        }
        ui.SetItemTooltip("Poser un texte au centre de l'écran");

        if (ui.MenuBarButton(GetFontLabel(shown.font) + "##police")) {
            ui.OpenPopupMenu("polices");
        }
        ui.SetItemTooltip(std::string("Police") + target);
        if (ui.BeginPopupMenu("polices")) {
            std::optional<std::string> chosen;
            if (ui.MenuItem("Arimo (par défaut)")) {
                chosen = std::string{};
            }
            for (const std::string& font : GetProjectFonts(engine)) {
                ui.PushId(font);
                if (ui.MenuItem(GetFontLabel(font))) {
                    chosen = font;
                }
                ui.PopId();
            }
            ui.Separator();
            if (ui.MenuItem("Ajouter une police...")) {
                chosen = ChooseAsset(engine, AssetType::Font, shown.font); // copiée dans Polices/ si elle vient d'ailleurs
            }
            ui.EndMenu();
            if (chosen && *chosen != shown.font) {
                apply("Police du texte", [&](ScreenTextComponent& changed) { changed.font = *chosen; });
            }
        }

        if (ui.MenuBarButton(std::format("{:g}##taille", shown.size))) {
            ui.OpenPopupMenu("tailles");
        }
        ui.SetItemTooltip(std::string("Taille") + target + " (pixels d'un écran de 1080 lignes)");
        if (ui.BeginPopupMenu("tailles")) {
            std::optional<float> chosen;
            for (const float size : Sizes) {
                if (ui.MenuItem(std::format("{:g}", size))) {
                    chosen = size;
                }
            }
            ui.EndMenu();
            if (chosen && *chosen != shown.size) {
                apply("Taille du texte", [&](ScreenTextComponent& changed) { changed.size = *chosen; });
            }
        }

        Math::LinearColor color = shown.color;
        if (ui.MenuBarColorButton("##couleur du texte", color)) {
            apply("Couleur du texte", [&](ScreenTextComponent& changed) { changed.color = color; });
        }
        ui.SetItemTooltip(std::string("Couleur") + target);
        ui.EndDisabled();
    }

    void TextTools::Place(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history) {
        history.Commit(engine); // un geste en cours est un pas à part, avant celui-ci
        Scene& scene = engine.GetScene();
        // Accroché au centre, sans décalage : on le glisse ensuite dans la vue.
        const Entity entity = scene.CreateEntity("Texte");
        scene.Add<ScreenTextComponent>(entity, m_next);
        sceneEditor.Select(scene, entity);
        history.Push("Placer : Texte à l'écran", scene.GetId(entity), {}, UndoHistory::Capture(engine, scene.GetId(entity)));
    }
}
