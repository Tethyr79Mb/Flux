#include "Panels/SceneEditor.hpp"

#include "Panels/Hardpoints.hpp"

#include "Document/SceneDocument.hpp"
#include "Document/UndoHistory.hpp"
#include "Panels/AssetPicker.hpp"
#include "Panels/ContentBrowser.hpp"
#include "Panels/Hardpoints.hpp"
#include "Search.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>
#include <Flux/UI/DrawList.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
            {"sound", "Sons", UI::Icon::Sound},
            {"sky", "Ciel", UI::Icon::Sky},
            {"world", "Monde", UI::Icon::Planet},
            {"sun", "Soleil", UI::Icon::Light},
            {"planet", "Planètes", UI::Icon::Planet},
            {"interior", "Volumes intérieurs", UI::Icon::Collider},
            {"collider", "Collisions", UI::Icon::Collider},
            {"seat", "Sièges", UI::Icon::Seat},
            {"interface", "Interfaces", UI::Icon::Window},
            {"screenText", "Textes à l'écran", UI::Icon::Text},
            {"screenImage", "Images à l'écran", UI::Icon::Image},
            {"sceneChange", "Changements de scène", UI::Icon::SceneChange},
        };
        constexpr std::string_view EmptyTitle = "Objets vides";

        // Pictogramme d'un composant (titre de sa section dans l'inspecteur, menu « Ajouter un composant »).
        UI::Icon ComponentIcon(std::string_view key) {
            for (const Category& category : Categories) {
                if (category.key == key) {
                    return category.icon;
                }
            }
            if (key == "animator") {
                return UI::Icon::Character;
            }
            if (key == "transform") {
                return UI::Icon::None;
            }
            return UI::Icon::Component;
        }

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
        // En dernier : ouvrir le prefab remplace la scène.
        if (!m_openPrefab.empty()) {
            if (const Project* project = engine.GetProject()) {
                document.OpenFile(engine, project->assetsDirectory / Platform::PathFromUtf8(m_openPrefab));
            }
            m_openPrefab.clear();
        }
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
        ui.SetNextPanelIcon(UI::Icon::Scene);
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
            ui.SearchField("recherche", m_search, "Rechercher un objet...");
            ui.BeginChild("objets", 0.0f, 0.0f);
            if (m_search.empty()) {
                DrawObjects(ui, engine, scene);
            } else {
                DrawSearchResults(ui, engine, scene);
            }
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

    // Objets dont le nom contient la recherche, de toute la hiérarchie, dans l'ordre de l'arbre ; chacun suivi du chemin
    // de ses parents. Clic : sélectionner ; double-clic : le montrer dans l'arbre (la recherche s'efface).
    void SceneEditor::DrawSearchResults(UI::Context& ui, Engine& engine, Scene& scene) {
        const SearchQuery query(m_search);
        const ComponentRegistry& components = engine.GetComponents();
        Entity reveal = NullEntity;
        bool any = false;
        const auto visit = [&](const auto& self, Entity entity, const std::string& path) -> void {
            const std::string name = DisplayName(scene, entity);
            if (query.Matches(name)) {
                any = true;
                const Uuid id = scene.GetId(entity);
                ui.PushId(id.value);
                const std::string label = path.empty() ? name : name + "   (" + path + ")";
                const UI::Context::TreeNodeResult node =
                    ui.TreeNode(label, id == m_selection, true, false, KindOf(scene, components, entity).icon);
                if (node.clicked) {
                    m_selection = id;
                }
                if (node.doubleClicked) {
                    reveal = entity;
                }
                ui.SetItemTooltip("Double-clic : montrer dans la scène");
                ui.PopId();
            }
            const std::string childPath = path.empty() ? name : path + " / " + name;
            scene.ForEachChild(entity, [&](Entity child) { self(self, child, childPath); });
        };
        scene.ForEachRoot([&](Entity root) { visit(visit, root, {}); });
        if (!any) {
            ui.TextDisabled("Aucun objet ne s'appelle ainsi.");
        }
        if (reveal != NullEntity) {
            m_search.clear();
            Select(scene, reveal);
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
        ui.SetNextPanelIcon(UI::Icon::Inspector);
        if (ui.BeginPanel("Inspecteur", {std::max(316.0f, screenWidth - 752.0f), 8.0f}, 368.0f)) {
            m_overrides = {};
            if (selected == NullEntity) {
                ui.TextDisabled("Aucune entité sélectionnée.\nCliquer sur une entité dans l'onglet Scènes.");
            } else {
                // Pièce d'un exemplaire de prefab : ce qui diffère du prefab, relu à chaque image (le texte du prefab
                // est gardé en mémoire tant que son fichier ne change pas). Pas en jeu : rien ne s'y modifie.
                if (!m_readOnly) {
                    m_overrides = SceneSerializer::GetPrefabOverrides(scene, engine.GetComponents(), selected,
                                                                      engine.GetPrefabReader());
                    // La place de l'exemplaire est la sienne, pas une modification du prefab.
                    if (scene.TryGet<PrefabInstanceComponent>(selected)) {
                        std::erase(m_overrides.fields, std::pair<std::string, std::string>{"transform", "position"});
                    }
                }
                const Uuid id = scene.GetId(selected);
                if (m_anglesFor != id) {
                    m_angles.clear();
                    m_anglesFor = id;
                }
                ui.PushId(id.value); // un champ en cours de saisie ne passe pas d'une entité à l'autre
                ui.BeginDisabled(m_readOnly);

                m_modified |= ui.InputText(m_overrides.name ? "» Nom###nom" : "Nom###nom", scene.Get<NameComponent>(selected).name);
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
                DrawPrefab(engine, scene, selected);

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
                    // Un seul bouton, et la liste des composants qui manquent dans un menu.
                    if (ui.Button("Ajouter un composant", UI::Icon::Plus, true)) {
                        ui.OpenPopupMenu("ajouter");
                    }
                    if (ui.BeginPopupMenu("ajouter")) {
                        for (const ComponentInfo* info : missing) {
                            const auto blocker = std::ranges::find_if(present, [&](const ComponentInfo* other) {
                                return info->Excludes(*other);
                            });
                            if (blocker != present.end()) {
                                ui.MenuItem(info->label + " (pas avec « " + (*blocker)->label + " »)##" + info->key, {}, false,
                                            ComponentIcon(info->key));
                            } else if (ui.MenuItem(info->label + "##" + info->key, {}, true, ComponentIcon(info->key))) {
                                m_pendingAction = Action::AddComponent;
                                m_pendingComponent = info->key;
                            }
                        }
                        ui.EndMenu();
                    }
                }
                ui.PopId();
            }
        }
        ui.EndPanel();
    }

    void SceneEditor::DrawPrefab(Engine& engine, Scene& scene, Entity selected) {
        UI::Context& ui = engine.GetUi();
        const ComponentRegistry& components = engine.GetComponents();
        const auto* link = scene.TryGet<PrefabLinkComponent>(selected);
        if (const auto* instance = scene.TryGet<PrefabInstanceComponent>(selected)) {
            ui.LabelText("Prefab", instance->prefab);
            if (!m_readOnly && !m_overrides.known) {
                ui.TextDisabled("Prefab illisible : les pièces gardent leurs réglages.");
            }
            if (ui.Button("Ouvrir le prefab")) {
                m_openPrefab = instance->prefab;
            }
            if (!m_readOnly) {
                ui.SameLine();
                if (ui.Button("Délier")) {
                    m_pendingAction = Action::UnlinkPrefab;
                }
            }
        } else if (link && scene.IsValid(link->instance)) {
            ui.LabelText("Prefab", "pièce de « " + DisplayName(scene, link->instance) + " »");
        } else if (!m_readOnly && engine.GetProject()) {
            // Un prefab ne contient pas encore d'autre prefab : bouton absent si un exemplaire est dessous.
            bool linked = false;
            const std::function<void(Entity)> check = [&](Entity entity) {
                linked = linked || scene.TryGet<PrefabInstanceComponent>(entity) || scene.TryGet<PrefabLinkComponent>(entity);
                scene.ForEachChild(entity, check);
            };
            check(selected);
            if (!linked && ui.Button("Créer un prefab", UI::Icon::Prefab, true)) {
                m_pendingAction = Action::CreatePrefab;
            }
        }
        // Composants du prefab retirés de cette pièce.
        for (const std::string& key : m_overrides.removed) {
            const ComponentInfo* info = components.Find(key);
            ui.TextDisabled("« " + (info ? info->label : key) + " » retiré (présent dans le prefab)");
            ui.SameLine();
            ui.PushId(key);
            if (ui.Button("Remettre")) {
                m_pendingAction = Action::RevertComponent;
                m_pendingComponent = key;
            }
            ui.PopId();
        }
    }

    bool SceneEditor::RefuseMemberChange(Engine& engine, const Scene& scene, Entity entity, std::string_view what) {
        const auto* link = scene.TryGet<PrefabLinkComponent>(entity);
        if (!link || link->instance == entity || !scene.IsValid(link->instance)) {
            return false;
        }
        Platform::ShowError(engine.GetWindow(), "Prefab",
                            "« " + DisplayName(scene, entity) + " » est une pièce du prefab « " + DisplayName(scene, link->instance)
                                + " » : on ne peut pas la " + std::string(what)
                                + " seule.\n\nModifier le prefab (« Ouvrir le prefab » sur sa racine), ou délier "
                                  "l'exemplaire pour en faire des objets ordinaires.");
        return true;
    }

    void SceneEditor::DrawComponent(Engine& engine, const ComponentInfo& info, void* component) {
        UI::Context& ui = engine.GetUi();
        ui.PushId(info.key); // deux composants peuvent avoir des champs de même libellé
        // Pièce de prefab : section marquée si elle diffère du prefab (identifiant fixe : elle reste dépliée).
        const bool added = std::ranges::contains(m_overrides.added, info.key);
        const bool changed = std::ranges::any_of(m_overrides.fields, [&](const auto& field) { return field.first == info.key; });
        const std::string header = info.label + (added ? " (ajouté)" : changed ? " (modifié)" : "") + "###en-tête";
        // Croix au bout du titre pour retirer le composant (pas la transformation, ni pendant le jeu).
        bool remove = false;
        if (ui.CollapsingHeader(header, true, ComponentIcon(info.key), info.removable && !m_readOnly ? &remove : nullptr)) {
            if (changed && !m_readOnly && ui.Button("Revenir au prefab")) {
                m_pendingAction = Action::RevertComponent;
                m_pendingComponent = info.key;
            }
            ui.BeginDisabled(m_readOnly); // l'en-tête reste repliable
            for (const FieldInfo& field : info.fields) {
                m_modified |= DrawField(engine, info, field, field.access(component));
            }
            ui.EndDisabled();
        }
        if (remove) {
            m_pendingAction = Action::RemoveComponent;
            m_pendingComponent = info.key;
        }
        ui.PopId();
    }

    bool SceneEditor::DrawField(Engine& engine, const ComponentInfo& info, const FieldInfo& field, void* value) {
        UI::Context& ui = engine.GetUi();
        const FieldOptions& o = field.options;
        // « » » devant un champ qui diffère du prefab ; identifié par sa clé seule, il ne change pas de widget en
        // devenant modifié au milieu d'un glissement.
        const bool overridden = std::ranges::contains(m_overrides.fields, std::pair<std::string, std::string>{info.key, field.key});
        const std::string label = (overridden ? "» " : "") + field.label + "###" + field.key;
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
                if (RefuseMemberChange(engine, scene, selected, "supprimer")) {
                    break;
                }
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
                if (entity == NullEntity || target == NullEntity || (entity == target && !toRoot)
                    || RefuseMemberChange(engine, scene, entity, "déplacer dans la hiérarchie")) {
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
                // Une arme lâchée sur un emport s'y fixe (aimant), s'il l'accepte.
                const bool mounting = parent != NullEntity && scene.TryGet<WeaponComponent>(entity)
                                   && scene.TryGet<HardpointComponent>(parent);
                if (mounting) {
                    if (const std::string refusal = Hardpoints::GetRefusal(scene, entity, parent); !refusal.empty()) {
                        Platform::ShowError(engine.GetWindow(), "Emport",
                                            "« " + DisplayName(scene, entity) + " » ne se fixe pas sur « "
                                                + DisplayName(scene, parent) + " » : " + refusal + ".");
                        break;
                    }
                }
                const Uuid id = m_pendingMove.entity;
                std::string previous = UndoHistory::Capture(engine, id);
                if (scene.SetParent(entity, parent, before)) {
                    if (mounting) {
                        Hardpoints::Mount(scene, entity, parent);
                    }
                    history.Push(mounting ? "Fixer sur l'emport" : "Déplacer dans la hiérarchie", id, std::move(previous),
                                 UndoHistory::Capture(engine, id));
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
            case Action::CreatePrefab: {
                // Fichier choisi dans les assets (Prefabs/ par défaut), puis l'entité en devient le premier exemplaire.
                const Project* project = engine.GetProject();
                if (!project) {
                    break;
                }
                const std::filesystem::path directory = project->assetsDirectory / "Prefabs";
                std::error_code error;
                std::filesystem::create_directories(directory, error);
                const auto path = Platform::ShowSaveFileDialog(engine.GetWindow(), "Créer un prefab",
                                                               {"Prefabs Flux", ".prefab.json"}, directory,
                                                               DisplayName(scene, selected) + ".prefab.json");
                if (!path) {
                    break;
                }
                const std::filesystem::path relative = path->lexically_normal().lexically_relative(project->assetsDirectory);
                if (relative.empty() || *relative.begin() == "..") {
                    Platform::ShowError(engine.GetWindow(), "Créer un prefab", "Le prefab doit être dans le dossier des assets.");
                    break;
                }
                const auto text = SceneSerializer::SavePrefab(scene, engine.GetComponents(), selected,
                                                              SceneSerializer::PrefabIds::ForInstance);
                if (!text) {
                    Platform::ShowError(engine.GetWindow(), "Créer un prefab", text.error());
                    break;
                }
                if (auto written = SceneSerializer::WriteFile(*path, *text); !written) {
                    Platform::ShowError(engine.GetWindow(), "Créer un prefab", written.error());
                    break;
                }
                // Annuler délie l'entité ; le fichier reste (à supprimer dans Contenu s'il ne sert plus).
                const Uuid id = scene.GetId(selected);
                std::string before = UndoHistory::Capture(engine, id, true);
                SceneSerializer::LinkPrefab(scene, selected, Platform::ToGenericUtf8(relative));
                history.Push("Créer un prefab", id, std::move(before), UndoHistory::Capture(engine, id, true));
                Log::Info("prefab créé : {}", Platform::ToGenericUtf8(relative));
                break;
            }
            case Action::UnlinkPrefab: {
                const Uuid id = scene.GetId(selected);
                std::string before = UndoHistory::Capture(engine, id, true);
                SceneSerializer::UnlinkPrefab(scene, selected);
                history.Push("Délier du prefab", id, std::move(before), UndoHistory::Capture(engine, id, true));
                break;
            }
            case Action::RevertComponent: {
                const Uuid id = scene.GetId(selected);
                std::string before = UndoHistory::Capture(engine, id);
                // La racine d'un exemplaire garde sa place : seules sa rotation et son échelle reviennent au prefab.
                const auto* transform = scene.TryGet<TransformComponent>(selected);
                const std::optional<Math::Vector3d> place = transform && scene.TryGet<PrefabInstanceComponent>(selected)
                                                                ? std::optional(transform->position)
                                                                : std::nullopt;
                if (SceneSerializer::RevertToPrefab(scene, engine.GetComponents(), selected, m_pendingComponent,
                                                    engine.GetPrefabReader())) {
                    if (place) {
                        scene.Get<TransformComponent>(selected).position = *place;
                    }
                    history.Push("Revenir au prefab", id, std::move(before), UndoHistory::Capture(engine, id));
                }
                m_pendingComponent.clear();
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
