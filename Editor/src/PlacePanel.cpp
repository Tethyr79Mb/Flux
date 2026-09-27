#include "PlacePanel.hpp"

#include "AssetPicker.hpp"
#include "SceneEditor.hpp"
#include "UndoHistory.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Filesystem.hpp>
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
            constexpr ItemInfo Infos[] = {{"Objet vide", "Objet vide"}, {"Cube", "Cube"}, {"Modèle 3D...", "Modèle"},
                                           {"Texte à l'écran", "Texte"},
                                           {"Image à l'écran", "Image"},
                                           {"Changement de scène", "Changement de scène"},
                                           {"Personnage...", "Joueur"}};
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
        // Mode Jouer : rien à placer, la scène du jeu est jetée à l'arrêt (boutons grisés et inertes).
        const bool playing = engine.GetGameState() != GameState::Stopped;
        if (ui.BeginPanel("Placer", {8.0f, 8.0f + ui.GetMenuBarHeight()}, 300.0f)) {
            if (playing) {
                ui.TextDisabled("Mode Jouer : arrêter le jeu pour placer.");
            }
            ui.BeginDisabled(playing);
            ui.TextDisabled("Formes");
            button(Item::Empty);
            ui.SameLine();
            button(Item::Cube);
            button(Item::Model);
            ui.Separator();
            ui.TextDisabled("Interface");
            button(Item::ScreenText);
            ui.SameLine();
            button(Item::ScreenImage);
            ui.Separator();
            ui.TextDisabled("Logique");
            button(Item::SceneChange);
            ui.Separator();
            ui.TextDisabled("Physique (en mode Jouer)");
            button(Item::Character);
            ui.Separator();
            ui.TextDisabled("Clic : devant la caméra.\nGlisser dans la vue : à cet endroit.");
            ui.EndDisabled();
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
        // Un modèle sans fichier ne montrerait rien : le fichier d'abord (copié dans le projet s'il vient d'ailleurs),
        // et l'entité porte son nom.
        std::string name = info.name;
        std::optional<std::string> model;
        if (item == Item::Model || item == Item::Character) {
            model = ChooseAsset(engine, AssetType::Model, "");
            if (!model) {
                return;
            }
            std::string stem = Platform::ToUtf8(Platform::PathFromUtf8(*model).stem());
            if (!stem.empty() && item == Item::Model) {
                name = std::move(stem);
            }
        }
        const Entity entity = scene.CreateEntity(name);

        if (item == Item::ScreenText || item == Item::ScreenImage) {
            // Accroché au centre ; lâché dans la vue, décalé jusqu'au point de dépôt (en pixels de référence).
            // Une image est créée sans fichier : cadre gris dans la vue, à choisir dans l'inspecteur (« ... »).
            if (item == Item::ScreenText) {
                scene.Add<ScreenTextComponent>(entity);
            } else {
                scene.Add<ScreenImageComponent>(entity);
            }
            const float scale = ScreenUi::GetScale(view.min, view.max);
            if (const std::optional<ScreenUi::Offset> offset = ScreenUi::FindOffset(scene, entity); offset && drop && scale > 0.0f) {
                const Vector2f moved = *drop - ScreenUi::AnchorPoint(ScreenAnchor::Center, view.min, view.max);
                *offset->x = std::round(moved.x / scale);
                *offset->y = std::round(moved.y / scale);
            }
        } else if (item == Item::SceneChange) {
            // Rien à voir ni à placer : se règle dans l'inspecteur (scène suivante, délai).
            scene.Add<SceneChangeComponent>(entity);
        } else {
            if (item == Item::Cube) {
                // Solide dès sa création (immobile) : on marche dessus. « Simuler la physique » (inspecteur, Collision) le fait tomber.
                scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(180, 180, 185));
                scene.Add<ColliderComponent>(entity);
            } else if (item == Item::Model) {
                scene.Add<ModelComponent>(entity).model = std::move(*model);
            } else if (item == Item::Character) {
                // Les pieds du modèle (son origine) posés sur celle de l'entité, comme la capsule.
                scene.Add<ModelComponent>(entity).model = std::move(*model);
                scene.Add<CharacterComponent>(entity);
                scene.Add<AnimatorComponent>(entity); // sans effet si le modèle n'a pas d'animation
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
        if (item == Item::Character) {
            sceneEditor.RequestClaimPlayer(); // le nouveau personnage devient le seul dirigé par le joueur
        }
    }
}
