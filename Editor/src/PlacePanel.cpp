#include "PlacePanel.hpp"

#include "SceneEditor.hpp"
#include "UndoHistory.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/ScreenUi.hpp>
#include <Flux/UI/DrawList.hpp>

#include <cmath>
#include <string>

namespace FluxEditor {
    using namespace Flux;
    using Math::Vector2f;

    namespace {
        constexpr float DragThreshold = 4.0f; // pixels avant qu'un bouton tenu devienne un glisser-déposer
        constexpr double PlaceDistance = 5.0; // mètres devant la caméra (ou le long du rayon de la souris)

        struct ItemInfo
        {
            const char* label;
            const char* name; // nom de l'entité créée
        };

        ItemInfo GetInfo(int item) {
            constexpr ItemInfo Infos[] = {{"Objet vide", "Objet vide"}, {"Cube", "Cube"}, {"Texte à l'écran", "Texte"}};
            return Infos[item];
        }

        bool Contains(const UI::Rect& rect, Vector2f point) {
            return point.x >= rect.min.x && point.x < rect.max.x && point.y >= rect.min.y && point.y < rect.max.y;
        }
    }

    void PlacePanel::Update(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, const UI::Rect& view) {
        UI::Context& ui = engine.GetUi();
        const Input& input = engine.GetWindow().GetInput();
        const Vector2f mouse = input.GetMousePosition();

        std::optional<Item> clicked;
        std::optional<Item> held;
        const auto button = [&](Item item) {
            if (ui.Button(GetInfo(static_cast<int>(item)).label)) {
                clicked = item;
            }
            if (ui.IsItemActive()) {
                held = item;
            }
        };
        // Position sans ancrage (l'éditeur l'ancre à gauche, au-dessus de la hiérarchie).
        if (ui.BeginPanel("Placer", {8.0f, 8.0f + ui.GetMenuBarHeight()}, 300.0f)) {
            ui.TextDisabled("Formes");
            button(Item::Empty);
            ui.SameLine();
            button(Item::Cube);
            ui.Separator();
            ui.TextDisabled("Interface");
            button(Item::ScreenText);
            ui.Separator();
            ui.TextDisabled("Clic : devant la caméra.\nGlisser dans la vue : à cet endroit.");
        }
        ui.EndPanel();

        if (held) {
            if (m_held != held) {
                m_held = held;
                m_pressMouse = mouse;
            }
            const Vector2f moved = mouse - m_pressMouse;
            m_dragging |= std::sqrt(moved.x * moved.x + moved.y * moved.y) > DragThreshold;
        }
        // Relâché sur le bouton sans l'avoir quitté : un clic. Après un glissement, relâché sur le bouton : abandon.
        if (clicked && !m_dragging) {
            Place(engine, sceneEditor, history, *clicked, std::nullopt, view);
        }
        if (m_dragging && m_held && !input.IsMouseButtonDown(MouseButton::Left)) {
            // Lâché dans la vue, pas sur un panneau : l'objet est posé là. Ailleurs, rien.
            if (Contains(view, mouse) && !ui.WantsMouse()) {
                Place(engine, sceneEditor, history, *m_held, mouse, view);
            }
            m_dragging = false;
            m_held.reset();
        }
        if (!held && !m_dragging) {
            m_held.reset();
        }

        // Pendant le glissement, le nom de l'objet suit la souris (sous les panneaux, visible dans la vue).
        if (m_dragging && m_held) {
            UI::DrawList& list = engine.GetRenderer().GetOverlay();
            const UI::Style& style = ui.GetStyle();
            const std::string label = GetInfo(static_cast<int>(*m_held)).label;
            const Vector2f topLeft = mouse + Vector2f{14.0f, 10.0f};
            const Vector2f size = ui.MeasureText(label) + style.framePadding * 2.0f;
            list.AddRect(topLeft, topLeft + size, style.button);
            list.AddText(topLeft + style.framePadding, label, style.fontSize, style.text);
        }
    }

    void PlacePanel::Place(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, Item item,
                           std::optional<Vector2f> drop, const UI::Rect& view) {
        history.Commit(engine); // un geste en cours est un pas à part, avant celui-ci
        Scene& scene = engine.GetScene();
        const ItemInfo info = GetInfo(static_cast<int>(item));
        const Entity entity = scene.CreateEntity(info.name);

        if (item == Item::ScreenText) {
            // Accroché au centre ; lâché dans la vue, décalé jusqu'au point de dépôt (en pixels de référence).
            auto& text = scene.Add<ScreenTextComponent>(entity);
            if (drop) {
                const float scale = ScreenUi::GetScale(view.min, view.max);
                const Vector2f moved = *drop - ScreenUi::AnchorPoint(ScreenAnchor::Center, view.min, view.max);
                if (scale > 0.0f) {
                    text.offsetX = std::round(moved.x / scale);
                    text.offsetY = std::round(moved.y / scale);
                }
            }
        } else {
            if (item == Item::Cube) {
                scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(180, 180, 185));
            }
            // Devant la caméra, ou le long du rayon qui passe sous la souris ; dans le secteur de la caméra.
            const Camera& camera = engine.GetCamera();
            const Math::Vector3d direction = drop ? ScreenProjection(camera, view.min, view.max).GetRay(*drop).direction
                                                  : Math::Vector3d(camera.GetForward());
            Math::WorldPosition position = camera.position;
            position += direction * PlaceDistance;
            scene.Get<TransformComponent>(entity).position = position.offset;
            if (position.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, position.sector);
            }
        }

        sceneEditor.Select(scene, entity);
        history.Push(std::string("Placer : ") + info.label, scene.GetId(entity), {}, UndoHistory::Capture(engine, scene.GetId(entity)));
    }
}
