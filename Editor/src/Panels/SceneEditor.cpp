#include "Panels/SceneEditor.hpp"

#include "Document/SceneDocument.hpp"
#include "Document/UndoHistory.hpp"
#include "Panels/ContentBrowser.hpp"
#include "Panels/AssetPicker.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/Context.hpp>
#include <Flux/UI/DrawList.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        constexpr float DegreesPerRadian = 180.0f / std::numbers::pi_v<float>;
        constexpr float DragThreshold = 4.0f; // pixels avant qu'une ligne tenue devienne un glisser-déposer
        constexpr float SceneTilesHeight = 214.0f; // zone des scènes : deux rangées de vignettes

        // ancestor est entity ou l'un de ses ancêtres.
        bool IsSelfOrAncestor(const Scene& scene, Entity ancestor, Entity entity) {
            for (; entity != NullEntity; entity = scene.GetParent(entity)) {
                if (entity == ancestor) {
                    return true;
                }
            }
            return false;
        }

        bool IsPlayer(const Scene& scene, Entity entity) {
            const auto* character = scene.TryGet<CharacterComponent>(entity);
            return character && character->player;
        }

        std::string DisplayName(const Scene& scene, Entity entity) {
            const std::string& name = scene.Get<NameComponent>(entity).name;
            return name.empty() ? "(sans nom)" : name;
        }

        constexpr std::string_view SceneExtension = ".scene.json";

        // Groupes de l'onglet Scènes, dans l'ordre d'affichage : le premier composant trouvé donne le type d'une
        // racine (un Chasseur est un vaisseau avant d'être un modèle 3D, un personnage aussi).
        struct Category
        {
            std::string_view key; // du composant (ComponentInfo::key)
            std::string_view title;
            UI::Icon icon;
        };
        constexpr Category Categories[] = {
            {"ship", "Vaisseaux", UI::Icon::Ship},
            {"character", "Personnages", UI::Icon::Character},
            {"model", "Modèles 3D", UI::Icon::Model},
            {"meshRenderer", "Cubes", UI::Icon::Cube},
            {"light", "Lumières", UI::Icon::Light},
            {"collider", "Collisions", UI::Icon::Collider},
            {"seat", "Sièges", UI::Icon::Seat},
            {"screenText", "Textes à l'écran", UI::Icon::Text},
            {"screenImage", "Images à l'écran", UI::Icon::Image},
            {"sceneChange", "Changements de scène", UI::Icon::SceneChange},
        };
        constexpr std::string_view EmptyTitle = "Objets vides";

        // Groupe d'une racine (et icône de toute entité) : sa catégorie ; sinon le nom de son premier composant (ceux
        // du jeu) ; sinon « Objets vides ». rank ordonne les groupes : catégories, composants du jeu, objets vides.
        struct Kind
        {
            int rank;
            std::string title;
            UI::Icon icon;
        };
        Kind KindOf(const Scene& scene, const ComponentRegistry& components, Entity entity) {
            for (int i = 0; i < static_cast<int>(std::size(Categories)); ++i) {
                const ComponentInfo* info = components.Find(Categories[i].key);
                if (info && info->get(scene.GetRegistry(), entity)) {
                    return {i, std::string(Categories[i].title), Categories[i].icon};
                }
            }
            for (std::size_t i = 0; i < components.GetCount(); ++i) {
                const ComponentInfo& info = components.Get(i);
                if (info.removable && info.get(scene.GetRegistry(), entity)) {
                    return {static_cast<int>(std::size(Categories)), info.label, UI::Icon::Component};
                }
            }
            return {static_cast<int>(std::size(Categories)) + 1, std::string(EmptyTitle), UI::Icon::Empty};
        }

        // Nom affiché d'une scène : son fichier sans « .scene.json ».
        std::string SceneName(const std::filesystem::path& path) {
            std::string name = Platform::ToUtf8(path.filename());
            if (name.ends_with(SceneExtension)) {
                name.resize(name.size() - SceneExtension.size());
            }
            return name;
        }
    }

    void SceneEditor::Update(Engine& engine, UndoHistory& history, SceneDocument& document, ContentBrowser& files) {
        Scene& scene = engine.GetScene();
        // Mode Jouer : la scène du jeu est jetée à l'arrêt, la modifier ne servirait qu'à perdre des changements.
        // On peut toujours sélectionner et regarder les valeurs.
        m_readOnly = engine.GetGameState() != GameState::Stopped;
        // Suppr : l'entité sélectionnée, sauf pendant une saisie (la touche efface alors du texte).
        if (engine.GetWindow().GetInput().WasKeyPressed(Key::Delete) && !engine.GetUi().WantsKeyboard()) {
            RequestDelete();
        }
        Entity selected = m_selection ? scene.FindById(m_selection) : NullEntity;
        DrawScenes(engine, scene, document, files);
        DrawInspector(engine, scene, selected);
        ApplyAction(engine, scene, selected, history);
    }

    void SceneEditor::Select(Scene& scene, Entity entity) {
        m_selection = entity == NullEntity ? Uuid{} : scene.GetId(entity);
        Entity root = entity;
        for (Entity parent = entity == NullEntity ? NullEntity : scene.GetParent(entity); parent != NullEntity;
             parent = scene.GetParent(parent)) {
            m_reveal.insert(scene.GetId(parent));
            root = parent;
        }
        m_revealRoot = root == NullEntity ? Uuid{} : scene.GetId(root);
    }

    void SceneEditor::DrawScenes(Engine& engine, Scene& scene, SceneDocument& document, ContentBrowser& files) {
        UI::Context& ui = engine.GetUi();
        const float screenHeight = static_cast<float>(engine.GetWindow().GetSize().y);
        // Position sans ancrage (l'éditeur l'ancre à gauche, en onglet avec « Placer ») : jusqu'en bas.
        const float top = 350.0f + ui.GetMenuBarHeight();
        if (ui.BeginPanel("Scènes", {8.0f, top}, 300.0f, std::max(160.0f, screenHeight - top - 8.0f))) {
            // Les objets se créent dans l'onglet « Placer ».
            if (m_readOnly) {
                ui.TextDisabled("Mode Jouer : scène non modifiable.");
                ui.Separator();
            }
            const Input& input = engine.GetWindow().GetInput();
            m_mouse = input.GetMousePosition();
            m_hierarchyDrag.target = {};

            // En haut, les fichiers de scène en vignettes (deux rangées visibles) ; en dessous, les objets de la
            // scène ouverte.
            files.DrawSceneBrowser(ui, engine, document, SceneTilesHeight);
            const std::filesystem::path& path = document.GetPath();
            const std::string name = path.empty() ? std::string("nouvelle scène") : SceneName(path);
            ui.Text("Objets de « " + name + " »");
            ui.BeginChild("objets", 0.0f, 0.0f);
            DrawObjects(ui, engine, scene);
            ui.EndChild();

            HierarchyDrag& drag = m_hierarchyDrag;
            if (drag.entity) {
                const Math::Vector2f moved = m_mouse - drag.pressMouse;
                drag.dragging |= std::sqrt(moved.x * moved.x + moved.y * moved.y) > DragThreshold;
                if (!input.IsMouseButtonDown(MouseButton::Left)) {
                    if (drag.dragging && drag.target) {
                        m_pendingAction = Action::Move;
                        m_pendingMove = drag;
                    }
                    drag = {};
                }
            }
        }
        ui.EndPanel();
    }

    // Objets de la scène ouverte : les racines rangées par type, dans l'ordre de la scène à l'intérieur d'un groupe
    // (l'ordre d'affichage de l'interface à l'écran), chacune avec ses enfants en arbre.
    void SceneEditor::DrawObjects(UI::Context& ui, Engine& engine, Scene& scene) {
        if (scene.GetEntityCount() == 0) {
            ui.TextDisabled("Scène vide");
            return;
        }
        const ComponentRegistry& components = engine.GetComponents();
        std::map<std::pair<int, std::string>, std::pair<UI::Icon, std::vector<Entity>>> groups;
        scene.ForEachRoot([&](Entity root) {
            Kind kind = KindOf(scene, components, root);
            auto& group = groups[{kind.rank, std::move(kind.title)}];
            group.first = kind.icon;
            group.second.push_back(root);
        });
        for (const auto& [key, group] : groups) {
            const struct
            {
                const std::string& title;
                UI::Icon icon;
            } category{key.second, group.first};
            const std::vector<Entity>& roots = group.second;
            ui.PushId(category.title);
            // Un groupe se déplie de lui-même quand il contient l'entité à montrer (sélectionnée dans la vue 3D).
            if (m_revealRoot && std::ranges::any_of(roots, [&](Entity root) { return scene.GetId(root) == m_revealRoot; })) {
                ui.SetNextTreeNodeOpen(true);
                m_revealRoot = {};
            }
            const std::string title = category.title + " (" + std::to_string(roots.size()) + ")";
            const bool open = ui.TreeNode(title, false, false, true, category.icon).open;
            // Un enfant glissé sur le titre d'un groupe redevient une racine (il rejoint le groupe de son type).
            HierarchyDrag& drag = m_hierarchyDrag;
            const Entity dragged = drag.dragging ? scene.FindById(drag.entity) : NullEntity;
            if (dragged != NullEntity && scene.GetParent(dragged) != NullEntity && ui.IsMouseOverItem()) {
                drag.target = drag.entity;
                drag.zone = DropZone::Root;
                if (UI::DrawList* list = ui.GetPanelDrawList()) {
                    const UI::Rect row = ui.GetItemRect();
                    list->AddRectOutline(row.min, row.max, ui.GetStyle().checkMark, 2.0f);
                }
            }
            if (open) {
                for (const Entity root : roots) {
                    DrawNode(ui, engine, scene, root);
                }
                ui.TreePop();
            }
            ui.PopId();
        }
    }

    void SceneEditor::DrawNode(UI::Context& ui, Engine& engine, Scene& scene, Entity entity) {
        const Uuid id = scene.GetId(entity);
        bool leaf = true;
        scene.ForEachChild(entity, [&](Entity) { leaf = false; });

        if (const auto reveal = m_reveal.find(id); reveal != m_reveal.end()) {
            ui.SetNextTreeNodeOpen(true);
            m_reveal.erase(reveal);
        }
        // Portée d'identifiant par entité : plusieurs « Cube » ne se confondent pas, et l'état déplié suit l'entité.
        ui.PushId(id.value);
        const UI::Context::TreeNodeResult node = ui.TreeNode(DisplayName(scene, entity), id == m_selection, leaf, false,
                                                             KindOf(scene, engine.GetComponents(), entity).icon);
        if (node.clicked) {
            m_selection = id;
        }
        HierarchyDrag& drag = m_hierarchyDrag;
        if (ui.IsItemActive() && !drag.entity && !m_readOnly) {
            drag = {};
            drag.entity = id;
            drag.pressMouse = m_mouse;
        }
        // Ligne survolée pendant un glissement : haut de la ligne, devant elle ; bas, derrière ; milieu, dedans.
        // Pas sur l'entité glissée ni ses descendants (une entité ne peut pas devenir son propre enfant).
        const Entity dragged = drag.dragging ? scene.FindById(drag.entity) : NullEntity;
        if (dragged != NullEntity && ui.IsMouseOverItem() && !IsSelfOrAncestor(scene, dragged, entity)) {
            const UI::Rect row = ui.GetItemRect();
            const float quarter = (row.max.y - row.min.y) * 0.25f;
            drag.target = id;
            drag.zone = m_mouse.y < row.min.y + quarter   ? DropZone::Before
                      : m_mouse.y >= row.max.y - quarter ? DropZone::After
                                                          : DropZone::Inside;
            if (UI::DrawList* list = ui.GetPanelDrawList()) {
                const Math::LinearColor color = ui.GetStyle().checkMark;
                if (drag.zone == DropZone::Inside) {
                    list->AddRectOutline(row.min, row.max, color, 2.0f);
                } else {
                    const float y = drag.zone == DropZone::Before ? row.min.y : row.max.y;
                    list->AddRect({row.min.x, y - 1.0f}, {row.max.x, y + 1.0f}, color);
                }
            }
        }
        if (node.open) {
            scene.ForEachChild(entity, [&](Entity child) { DrawNode(ui, engine, scene, child); });
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
                ui.TextDisabled("Aucune entité sélectionnée.\nCliquer sur une entité dans l'onglet Scènes.");
            } else {
                const Uuid id = scene.GetId(selected);
                if (m_anglesFor != id) {
                    m_angles.clear();
                    m_anglesFor = id;
                }
                ui.PushId(id.value); // un champ en cours de saisie ne passe pas d'une entité à l'autre
                ui.BeginDisabled(m_readOnly);

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
                ui.EndDisabled();

                // Composants enregistrés (moteur puis jeu) : champs générés à partir de leur description.
                const ComponentRegistry& components = engine.GetComponents();
                m_inspected = selected;
                std::vector<const ComponentInfo*> present;
                std::vector<const ComponentInfo*> missing;
                const bool wasPlayer = IsPlayer(scene, selected);
                for (std::size_t i = 0; i < components.GetCount(); ++i) {
                    const ComponentInfo& info = components.Get(i);
                    if (void* component = info.get(scene.GetRegistry(), selected)) {
                        DrawComponent(engine, info, component);
                        present.push_back(&info);
                    } else if (info.removable) {
                        missing.push_back(&info);
                    }
                }
                // Un seul personnage dirigé par le joueur : cocher la case sur celui-ci la décoche sur les autres.
                if (!wasPlayer && IsPlayer(scene, selected) && !m_readOnly) {
                    m_pendingAction = Action::ClaimPlayer;
                }
                // Mélange venu d'une ancienne scène (l'inspecteur ne le permet plus) : dire lequel est ignoré.
                for (const ComponentInfo* info : present) {
                    for (const ComponentInfo* other : present) {
                        if (info < other && info->Excludes(*other)) {
                            ui.Separator();
                            ui.TextDisabled("« " + info->label + " » et « " + other->label + " » ne vont pas ensemble :\n"
                                            "en retirer un (la physique en ignore un).");
                        }
                    }
                }
                if (const auto* unknown = scene.TryGet<UnknownComponentsComponent>(selected)) {
                    ui.Separator();
                    for (const auto& [key, text] : unknown->components) {
                        ui.TextDisabled("« " + key + " » : type inconnu, conservé tel quel");
                    }
                }

                if (!missing.empty() && !m_readOnly) {
                    ui.Separator();
                    for (const ComponentInfo* info : missing) {
                        const auto blocker = std::ranges::find_if(present, [&](const ComponentInfo* other) {
                            return info->Excludes(*other);
                        });
                        if (blocker != present.end()) {
                            ui.TextDisabled(info->label + " : impossible avec « " + (*blocker)->label + " »");
                        } else if (ui.Button("Ajouter : " + info->label + "##" + info->key)) {
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

    void SceneEditor::DrawComponent(Engine& engine, const ComponentInfo& info, void* component) {
        UI::Context& ui = engine.GetUi();
        ui.PushId(info.key); // deux composants peuvent avoir des champs de même libellé
        if (ui.CollapsingHeader(info.label)) {
            ui.BeginDisabled(m_readOnly); // l'en-tête reste repliable
            for (const FieldInfo& field : info.fields) {
                m_modified |= DrawField(engine, info, field, field.access(component));
            }
            ui.EndDisabled();
            if (info.removable && !m_readOnly && ui.Button("Retirer")) {
                m_pendingAction = Action::RemoveComponent;
                m_pendingComponent = info.key;
            }
        }
        ui.PopId();
    }

    bool SceneEditor::DrawField(Engine& engine, const ComponentInfo& info, const FieldInfo& field, void* value) {
        UI::Context& ui = engine.GetUi();
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
                return field.options.alpha ? ui.ColorEdit4(label, *static_cast<Math::LinearColor*>(value))
                                           : ui.ColorEdit3(label, *static_cast<Math::LinearColor*>(value));
            case FieldType::String: {
                auto& text = *static_cast<std::string*>(value);
                if (field.asset == AssetType::Animation) {
                    return DrawAnimationChoice(engine, label, text);
                }
                if (field.asset == AssetType::None) {
                    return ui.InputText(label, text);
                }
                bool browse = false;
                bool changed = ui.InputTextBrowse(label, text, browse);
                if (browse) {
                    // Boîte de dialogue modale : la frame attend le choix.
                    if (std::optional<std::string> chosen = ChooseAsset(engine, field.asset, text); chosen && *chosen != text) {
                        text = std::move(*chosen);
                        changed = true;
                    }
                }
                return changed;
            }
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

    // Liste des animations du modèle de l'entité : on ne tape plus un nom qui n'existe pas. En tête, « (par défaut) »
    // (champ vide : 1re animation, marche figée au repos, marche accélérée en courant) ; un nom absent du fichier
    // reste affiché, marqué.
    bool SceneEditor::DrawAnimationChoice(Engine& engine, const std::string& label, std::string& text) {
        UI::Context& ui = engine.GetUi();
        const std::vector<std::string> animations = engine.GetAnimationNames(m_inspected);
        if (animations.empty()) {
            ui.LabelText(label, text.empty() ? "(modèle sans animation)" : text + " (modèle sans animation)");
            return false;
        }
        const bool known = text.empty() || std::ranges::contains(animations, text);
        const std::string missing = text + " (introuvable)";
        std::vector<std::string_view> names{"(par défaut)"};
        names.insert(names.end(), animations.begin(), animations.end());
        if (!known) {
            names.push_back(missing);
        }
        int index = 0;
        if (!known) {
            index = static_cast<int>(names.size()) - 1;
        } else if (!text.empty()) {
            index = 1 + static_cast<int>(std::ranges::find(animations, text) - animations.begin());
        }
        if (!ui.Choice(label, index, names) || (!known && index == static_cast<int>(names.size()) - 1)) {
            return false;
        }
        text = index == 0 ? std::string{} : animations[static_cast<std::size_t>(index - 1)];
        return true;
    }

    void SceneEditor::ApplyAction(Engine& engine, Scene& scene, Entity selected, UndoHistory& history) {
        const Action action = std::exchange(m_pendingAction, Action::None);
        if (action == Action::None || m_readOnly
            || (action != Action::Move && selected == NullEntity)) {
            m_pendingComponent.clear();
            return;
        }
        history.Commit(engine); // un geste en cours (champ encore tenu) est un pas à part, avant celui-ci
        switch (action) {
            case Action::None:
                break;
            case Action::Delete: {
                // Avec ses descendants, et sa place parmi ses frères : annuler la remet exactement où elle était.
                const Uuid id = scene.GetId(selected);
                std::string before = UndoHistory::Capture(engine, id, true);
                scene.DestroyEntity(selected);
                m_selection = {};
                history.Push("Supprimer", id, std::move(before), {});
                break;
            }
            case Action::Move: {
                const Entity entity = scene.FindById(m_pendingMove.entity);
                const Entity target = scene.FindById(m_pendingMove.target);
                const bool toRoot = m_pendingMove.zone == DropZone::Root;
                if (entity == NullEntity || target == NullEntity || (entity == target && !toRoot)) {
                    break;
                }
                Entity parent = scene.GetParent(target);
                Entity before = target;
                if (toRoot) {
                    parent = NullEntity;
                    before = NullEntity; // dernière racine : dessinée par-dessus les autres
                } else if (m_pendingMove.zone == DropZone::Inside) {
                    parent = target;
                    before = NullEntity; // dernier enfant : dessiné par-dessus ses frères
                    m_reveal.insert(m_pendingMove.target);
                } else if (m_pendingMove.zone == DropZone::After) {
                    before = scene.GetNextSibling(target);
                    if (before == entity) {
                        before = scene.GetNextSibling(entity); // déjà juste derrière : reste en place
                    }
                }
                const Uuid id = m_pendingMove.entity;
                std::string previous = UndoHistory::Capture(engine, id);
                if (scene.SetParent(entity, parent, before)) {
                    history.Push("Déplacer dans la hiérarchie", id, std::move(previous), UndoHistory::Capture(engine, id));
                }
                break;
            }
            case Action::ClaimPlayer: {
                // La case cochée est déjà un pas (Commit ci-dessus) ; chaque personnage décoché en est un de plus.
                std::vector<Entity> others;
                for (const auto [entity, character] : scene.GetRegistry().view<const CharacterComponent>().each()) {
                    if (character.player && entity != selected) {
                        others.push_back(entity);
                    }
                }
                for (const Entity entity : others) {
                    const Uuid id = scene.GetId(entity);
                    std::string before = UndoHistory::Capture(engine, id);
                    scene.Get<CharacterComponent>(entity).player = false;
                    history.Push("Un seul joueur", id, std::move(before), UndoHistory::Capture(engine, id));
                }
                break;
            }
            case Action::AddComponent:
            case Action::RemoveComponent:
                // Le type a pu disparaître entre-temps (module de jeu rechargé) : on le recherche par sa clé.
                if (const ComponentInfo* info = engine.GetComponents().Find(m_pendingComponent)) {
                    const Uuid id = scene.GetId(selected);
                    std::string before = UndoHistory::Capture(engine, id);
                    if (action == Action::AddComponent) {
                        info->add(scene.GetRegistry(), selected);
                        if (IsPlayer(scene, selected)) {
                            m_pendingAction = Action::ClaimPlayer; // nouveau personnage : joueur par défaut
                        }
                    } else if (info->removable) {
                        info->remove(scene.GetRegistry(), selected);
                    }
                    history.Push(action == Action::AddComponent ? "Ajouter un composant" : "Retirer un composant", id,
                                 std::move(before), UndoHistory::Capture(engine, id));
                }
                m_pendingComponent.clear();
                break;
        }
    }
}
