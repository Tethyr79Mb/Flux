#include "Panels/PlacePanel.hpp"

#include "Document/UndoHistory.hpp"
#include "Panels/AssetPicker.hpp"
#include "Panels/GroundSnap.hpp"
#include "Panels/SceneEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Orbits.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/Scene/ScreenUi.hpp>
#include <Flux/UI/DrawList.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>

namespace FluxEditor {
    using namespace Flux;
    using Math::Vector2f;

    namespace {
        constexpr float DragThreshold = 4.0f; // pixels avant qu'un bouton tenu devienne un glisser-déposer
        constexpr double PlaceDistance = 5.0; // mètres devant la caméra (ou le long du rayon de la souris)
        constexpr double ShipDistance = 25.0; // la maquette fait 16 m de long : la voir en entier
        constexpr double PrefabShipDistance = 32.0; // un prefab de vaisseau, vu comme le chasseur (20 m de long, 15 d'envergure)
        constexpr double PlanetDistance = 3.0;   // rayons de la planète, de la caméra à son centre
        constexpr double SunDistance = 1.496e11; // mètres : de la Terre au Soleil, orbite d'une nouvelle planète

        // Maquette de vaisseau en cubes, dans son repère (-z : l'avant, le pont à y = 0) : une cabine de 6 x 3 x 16 m
        // ouverte à l'arrière, avec une console à l'avant. De quoi essayer la marche à bord avant le vrai modèle.
        void AddShipBlockout(Scene& scene, Entity ship) {
            struct Part
            {
                const char* name;
                Math::Vector3d position;
                Math::Vector3f scale;
                std::uint8_t grey;
            };
            constexpr Part Parts[] = {{"Pont", {0.0, -0.1, 0.0}, {6.4f, 0.2f, 16.0f}, 90},
                                      {"Plafond", {0.0, 3.1, 0.0}, {6.4f, 0.2f, 16.0f}, 120},
                                      {"Paroi bâbord", {-3.1, 1.5, 0.0}, {0.2f, 3.0f, 16.0f}, 150},
                                      {"Paroi tribord", {3.1, 1.5, 0.0}, {0.2f, 3.0f, 16.0f}, 150},
                                      {"Cloison avant", {0.0, 1.5, -8.1}, {6.4f, 3.0f, 0.2f}, 130},
                                      {"Console", {0.0, 0.5, -7.2}, {3.0f, 1.0f, 0.8f}, 60}};
            const auto addPart = [&](const Part& part, Entity parent) {
                const Entity entity = scene.CreateEntity(part.name, parent);
                TransformComponent& transform = scene.Get<TransformComponent>(entity);
                transform.position = part.position;
                transform.scale = part.scale;
                scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(part.grey, part.grey, part.grey + 8));
                scene.Add<ColliderComponent>(entity);
            };
            for (const Part& part : Parts) {
                addPart(part, ship);
            }
            // Poste de pilotage face à la console : l'origine du siège est au sol, sous les hanches du pilote assis.
            // Assise courte devant, pour que le personnage qui se relève (un demi-mètre devant) ne la touche pas.
            const Entity seat = scene.CreateEntity("Poste de pilotage", ship);
            scene.Get<TransformComponent>(seat).position = {0.0, 0.0, -5.9};
            scene.Add<SeatComponent>(seat).pilot = true;
            addPart({"Assise", {0.0, 0.43, 0.02}, {0.6f, 0.1f, 0.45f}, 70}, seat);
            addPart({"Dossier", {0.0, 0.9, 0.32}, {0.6f, 0.8f, 0.1f}, 70}, seat);
        }

        struct ItemInfo
        {
            const char* label;
            const char* name; // nom de l'entité créée
            UI::Icon icon;
        };

        ItemInfo GetInfo(int item) {
            constexpr ItemInfo Infos[] = {{"Objet vide", "Objet vide", UI::Icon::Empty},
                                           {"Cube", "Cube", UI::Icon::Cube},
                                           {"Modèle 3D...", "Modèle", UI::Icon::Model},
                                           {"Image à l'écran", "Image", UI::Icon::Image},
                                           {"Changement de scène", "Changement de scène", UI::Icon::SceneChange},
                                           {"Personnage...", "Joueur", UI::Icon::Character},
                                           {"Vaisseau (maquette)", "Vaisseau", UI::Icon::Ship},
                                           {"Lumière", "Lumière", UI::Icon::Light},
                                           {"Spot", "Spot", UI::Icon::Light},
                                           {"Ciel", "Ciel", UI::Icon::Sky},
                                           {"Volume intérieur", "Intérieur", UI::Icon::Collider},
                                           {"Interface...", "Interface", UI::Icon::Window},
                                           {"Son...", "Son", UI::Icon::Sound},
                                           {"Planète", "Planète", UI::Icon::Planet},
                                           {"Soleil", "Soleil", UI::Icon::Light},
                                           {"Prefab", "Prefab", UI::Icon::Prefab},
                                           {"Objet du jeu", "Objet", UI::Icon::Cube}};
            return Infos[item];
        }

        // Le texte cherché (sans casse, accents compris tels quels) apparaît-il dans le libellé ?
        bool Matches(std::string_view label, std::string_view filter) {
            if (filter.empty()) {
                return true;
            }
            const auto lower = [](std::string_view text) {
                std::string result(text);
                for (char& c : result) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return result;
            };
            return lower(label).find(lower(filter)) != std::string::npos;
        }

        // Nom d'un prefab dans le panneau : son fichier sans « .prefab.json ».
        std::string PrefabLabel(const std::filesystem::path& path) {
            std::string name = Platform::ToUtf8(path.filename());
            constexpr std::string_view Extension = ".prefab.json";
            if (name.ends_with(Extension)) {
                name.resize(name.size() - Extension.size());
            }
            return name;
        }

        bool Contains(const UI::Rect& rect, Vector2f point) {
            return point.x >= rect.min.x && point.x < rect.max.x && point.y >= rect.min.y && point.y < rect.max.y;
        }
    }

    void PlacePanel::Update(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, const UI::Rect& view,
                            const std::vector<std::filesystem::path>& prefabs, bool dropToGround) {
        UI::Context& ui = engine.GetUi();
        const Input& input = engine.GetWindow().GetInput();
        const Vector2f mouse = input.GetMousePosition();

        std::optional<Choice> clicked;
        std::optional<Choice> held;
        const auto choose = [&](bool pressed, Choice choice) {
            if (pressed) {
                clicked = choice;
            }
            if (ui.IsItemActive()) {
                held = std::move(choice);
            }
        };
        // Une ligne par objet, avec son pictogramme ; la recherche ne garde que les lignes qui contiennent le texte.
        const auto row = [&](Item item) {
            const ItemInfo info = GetInfo(static_cast<int>(item));
            if (Matches(info.label, m_filter)) {
                choose(ui.Selectable(info.label, info.icon), {item, {}, {}});
            }
        };
        // Catégorie repliable (dépliée au départ) ; en cherchant, toutes s'ouvrent et les vides disparaissent.
        // Les objets du jeu de la même catégorie suivent ceux de l'éditeur.
        const std::span<const PlaceItem> extensions = engine.GetEditorExtensions().GetPlaceItems();
        const auto extensionMatches = [&](const PlaceItem& extension, std::string_view title) {
            return extension.category == title && Matches(extension.label, m_filter);
        };
        const auto category = [&](std::string_view title, std::initializer_list<Item> items) {
            const bool any = std::ranges::any_of(items, [&](Item item) { return Matches(GetInfo(static_cast<int>(item)).label, m_filter); })
                          || std::ranges::any_of(extensions, [&](const PlaceItem& extension) { return extensionMatches(extension, title); });
            if (!any) {
                return;
            }
            if (!m_filter.empty()) {
                ui.SetNextTreeNodeOpen(true);
            }
            if (ui.CollapsingHeader(title)) {
                for (const Item item : items) {
                    row(item);
                }
                for (const PlaceItem& extension : extensions) {
                    if (extensionMatches(extension, title)) {
                        choose(ui.Selectable(extension.label, extension.icon), {Item::Extension, {}, extension.label});
                    }
                }
            }
        };
        // Position sans ancrage (l'éditeur l'ancre à gauche, en onglet avec « Scènes »).
        // Mode Jouer : rien à placer, la scène du jeu est jetée à l'arrêt (lignes grisées et inertes).
        const bool playing = engine.GetGameState() != GameState::Stopped;
        ui.SetNextPanelIcon(UI::Icon::Place);
        if (ui.BeginPanel("Placer", {8.0f, 8.0f + ui.GetMenuBarHeight()}, 300.0f)) {
            ui.SearchField("##recherche", m_filter, "Rechercher un objet...");
            if (playing) {
                ui.TextDisabled("Mode Jouer : arrêter le jeu pour placer.");
            }
            ui.BeginDisabled(playing);
            constexpr std::string_view Categories[] = {"Formes", "Lumières", "Espace", "Physique (en mode Jouer)", "Interface", "Son", "Logique"};
            category("Formes", {Item::Empty, Item::Cube, Item::Model});
            category("Lumières", {Item::Light, Item::Spot, Item::Sky, Item::Sun, Item::Interior});
            category("Espace", {Item::Planet}); // Monde : bouton de la barre de menus
            category("Physique (en mode Jouer)", {Item::Character, Item::Ship});
            category("Interface", {Item::ScreenImage, Item::Interface});
            category("Son", {Item::Sound});
            category("Logique", {Item::SceneChange});
            // Catégories que seul le jeu remplit, dans l'ordre de leur premier objet.
            for (std::size_t i = 0; i < extensions.size(); ++i) {
                const std::string& title = extensions[i].category;
                const bool known = std::ranges::find(Categories, std::string_view(title)) != std::end(Categories)
                                || std::ranges::any_of(extensions.first(i), [&](const PlaceItem& earlier) { return earlier.category == title; });
                if (!known) {
                    category(title, {});
                }
            }
            if (const Project* project = engine.GetProject(); project && !prefabs.empty()) {
                const bool any = std::ranges::any_of(prefabs, [&](const std::filesystem::path& prefab) {
                    return Matches(PrefabLabel(prefab), m_filter);
                });
                if (any && !m_filter.empty()) {
                    ui.SetNextTreeNodeOpen(true);
                }
                if (any && ui.CollapsingHeader("Prefabs")) {
                    for (const std::filesystem::path& prefab : prefabs) {
                        const std::string label = PrefabLabel(prefab);
                        if (!Matches(label, m_filter)) {
                            continue;
                        }
                        const std::string relative = Platform::ToGenericUtf8(prefab.lexically_relative(project->assetsDirectory));
                        ui.PushId(relative);
                        choose(ui.Selectable(label, UI::Icon::Prefab), {Item::Prefab, prefab, {}});
                        ui.PopId();
                    }
                }
            }
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
            PlaceFromPanel(engine, sceneEditor, history, *clicked, std::nullopt, view, dropToGround);
        }
        if (m_dragging && m_held && !input.IsMouseButtonDown(MouseButton::Left)) {
            // Lâché dans la vue, pas sur un panneau : l'objet est posé là. Ailleurs, rien.
            if (Contains(view, mouse) && !ui.WantsMouse()) {
                PlaceFromPanel(engine, sceneEditor, history, *m_held, mouse, view, dropToGround);
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
            const std::string label = m_held->item == Item::Prefab      ? PrefabLabel(m_held->prefab)
                                      : m_held->item == Item::Extension ? m_held->extension
                                                                        : GetInfo(static_cast<int>(m_held->item)).label;
            const Vector2f topLeft = mouse + Vector2f{14.0f, 10.0f};
            const Vector2f size = ui.MeasureText(label) + style.framePadding * 2.0f;
            list.AddRect(topLeft, topLeft + size, style.button);
            list.AddText(topLeft + style.framePadding, label, style.fontSize, style.text);
        }
    }

    std::vector<std::string> PlacePanel::GetItemLabels(Engine& engine, const std::vector<std::filesystem::path>& prefabs) {
        std::vector<std::string> labels;
        for (int item = 0; item < static_cast<int>(Item::Prefab); ++item) {
            labels.emplace_back(GetInfo(item).label);
        }
        for (const PlaceItem& extension : engine.GetEditorExtensions().GetPlaceItems()) {
            labels.push_back(extension.label);
        }
        for (const std::filesystem::path& prefab : prefabs) {
            labels.push_back("Prefab : " + PrefabLabel(prefab));
        }
        return labels;
    }

    std::expected<Uuid, std::string> PlacePanel::PlaceByLabel(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history,
                                                              std::string_view label, std::optional<std::string> file,
                                                              const UI::Rect& view, bool dropToGround,
                                                              const std::vector<std::filesystem::path>& prefabs) {
        std::optional<Choice> choice;
        for (int item = 0; item < static_cast<int>(Item::Prefab) && !choice; ++item) {
            if (label == GetInfo(item).label) {
                choice = Choice{static_cast<Item>(item), {}, {}};
            }
        }
        if (!choice && engine.GetEditorExtensions().FindPlaceItem(label)) {
            choice = Choice{Item::Extension, {}, std::string(label)};
        }
        constexpr std::string_view PrefabPrefix = "Prefab : ";
        for (const std::filesystem::path& prefab : prefabs) {
            if (!choice && label.starts_with(PrefabPrefix) && label.substr(PrefabPrefix.size()) == PrefabLabel(prefab)) {
                choice = Choice{Item::Prefab, prefab, {}};
            }
        }
        if (!choice) {
            return std::unexpected(std::format("« {} » n'est pas dans le panneau Placer", label));
        }
        auto placed = Place(engine, sceneEditor, history, *choice, std::nullopt, view, dropToGround, std::move(file));
        if (!placed && placed.error().empty()) {
            return std::unexpected("abandonné (fichier non choisi)");
        }
        return placed;
    }

    void PlacePanel::PlaceFromPanel(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, const Choice& choice,
                                    std::optional<Vector2f> drop, const UI::Rect& view, bool dropToGround) {
        if (auto placed = Place(engine, sceneEditor, history, choice, drop, view, dropToGround); !placed && !placed.error().empty()) {
            Platform::ShowError(engine.GetWindow(), "Placer", placed.error());
        }
    }

    std::expected<Uuid, std::string> PlacePanel::Place(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history,
                                                       const Choice& choice, std::optional<Vector2f> drop, const UI::Rect& view,
                                                       bool dropToGround, std::optional<std::string> file) {
        history.Commit(engine); // un geste en cours est un pas à part, avant celui-ci
        Scene& scene = engine.GetScene();
        const Item item = choice.item;
        if (item == Item::Sky) {
            // Un seul ciel compte : s'il y en a déjà un, c'est lui qu'on sélectionne (à régler dans l'inspecteur).
            for (const auto [existing, sky] : scene.GetRegistry().view<const SkyComponent>().each()) {
                sceneEditor.Select(scene, existing);
                return scene.GetId(existing);
            }
        } else if (item == Item::Sun) {
            for (const auto [existing, sun] : scene.GetRegistry().view<const SunComponent>().each()) {
                sceneEditor.Select(scene, existing);
                return scene.GetId(existing);
            }
        }
        const ItemInfo info = GetInfo(static_cast<int>(item));
        // Objet du jeu : copié, son build et ses réglages restent valides le temps du placement.
        std::optional<PlaceItem> extension;
        if (item == Item::Extension) {
            const PlaceItem* found = engine.GetEditorExtensions().FindPlaceItem(choice.extension);
            if (!found) {
                return std::unexpected(std::string{}); // le jeu rechargé ne le propose plus
            }
            extension = *found;
        }
        // Un modèle sans fichier ne montrerait rien : le fichier d'abord (copié dans le projet s'il vient d'ailleurs),
        // et l'entité porte son nom.
        std::string name = extension ? extension->name : std::string(info.name);
        std::optional<std::string> model;
        if (item == Item::Model || item == Item::Character) {
            model = file ? file : ChooseAsset(engine, AssetType::Model, "");
            if (!model) {
                return std::unexpected(std::string{});
            }
            std::string stem = Platform::ToUtf8(Platform::PathFromUtf8(*model).stem());
            if (!stem.empty() && item == Item::Model) {
                name = std::move(stem);
            }
        }
        // Une interface : le fichier .fluxui d'abord (créé dans Contenu, clic droit > Nouvelle interface).
        std::optional<std::string> interfaceFile;
        if (item == Item::Interface) {
            interfaceFile = file ? file : ChooseAsset(engine, AssetType::Interface, "");
            if (!interfaceFile) {
                return std::unexpected(std::string{});
            }
            if (std::string stem = Platform::ToUtf8(Platform::PathFromUtf8(*interfaceFile).stem()); !stem.empty()) {
                name = std::move(stem);
            }
        }
        // Un son : le fichier d'abord, comme un modèle ; l'entité porte son nom.
        std::optional<std::string> soundFile;
        if (item == Item::Sound) {
            soundFile = file ? file : ChooseAsset(engine, AssetType::Sound, "");
            if (!soundFile) {
                return std::unexpected(std::string{});
            }
            if (std::string stem = Platform::ToUtf8(Platform::PathFromUtf8(*soundFile).stem()); !stem.empty()) {
                name = std::move(stem);
            }
        }
        Entity entity = NullEntity;
        if (item == Item::Prefab) {
            // Nouvel exemplaire : ses pièces suivront le prefab, comme celles des autres exemplaires.
            const Project* project = engine.GetProject();
            const std::string path = project ? Platform::ToGenericUtf8(choice.prefab.lexically_relative(project->assetsDirectory))
                                             : std::string{};
            auto placed = SceneSerializer::Instantiate(scene, engine.GetComponents(), path, engine.GetPrefabReader());
            if (!placed) {
                return std::unexpected("prefab : " + placed.error());
            }
            entity = *placed;
            name = PrefabLabel(choice.prefab);
        } else {
            entity = scene.CreateEntity(name);
        }

        if (item == Item::Interface) {
            // Toute la vue : rien à placer.
            scene.Add<InterfaceComponent>(entity).file = *interfaceFile;
        } else if (item == Item::ScreenImage) {
            // Accrochée au centre ; lâchée dans la vue, décalée jusqu'au point de dépôt (en pixels de référence).
            // Créée sans fichier : cadre gris dans la vue, à choisir dans l'inspecteur (« ... »). Le texte se pose
            // depuis la barre d'outils (TextTools), avec sa police, sa taille et sa couleur.
            scene.Add<ScreenImageComponent>(entity);
            const float scale = ScreenUi::GetScale(view.min, view.max);
            if (const std::optional<ScreenUi::Offset> offset = ScreenUi::FindOffset(scene, entity); offset && drop && scale > 0.0f) {
                const Vector2f moved = *drop - ScreenUi::AnchorPoint(ScreenAnchor::Center, view.min, view.max);
                *offset->x = std::round(moved.x / scale);
                *offset->y = std::round(moved.y / scale);
            }
        } else if (item == Item::SceneChange) {
            // Rien à voir ni à placer : se règle dans l'inspecteur (scène suivante, délai).
            scene.Add<SceneChangeComponent>(entity);
        } else if (item == Item::Sky) {
            // Autour de tout, à l'infini : sa position ne compte pas.
            scene.Add<SkyComponent>(entity);
        } else if (item == Item::Sun) {
            // Fixe, au centre de la scène : le système se construit autour de lui (planètes en orbite, Orbits::Update).
            scene.Add<SunComponent>(entity);
        } else {
            if (item == Item::Cube) {
                // Solide dès sa création (immobile) : on marche dessus. « Simuler la physique » (inspecteur, Collision) le fait tomber.
                scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(180, 180, 185));
                scene.Add<ColliderComponent>(entity);
            } else if (extension) {
                extension->build(scene, entity);
            } else if (item == Item::Model) {
                scene.Add<ModelComponent>(entity).model = std::move(*model);
            } else if (item == Item::Character) {
                // Les pieds du modèle (son origine) posés sur celle de l'entité, comme la capsule.
                scene.Add<ModelComponent>(entity).model = std::move(*model);
                scene.Add<CharacterComponent>(entity);
                scene.Add<AnimatorComponent>(entity); // sans effet si le modèle n'a pas d'animation
            } else if (item == Item::Ship) {
                scene.Add<ShipComponent>(entity);
                AddShipBlockout(scene, entity);
            } else if (item == Item::Sound) {
                scene.Add<SoundComponent>(entity).file = std::move(*soundFile);
            } else if (item == Item::Interior) {
                scene.Add<InteriorComponent>(entity);
            } else if (item == Item::Planet) {
                scene.Add<PlanetComponent>(entity);
                scene.Add<CloudsComponent>(entity);
                scene.Add<VegetationComponent>(entity);
                if (!scene.GetRegistry().view<const SunComponent>().empty()) {
                    // En orbite autour du Soleil, à la distance de la Terre ; chaque nouvelle planète 40° plus loin.
                    const auto planets = scene.GetRegistry().view<const PlanetComponent, const OrbitComponent>();
                    const auto count = std::ranges::distance(planets.begin(), planets.end());
                    scene.Add<OrbitComponent>(entity, OrbitComponent{.distance = SunDistance / 1.0e9,
                                                                     .angle = std::fmod(40.0 * static_cast<double>(count), 360.0)});
                }
                // Liseré bleuté au bord du disque, vu de l'espace : la planète se détache du noir, même côté nuit.
                scene.Add<RimLightComponent>(entity, RimLightComponent{.intensity = 0.8f, .sharpness = 6.0f});
            } else if (item == Item::Light || item == Item::Spot) {
                LightComponent& light = scene.Add<LightComponent>(entity);
                if (item == Item::Spot) {
                    // Tourné vers le bas (son -z vers -y du monde) : une lampe de plafond, à orienter ensuite.
                    light.type = LightType::Spot;
                    scene.Get<TransformComponent>(entity).rotation =
                        Math::Quaternionf::FromAxisAngle({1.0f, 0.0f, 0.0f}, -0.5f * std::numbers::pi_v<float>);
                }
            }
            // Devant la caméra, ou le long du rayon qui passe sous la souris ; dans le secteur de la caméra.
            const Camera& camera = engine.GetCamera();
            Math::Vector3d direction = drop ? ScreenProjection(camera, view.min, view.max).GetRay(*drop).direction
                                                  : Math::Vector3d(camera.GetForward());
            Math::WorldPosition position = camera.position;
            if (extension && extension->level && !drop) {
                // À l'horizontale, face à la caméra (PlaceItem::level).
                direction = Math::Vector3d(direction.x, 0.0, direction.z);
                direction = Math::Length(direction) > 1.0e-6 ? Math::Normalize(direction) : Math::Vector3d(0.0, 0.0, -1.0);
                position += Math::Vector3d(0.0, -extension->below, 0.0);
                scene.Get<TransformComponent>(entity).rotation = Math::Quaternionf::FromAxisAngle(
                    {0.0f, 1.0f, 0.0f}, static_cast<float>(std::atan2(direction.x, direction.z)));
            }
            // Un prefab de vaisseau est vu comme le chasseur : en entier.
            const bool bigPrefab = item == Item::Prefab && scene.TryGet<ShipComponent>(entity);
            double distance = extension ? extension->distance
                            : item == Item::Ship ? ShipDistance
                            : bigPrefab ? PrefabShipDistance : PlaceDistance;
            if (item == Item::Planet) {
                // Assez loin pour la voir en entier.
                distance = PlanetDistance * static_cast<double>(scene.Get<PlanetComponent>(entity).radius);
            }
            position += direction * distance;
            scene.Get<TransformComponent>(entity).position = position.offset;
            if (position.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, position.sector);
            }
            if (dropToGround && item != Item::Planet && (!extension || extension->dropToGround)) {
                // Posé sur ce qui est dessous. Les rayons partent au moins de la hauteur de la caméra : un objet mis
                // devant elle quand elle regarde le sol, donc sous lui, remonte dessus.
                std::vector<RelativeTransform> transforms;
                scene.ComputeRelativeTransforms(camera.position, transforms);
                GroundProbe probe;
                const Math::Vector3d at = Math::Delta(camera.position, position);
                if (PrepareGround(engine, entity, camera.position, transforms, at, probe)) {
                    if (const std::optional<double> height = FindGround(probe, at, 0.0)) {
                        position += Math::Vector3d(0.0, *height - at.y, 0.0);
                        scene.Get<TransformComponent>(entity).position = position.offset;
                        if (position.sector != Math::Vector3i64{}) {
                            scene.GetRegistry().emplace_or_replace<SectorComponent>(entity, position.sector);
                        } else {
                            scene.GetRegistry().remove<SectorComponent>(entity);
                        }
                    }
                }
            }
            if (item == Item::Planet && scene.TryGet<OrbitComponent>(entity)) {
                // Sur son orbite, loin de la caméra : la caméra va la voir.
                Orbits::Update(scene);
                Math::WorldPosition seen = scene.GetWorldPosition(entity);
                seen += Math::Vector3d(camera.GetForward()) * -distance;
                engine.GetCamera().position = seen;
            }
        }

        sceneEditor.Select(scene, entity);
        // Avec ses enfants (maquette, prefab, objet du jeu) : annuler retire tout, rétablir remet tout.
        const std::string label = item == Item::Prefab ? name : extension ? extension->label : std::string(info.label);
        history.Push("Placer : " + label, scene.GetId(entity), {},
                     UndoHistory::Capture(engine, scene.GetId(entity), item == Item::Ship || item == Item::Prefab || extension));
        if (item == Item::Character) {
            sceneEditor.RequestClaimPlayer(); // le nouveau personnage devient le seul dirigé par le joueur
        }
        return scene.GetId(entity);
    }
}
