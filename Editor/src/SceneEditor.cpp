#include "SceneEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
        // Position sans ancrage (l'éditeur l'ancre à gauche) : sous le panneau « Moteur », jusqu'en bas.
        const float top = 350.0f + ui.GetMenuBarHeight();
        if (ui.BeginPanel("Hiérarchie", {8.0f, top}, 300.0f, std::max(160.0f, screenHeight - top - 8.0f))) {
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
        // Position sans ancrage (l'éditeur l'ancre à droite) : entre la vue et le panneau « Démo » du jeu.
        if (ui.BeginPanel("Inspecteur", {std::max(316.0f, screenWidth - 752.0f), 8.0f}, 368.0f)) {
            if (selected == NullEntity) {
                ui.TextDisabled("Aucune entité sélectionnée.\nCliquer sur une entité dans la hiérarchie.");
            } else {
                const Uuid id = scene.GetId(selected);
                if (m_anglesFor != id) {
                    m_angles.clear();
                    m_anglesFor = id;
                }
                ui.PushId(id.value); // un champ en cours de saisie ne passe pas d'une entité à l'autre

                m_modified |= ui.InputText("Nom", scene.Get<NameComponent>(selected).name);
                ui.LabelText("Identifiant", id.ToString());
                const Entity parent = scene.GetParent(selected);
                ui.LabelText("Parent", parent == NullEntity ? "(racine)" : DisplayName(scene, parent));
                if (parent == NullEntity) {
                    // Seules les racines ont un secteur ; secteur 0 = pas de composant (fichier plus lisible).
                    const auto* sectorComponent = scene.TryGet<SectorComponent>(selected);
                    Math::Vector3i64 sector = sectorComponent ? sectorComponent->sector : Math::Vector3i64{};
                    if (ui.DragInt3("Secteur", sector)) {
                        m_modified = true;
                        if (sector == Math::Vector3i64{}) {
                            scene.Remove<SectorComponent>(selected);
                        } else {
                            scene.Add<SectorComponent>(selected, sector);
                        }
                    }
                }

                // Composants enregistrés (moteur puis jeu) : champs générés à partir de leur description.
                const ComponentRegistry& components = engine.GetComponents();
                std::vector<const ComponentInfo*> missing;
                for (std::size_t i = 0; i < components.GetCount(); ++i) {
                    const ComponentInfo& info = components.Get(i);
                    if (void* component = info.get(scene.GetRegistry(), selected)) {
                        DrawComponent(ui, info, component);
                    } else if (info.removable) {
                        missing.push_back(&info);
                    }
                }
                if (const auto* unknown = scene.TryGet<UnknownComponentsComponent>(selected)) {
                    ui.Separator();
                    for (const auto& [key, text] : unknown->components) {
                        ui.TextDisabled("« " + key + " » : type inconnu, conservé tel quel");
                    }
                }

                if (!missing.empty()) {
                    ui.Separator();
                    for (const ComponentInfo* info : missing) {
                        if (ui.Button("Ajouter : " + info->label + "##" + info->key)) {
                            m_pendingAction = Action::AddComponent;
                            m_pendingComponent = info->key;
                        }
                    }
                }
                ui.PopId();
            }
        }
        ui.EndPanel();
    }

    void SceneEditor::DrawComponent(UI::Context& ui, const ComponentInfo& info, void* component) {
        ui.PushId(info.key); // deux composants peuvent avoir des champs de même libellé
        if (ui.CollapsingHeader(info.label)) {
            for (const FieldInfo& field : info.fields) {
                m_modified |= DrawField(ui, info, field, field.access(component));
            }
            if (info.removable && ui.Button("Retirer")) {
                m_pendingAction = Action::RemoveComponent;
                m_pendingComponent = info.key;
            }
        }
        ui.PopId();
    }

    bool SceneEditor::DrawField(UI::Context& ui, const ComponentInfo& info, const FieldInfo& field, void* value) {
        const FieldOptions& o = field.options;
        const std::string label = field.label + "##" + field.key;
        switch (field.type) {
            case FieldType::Bool:
                return ui.Checkbox(label, *static_cast<bool*>(value));
            case FieldType::Int:
                return ui.DragInt(label, *static_cast<std::int32_t*>(value), static_cast<float>(o.speed), o.min, o.max);
            case FieldType::Float:
                return ui.DragFloat(label, *static_cast<float*>(value), static_cast<float>(o.speed), o.min, o.max, o.decimals);
            case FieldType::Double:
                return ui.DragDouble(label, *static_cast<double*>(value), o.speed, o.min, o.max, o.decimals);
            case FieldType::Vector3f:
                return ui.DragFloat3(label, *static_cast<Math::Vector3f*>(value), static_cast<float>(o.speed), o.min, o.max,
                                     o.decimals);
            case FieldType::Vector3d:
                return ui.DragDouble3(label, *static_cast<Math::Vector3d*>(value), o.speed, o.min, o.max, o.decimals);
            case FieldType::Quaternion: {
                auto& rotation = *static_cast<Math::Quaternionf*>(value);
                auto [cached, created] = m_angles.try_emplace(info.key + '/' + field.key);
                Angles& angles = cached->second;
                if (created || !(rotation == angles.source)) {
                    const Math::Vector3f radians = Math::ToYawPitchRoll(rotation);
                    // Affichés par axe : x = tangage, y = lacet, z = roulis.
                    angles.degrees = Math::Vector3f{radians.y, radians.x, radians.z} * DegreesPerRadian;
                }
                const bool changed = ui.DragFloat3(label, angles.degrees, 0.5f, UI::Context::NoMin, UI::Context::NoMax, 1);
                if (changed) {
                    const Math::Vector3f radians = angles.degrees * (1.0f / DegreesPerRadian);
                    rotation = Math::Normalize(Math::Quaternionf::FromYawPitchRoll(radians.y, radians.x, radians.z));
                }
                angles.source = rotation;
                return changed;
            }
            case FieldType::Color:
                return ui.ColorEdit3(label, *static_cast<Math::LinearColor*>(value));
            case FieldType::String:
                return ui.InputText(label, *static_cast<std::string*>(value));
            case FieldType::Enum: {
                std::vector<std::string_view> names;
                int index = -1;
                const std::int64_t current = field.getEnum(value);
                for (const EnumOption& option : field.enumOptions) {
                    if (option.value == current) {
                        index = static_cast<int>(names.size());
                    }
                    names.push_back(option.label);
                }
                if (!ui.Choice(label, index, names)) {
                    return false;
                }
                field.setEnum(value, field.enumOptions[static_cast<std::size_t>(index)].value);
                return true;
            }
        }
        return false;
    }

    void SceneEditor::ApplyAction(Engine& engine, Scene& scene, Entity selected) {
        const Action action = std::exchange(m_pendingAction, Action::None);
        if (action == Action::CreateRoot || (action != Action::None && selected != NullEntity)) {
            m_modified = true;
        }
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
            case Action::AddComponent:
            case Action::RemoveComponent:
                // Le type a pu disparaître entre-temps (module de jeu rechargé) : on le recherche par sa clé.
                if (const ComponentInfo* info = engine.GetComponents().Find(m_pendingComponent); info && selected != NullEntity) {
                    if (action == Action::AddComponent) {
                        info->add(scene.GetRegistry(), selected);
                    } else if (info->removable) {
                        info->remove(scene.GetRegistry(), selected);
                    }
                }
                m_pendingComponent.clear();
                break;
        }
    }
}
