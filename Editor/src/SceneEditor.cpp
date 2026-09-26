#include "SceneEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <numbers>
#include <string>
#include <utility>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        constexpr float DegreesPerRadian = 180.0f / std::numbers::pi_v<float>;

        std::string DisplayName(const Scene& scene, Entity entity) {
            const std::string& name = scene.Get<NameComponent>(entity).name;
            return name.empty() ? "(sans nom)" : name;
        }

        // Nouvelle entité visible : un cube gris.
        Entity CreateCube(Scene& scene, Entity parent) {
            const Entity entity = scene.CreateEntity(parent == NullEntity ? "Cube" : "Enfant", parent);
            scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(180, 180, 185));
            return entity;
        }
    }

    void SceneEditor::Update(Engine& engine) {
        Scene& scene = engine.GetScene();
        Entity selected = m_selection ? scene.FindById(m_selection) : NullEntity;
        DrawHierarchy(engine, scene);
        DrawInspector(engine, scene, selected);
        ApplyAction(engine, scene, selected);
    }

    void SceneEditor::Select(Scene& scene, Entity entity) {
        m_selection = entity == NullEntity ? Uuid{} : scene.GetId(entity);
        for (Entity parent = entity == NullEntity ? NullEntity : scene.GetParent(entity); parent != NullEntity;
             parent = scene.GetParent(parent)) {
            m_reveal.insert(scene.GetId(parent));
        }
    }

    void SceneEditor::DrawHierarchy(Engine& engine, Scene& scene) {
        UI::Context& ui = engine.GetUi();
        const float screenHeight = static_cast<float>(engine.GetWindow().GetSize().y);
        // Sous le panneau « Moteur », jusqu'en bas de la fenêtre.
        if (ui.BeginPanel("Hiérarchie", {8.0f, 350.0f}, 300.0f, std::max(160.0f, screenHeight - 358.0f))) {
            if (ui.Button("Nouveau cube")) {
                m_pendingAction = Action::CreateRoot;
            }
            if (m_selection) {
                ui.SameLine();
                if (ui.Button("Enfant")) {
                    m_pendingAction = Action::CreateChild;
                }
                ui.SameLine();
                if (ui.Button("Supprimer")) {
                    m_pendingAction = Action::Delete;
                }
            }
            ui.Separator();
            if (scene.GetEntityCount() == 0) {
                ui.TextDisabled("Scène vide");
            }
            scene.ForEachRoot([&](Entity root) { DrawNode(ui, scene, root); });
        }
        ui.EndPanel();
    }

    void SceneEditor::DrawNode(UI::Context& ui, Scene& scene, Entity entity) {
        const Uuid id = scene.GetId(entity);
        bool leaf = true;
        scene.ForEachChild(entity, [&](Entity) { leaf = false; });

        if (const auto reveal = m_reveal.find(id); reveal != m_reveal.end()) {
            ui.SetNextTreeNodeOpen(true);
            m_reveal.erase(reveal);
        }
        // Portée d'identifiant par entité : plusieurs « Cube » ne se confondent pas, et l'état déplié suit l'entité.
        ui.PushId(id.value);
        const UI::Context::TreeNodeResult node = ui.TreeNode(DisplayName(scene, entity), id == m_selection, leaf);
        if (node.clicked) {
            m_selection = id;
        }
        if (node.open) {
            scene.ForEachChild(entity, [&](Entity child) { DrawNode(ui, scene, child); });
            ui.TreePop();
        }
        ui.PopId();
    }

    void SceneEditor::DrawInspector(Engine& engine, Scene& scene, Entity selected) {
        UI::Context& ui = engine.GetUi();
        const float screenWidth = static_cast<float>(engine.GetWindow().GetSize().x);
        // Entre la vue et le panneau « Démo » du jeu (en haut à droite).
        if (ui.BeginPanel("Inspecteur", {std::max(316.0f, screenWidth - 752.0f), 8.0f}, 368.0f)) {
            if (selected == NullEntity) {
                ui.TextDisabled("Aucune entité sélectionnée.\nCliquer sur une entité dans la hiérarchie.");
            } else {
                const Uuid id = scene.GetId(selected);
                ui.PushId(id.value); // un champ en cours de saisie ne passe pas d'une entité à l'autre

                ui.InputText("Nom", scene.Get<NameComponent>(selected).name);
                ui.LabelText("Identifiant", id.ToString());
                const Entity parent = scene.GetParent(selected);
                ui.LabelText("Parent", parent == NullEntity ? "(racine)" : DisplayName(scene, parent));

                ui.Separator();
                ui.Text("Transformation");
                if (parent == NullEntity) {
                    // Seules les racines ont un secteur ; secteur 0 = pas de composant (fichier plus lisible).
                    const auto* sectorComponent = scene.TryGet<SectorComponent>(selected);
                    Math::Vector3i64 sector = sectorComponent ? sectorComponent->sector : Math::Vector3i64{};
                    if (ui.DragInt3("Secteur", sector)) {
                        if (sector == Math::Vector3i64{}) {
                            scene.Remove<SectorComponent>(selected);
                        } else {
                            scene.Add<SectorComponent>(selected, sector);
                        }
                    }
                }
                auto& transform = scene.Get<TransformComponent>(selected);
                ui.DragDouble3("Position", transform.position, 0.05);

                if (m_anglesFor != id || !(transform.rotation == m_anglesSource)) {
                    const Math::Vector3f angles = Math::ToYawPitchRoll(transform.rotation);
                    // Affichés par axe : x = tangage, y = lacet, z = roulis.
                    m_anglesDegrees = Math::Vector3f{angles.y, angles.x, angles.z} * DegreesPerRadian;
                    m_anglesFor = id;
                }
                if (ui.DragFloat3("Rotation (°)", m_anglesDegrees, 0.5f, UI::Context::NoMin, UI::Context::NoMax, 1)) {
                    const Math::Vector3f radians = m_anglesDegrees * (1.0f / DegreesPerRadian);
                    transform.rotation = Math::Normalize(Math::Quaternionf::FromYawPitchRoll(radians.y, radians.x, radians.z));
                }
                m_anglesSource = transform.rotation;
                ui.DragFloat3("Échelle", transform.scale, 0.01f, 0.001, 1.0e6);

                ui.Separator();
                if (auto* renderer = scene.TryGet<MeshRendererComponent>(selected)) {
                    ui.Text("Rendu : cube");
                    ui.ColorEdit3("Couleur", renderer->color);
                    if (ui.Button("Retirer le rendu")) {
                        m_pendingAction = Action::RemoveMeshRenderer;
                    }
                } else if (ui.Button("Ajouter un rendu (cube)")) {
                    scene.Add<MeshRendererComponent>(selected);
                }
                ui.PopId();
            }
        }
        ui.EndPanel();
    }

    void SceneEditor::ApplyAction(Engine& engine, Scene& scene, Entity selected) {
        const Action action = std::exchange(m_pendingAction, Action::None);
        switch (action) {
            case Action::None:
                break;
            case Action::CreateRoot: {
                // Cinq mètres devant la caméra, dans son secteur : visible tout de suite, même à une année-lumière.
                const Camera& camera = engine.GetCamera();
                Math::WorldPosition position = camera.position;
                position += Math::Vector3d(camera.GetForward()) * 5.0;
                const Entity entity = CreateCube(scene, NullEntity);
                scene.Get<TransformComponent>(entity).position = position.offset;
                if (position.sector != Math::Vector3i64{}) {
                    scene.Add<SectorComponent>(entity, position.sector);
                }
                Select(scene, entity);
                break;
            }
            case Action::CreateChild:
                if (selected != NullEntity) {
                    const Entity entity = CreateCube(scene, selected);
                    auto& transform = scene.Get<TransformComponent>(entity);
                    transform.position = {1.5, 0.0, 0.0};
                    transform.scale = {0.5f, 0.5f, 0.5f};
                    Select(scene, entity);
                }
                break;
            case Action::Delete:
                if (selected != NullEntity) {
                    scene.DestroyEntity(selected);
                    m_selection = {};
                }
                break;
            case Action::RemoveMeshRenderer:
                if (selected != NullEntity) {
                    scene.Remove<MeshRendererComponent>(selected);
                }
                break;
        }
    }
}
