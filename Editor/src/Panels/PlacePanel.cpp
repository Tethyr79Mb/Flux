#include "Panels/PlacePanel.hpp"

#include "Document/UndoHistory.hpp"
#include "Panels/AssetPicker.hpp"
#include "Panels/SceneEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/ScreenUi.hpp>
#include <Flux/UI/DrawList.hpp>

#include <cmath>
#include <numbers>
#include <cstdint>
#include <string>

namespace FluxEditor {
    using namespace Flux;
    using Math::Vector2f;

    namespace {
        constexpr float DragThreshold = 4.0f; // pixels avant qu'un bouton tenu devienne un glisser-déposer
        constexpr double PlaceDistance = 5.0; // mètres devant la caméra (ou le long du rayon de la souris)
        constexpr double ShipDistance = 25.0; // la maquette fait 16 m de long : la voir en entier
        constexpr double FighterDistance = 32.0; // le chasseur fait 20 m de long et 15 m d'envergure

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

        // Chasseur monoplace (modèle du projet, dans Modeles/Chasseur) : coque affichée et collision en maillage
        // simplifié, verrière solide (personnages et caméra s'y arrêtent), poste de pilotage. Origine du vaisseau :
        // le plancher du cockpit, l'avant vers -z ; les roues touchent le sol 2,3 m plus bas. Pas de soute : F à moins
        // de 10 m du siège met directement aux commandes ; en se relevant, le pilote se retrouve au sol, devant l'aile
        // gauche.
        void AddFighter(Scene& scene, Entity ship) {
            const Entity hull = scene.CreateEntity("Coque", ship);
            scene.Add<ModelComponent>(hull).model = "Modeles/Chasseur/Chasseur.glb";
            ColliderComponent& collider = scene.Add<ColliderComponent>(hull);
            collider.shape = ColliderShape::Mesh;
            collider.mesh = "Modeles/Chasseur/Chasseur-collision.glb";
            // Liseré bleu froid sur les bords de la silhouette, qui la détache du fond spatial.
            RimLightComponent& rim = scene.Add<RimLightComponent>(hull);
            rim.intensity = 0.8f;
            rim.sharpness = 4.0f;
            // La verrière arrête aussi les personnages (pas seulement la caméra) : on ne tombe pas dans le cockpit
            // depuis le nez, et on n'en sort pas en sautant. Le pilote n'y touche pas : assis, il est hors physique.
            const Entity glass = scene.CreateEntity("Verrière", ship);
            ColliderComponent& glassCollider = scene.Add<ColliderComponent>(glass);
            glassCollider.shape = ColliderShape::Mesh;
            glassCollider.mesh = "Modeles/Chasseur/Chasseur-verriere.glb";
            // Train d'atterrissage : la collision simplifiée s'arrête au ventre (1,97 m sous l'origine), les roues
            // descendent à 2,3 m. Une boîte par jambe, pour qu'il se pose sur ses roues.
            struct Gear { const char* name; Math::Vector3f center; Math::Vector3f size; };
            for (const Gear& gear : {Gear{"Train avant", {0.0f, -2.1f, 1.6f}, {0.3f, 0.4f, 0.6f}},
                                     Gear{"Train gauche", {-2.4f, -2.1f, 10.0f}, {0.7f, 0.4f, 0.6f}},
                                     Gear{"Train droit", {2.4f, -2.1f, 10.0f}, {0.7f, 0.4f, 0.6f}}}) {
                const Entity leg = scene.CreateEntity(gear.name, ship);
                scene.Get<TransformComponent>(leg).position = {gear.center.x, gear.center.y, gear.center.z};
                scene.Add<ColliderComponent>(leg).size = gear.size;
            }
            const Entity seat = scene.CreateEntity("Poste de pilotage", ship);
            // Le cockpit est tout à l'avant (3,6 m derrière le nez) et l'origine est sous les hanches du pilote.
            SeatComponent& component = scene.Add<SeatComponent>(seat);
            component.pilot = true;
            component.reach = 10.0f;
            component.exit = {-3.5f, -2.3f, 1.4f};
            // Tête du pilote calée contre l'appui-tête incliné : assez haut et en arrière pour voir le tableau de bord
            // (1 à 1,5 m devant, sous la ligne des yeux) et le nez par la verrière.
            component.eye = {0.0f, 1.15f, 0.2f};
            // Bruit des moteurs, à l'arrière (tuyères) : plus fort et plus aigu avec la vitesse.
            const Entity engines = scene.CreateEntity("Moteurs", ship);
            scene.Get<TransformComponent>(engines).position = {0.0, 1.2, 15.0};
            SoundComponent& sound = scene.Add<SoundComponent>(engines);
            sound.file = "Sons/Reacteur.wav";
            sound.loop = true;
            sound.followShipSpeed = true;
            sound.minDistance = 6.0f;
            sound.maxDistance = 150.0f;
        }

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
                                           {"Personnage...", "Joueur"},
                                           {"Vaisseau (maquette)", "Vaisseau"},
                                           {"Chasseur", "Chasseur"},
                                           {"Lumière", "Lumière"},
                                           {"Spot", "Spot"},
                                           {"Ciel", "Ciel"},
                                           {"Volume intérieur", "Intérieur"},
                                           {"Interface...", "Interface"},
                                           {"Son...", "Son"}};
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
        // Position sans ancrage (l'éditeur l'ancre à gauche, en onglet avec « Scènes »).
        // Mode Jouer : rien à placer, la scène du jeu est jetée à l'arrêt (boutons grisés et inertes).
        const bool playing = engine.GetGameState() != GameState::Stopped;
        ui.SetNextPanelIcon(UI::Icon::Place);
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
            ui.TextDisabled("Lumières");
            button(Item::Light);
            ui.SameLine();
            button(Item::Spot);
            ui.SameLine();
            button(Item::Sky);
            button(Item::Interior);
            ui.Separator();
            ui.TextDisabled("Interface");
            button(Item::ScreenText);
            ui.SameLine();
            button(Item::ScreenImage);
            ui.SameLine();
            button(Item::Interface);
            ui.Separator();
            ui.TextDisabled("Son");
            button(Item::Sound);
            ui.Separator();
            ui.TextDisabled("Logique");
            button(Item::SceneChange);
            ui.Separator();
            ui.TextDisabled("Physique (en mode Jouer)");
            button(Item::Character);
            button(Item::Ship);
            ui.SameLine();
            button(Item::Fighter);
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
        if (item == Item::Sky) {
            // Un seul ciel compte : s'il y en a déjà un, c'est lui qu'on sélectionne (à régler dans l'inspecteur).
            for (const auto [existing, sky] : scene.GetRegistry().view<const SkyComponent>().each()) {
                sceneEditor.Select(scene, existing);
                return;
            }
        }
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
        // Une interface : le fichier .fluxui d'abord (créé dans Contenu, clic droit > Nouvelle interface).
        std::optional<std::string> interfaceFile;
        if (item == Item::Interface) {
            interfaceFile = ChooseAsset(engine, AssetType::Interface, "");
            if (!interfaceFile) {
                return;
            }
            if (std::string stem = Platform::ToUtf8(Platform::PathFromUtf8(*interfaceFile).stem()); !stem.empty()) {
                name = std::move(stem);
            }
        }
        // Un son : le fichier d'abord, comme un modèle ; l'entité porte son nom.
        std::optional<std::string> soundFile;
        if (item == Item::Sound) {
            soundFile = ChooseAsset(engine, AssetType::Sound, "");
            if (!soundFile) {
                return;
            }
            if (std::string stem = Platform::ToUtf8(Platform::PathFromUtf8(*soundFile).stem()); !stem.empty()) {
                name = std::move(stem);
            }
        }
        const Entity entity = scene.CreateEntity(name);

        if (item == Item::Interface) {
            // Toute la vue : rien à placer.
            scene.Add<InterfaceComponent>(entity).file = *interfaceFile;
        } else if (item == Item::ScreenText || item == Item::ScreenImage) {
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
        } else if (item == Item::Sky) {
            // Autour de tout, à l'infini : sa position ne compte pas.
            scene.Add<SkyComponent>(entity);
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
            } else if (item == Item::Ship) {
                scene.Add<ShipComponent>(entity);
                AddShipBlockout(scene, entity);
            } else if (item == Item::Fighter) {
                // Chasseur : vire vite, pour suivre la souris (30 °/s par défaut, celle d'un transport, paraît mou).
                scene.Add<ShipComponent>(entity).turnRate = 90.0f;
                AddFighter(scene, entity);
            } else if (item == Item::Sound) {
                scene.Add<SoundComponent>(entity).file = std::move(*soundFile);
            } else if (item == Item::Interior) {
                scene.Add<InteriorComponent>(entity);
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
            const Math::Vector3d direction = drop ? ScreenProjection(camera, view.min, view.max).GetRay(*drop).direction
                                                  : Math::Vector3d(camera.GetForward());
            Math::WorldPosition position = camera.position;
            position += direction * (item == Item::Ship ? ShipDistance : item == Item::Fighter ? FighterDistance : PlaceDistance);
            scene.Get<TransformComponent>(entity).position = position.offset;
            if (position.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, position.sector);
            }
        }

        sceneEditor.Select(scene, entity);
        // Avec ses enfants (maquette, chasseur) : annuler retire tout, rétablir remet tout.
        history.Push(std::string("Placer : ") + info.label, scene.GetId(entity), {},
                     UndoHistory::Capture(engine, scene.GetId(entity), item == Item::Ship || item == Item::Fighter));
        if (item == Item::Character) {
            sceneEditor.RequestClaimPlayer(); // le nouveau personnage devient le seul dirigé par le joueur
        }
    }
}
