#include "Panels/PlacePanel.hpp"

#include "Document/UndoHistory.hpp"
#include "Panels/AssetPicker.hpp"
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
#include <initializer_list>
#include <numbers>
#include <string>

namespace FluxEditor {
    using namespace Flux;
    using Math::Vector2f;

    namespace {
        constexpr float DragThreshold = 4.0f; // pixels avant qu'un bouton tenu devienne un glisser-déposer
        constexpr double PlaceDistance = 5.0; // mètres devant la caméra (ou le long du rayon de la souris)
        constexpr double ShipDistance = 25.0; // la maquette fait 16 m de long : la voir en entier
        constexpr double FighterDistance = 32.0; // le chasseur fait 20 m de long et 15 m d'envergure
        constexpr double StationDistance = 250.0; // la station fait 142 x 31 x 119 m
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
            // Flamme de la tuyère (rectangle bleu au bout du bloc moteur, sa face arrière à z 16,12) ; suit la poussée.
            const Entity thruster = scene.CreateEntity("Réacteur", ship);
            scene.Get<TransformComponent>(thruster).position = {0.0, 1.695, 16.13};
            ThrusterComponent& flame = scene.Add<ThrusterComponent>(thruster);
            flame.width = 2.3f;
            flame.height = 0.75f;
            // Armes, chacune sur son emport et avec son modèle (pièces sorties de Chasseur.glb par armes.py, origine
            // sur le pivot) : le Gatling sous le nez (centre de la boule), les autocanons sous les racines d'aile
            // (milieu de la culasse) et deux lance-missiles sous les ailes.
            struct Mount
            {
                const char* name;
                const char* weapon;
                const char* model;
                Math::Vector3d position;
                WeaponKind kind;
                std::int32_t size;
                float cone;
                float muzzle;
                std::int32_t group; // touche de tir
            };
            constexpr const char* Gatling = "Modeles/Chasseur/Gatling-MK1.glb";
            constexpr const char* Autocanon = "Modeles/Chasseur/Autocanon-MK1.glb";
            constexpr const char* Missiles = "Modeles/Chasseur/Lance-missiles.glb";
            for (const Mount& mount :
                 {Mount{"Emport du nez", "Gatling MK1", Gatling, {0.0, -1.55, -1.2}, WeaponKind::Gun, 2, 30.0f, 3.05f, 1},
                  Mount{"Emport ventral gauche", "Autocanon MK1", Autocanon, {-1.5, -1.06, 7.2}, WeaponKind::Gun, 1, 5.0f, 1.61f, 2},
                  Mount{"Emport ventral droit", "Autocanon MK1", Autocanon, {1.5, -1.06, 7.2}, WeaponKind::Gun, 1, 5.0f, 1.61f, 2},
                  Mount{"Emport d'aile gauche", "Lance-missiles", Missiles, {-5.0, -1.3, 9.5}, WeaponKind::Missile, 2, 0.0f, 0.85f, 3},
                  Mount{"Emport d'aile droit", "Lance-missiles", Missiles, {5.0, -1.3, 9.5}, WeaponKind::Missile, 2, 0.0f, 0.85f, 3}}) {
                const Entity hardpoint = scene.CreateEntity(mount.name, ship);
                scene.Get<TransformComponent>(hardpoint).position = mount.position;
                scene.Add<HardpointComponent>(hardpoint, HardpointComponent{.kind = mount.kind, .size = mount.size});
                const Entity weapon = scene.CreateEntity(mount.weapon, hardpoint);
                scene.Add<ModelComponent>(weapon).model = mount.model;
                WeaponComponent& arms = scene.Add<WeaponComponent>(weapon);
                arms.kind = mount.kind;
                arms.size = mount.size;
                arms.cone = mount.cone;
                arms.muzzle = mount.muzzle;
                arms.group = mount.group;
                if (mount.size >= 2 && mount.kind == WeaponKind::Gun) {
                    arms.fireRate = 12.0f; // Gatling à six tubes
                }
                if (mount.kind == WeaponKind::Missile) {
                    // Quatre tubes ; plus lent qu'un obus mais guidé, deux missiles détruisent une Cible.
                    arms.ammo = 4;
                    arms.fireRate = 1.0f;
                    arms.projectileSpeed = 400.0f;
                    arms.range = 4000.0f;
                    arms.damage = 60.0f;
                    arms.color = {1.0f, 0.85f, 0.6f};
                }
            }
        }

        // Station en maquette de cubes, dans son repère (le sol du hangar à y = 0, l'entrée vers -z) : un hangar de
        // 140 x 30 x 100 m ouvert sur l'espace avec quatre quais, puis au fond un couloir qui dessert deux boutiques
        // de marchands et une salle des terminaux. Gravité à bord, immobile. De quoi marcher et essayer l'amarrage
        // avant le vrai modèle.
        void AddStation(Scene& scene, Entity station) {
            struct Box
            {
                const char* name;
                Math::Vector3d min;
                Math::Vector3d max;
                Math::LinearColor color;
                float emission = 0.0f;
                bool solid = true; // marquages au sol et néons : seulement affichés
            };
            const auto grey = [](std::uint8_t level) { return Math::LinearColor::FromSrgb8(level, level, level + 8); };
            const auto addBox = [&](const Box& box, Entity parent) {
                const Entity entity = scene.CreateEntity(box.name, parent);
                TransformComponent& transform = scene.Get<TransformComponent>(entity);
                transform.position = (box.min + box.max) * 0.5;
                const Math::Vector3d size = box.max - box.min;
                transform.scale = {static_cast<float>(size.x), static_cast<float>(size.y), static_cast<float>(size.z)};
                MeshRendererComponent& mesh = scene.Add<MeshRendererComponent>(entity, MeshType::Cube, box.color);
                mesh.emission = box.emission;
                if (box.solid) {
                    scene.Add<ColliderComponent>(entity);
                }
                return entity;
            };
            const auto addLight = [&](const char* name, Math::Vector3d position, float intensity, float range) {
                const Entity entity = scene.CreateEntity(name, station);
                scene.Get<TransformComponent>(entity).position = position;
                LightComponent& light = scene.Add<LightComponent>(entity);
                light.color = {1.0f, 0.95f, 0.88f};
                light.intensity = intensity;
                light.range = range;
            };
            const auto addInterior = [&](const char* name, Math::Vector3d center, Math::Vector3f size, float intensity) {
                const Entity entity = scene.CreateEntity(name, station);
                scene.Get<TransformComponent>(entity).position = center;
                InteriorComponent& interior = scene.Add<InteriorComponent>(entity);
                interior.size = size;
                interior.intensity = intensity;
                interior.fade = 1.0f;
            };

            // Hangar : parois d'un mètre ; l'ouverture de l'avant (100 x 24 m) laisse passer un chasseur de 15 m
            // d'envergure avec de la marge. Le mur du fond, plus mince, a une porte de 3 m vers le couloir.
            const Entity hangar = scene.CreateEntity("Hangar", station);
            for (const Box& box : {Box{"Sol", {-71.0, -1.0, -51.0}, {71.0, 0.0, 50.4}, grey(80)},
                                   Box{"Plafond", {-71.0, 30.0, -51.0}, {71.0, 31.0, 51.0}, grey(110)},
                                   Box{"Paroi gauche", {-71.0, 0.0, -51.0}, {-70.0, 30.0, 51.0}, grey(140)},
                                   Box{"Paroi droite", {70.0, 0.0, -51.0}, {71.0, 30.0, 51.0}, grey(140)},
                                   Box{"Entrée gauche", {-70.0, 0.0, -51.0}, {-50.0, 30.0, -50.0}, grey(130)},
                                   Box{"Entrée droite", {50.0, 0.0, -51.0}, {70.0, 30.0, -50.0}, grey(130)},
                                   Box{"Linteau de l'entrée", {-50.0, 24.0, -51.0}, {50.0, 30.0, -50.0}, grey(130)},
                                   Box{"Fond gauche", {-70.0, 0.0, 50.0}, {-1.5, 30.0, 50.4}, grey(125)},
                                   Box{"Fond droit", {1.5, 0.0, 50.0}, {70.0, 30.0, 50.4}, grey(125)},
                                   Box{"Fond au-dessus de la porte", {-1.5, 3.0, 50.0}, {1.5, 30.0, 50.4}, grey(125)},
                                   // Balises de l'entrée : elle se repère de loin.
                                   Box{"Balise gauche", {-50.0, 0.0, -51.2}, {-49.6, 24.0, -50.8},
                                       Math::LinearColor{0.2f, 0.6f, 1.0f}, 4.0f, false},
                                   Box{"Balise droite", {49.6, 0.0, -51.2}, {50.0, 24.0, -50.8},
                                       Math::LinearColor{0.2f, 0.6f, 1.0f}, 4.0f, false}}) {
                addBox(box, hangar);
            }

            // Quatre quais de 24 x 28 m en ligne, au milieu du hangar. Chaque « Quai N » est le point d'amarrage :
            // l'origine d'un chasseur posé (son plancher de cockpit, 2,3 m au-dessus de ses roues), le nez vers la
            // sortie (-z), prêt à repartir. Le marquage au sol est seulement peint (pas de marche à franchir).
            constexpr double DockX[] = {-51.0, -17.0, 17.0, 51.0};
            constexpr double DockZ = 10.0;
            constexpr double HalfWidth = 12.0;
            constexpr double HalfLength = 14.0;
            constexpr double Line = 0.4;
            const Math::LinearColor yellow{1.0f, 0.75f, 0.1f};
            for (int i = 0; i < 4; ++i) {
                const double x = DockX[i];
                const std::string name = "Quai " + std::to_string(i + 1);
                const Entity dock = scene.CreateEntity(name, station);
                scene.Get<TransformComponent>(dock).position = {x, 2.3, DockZ};
                const Entity marks = scene.CreateEntity("Marquage du " + name, hangar);
                const double x0 = x - HalfWidth, x1 = x + HalfWidth, z0 = DockZ - HalfLength, z1 = DockZ + HalfLength;
                for (const Box& box : {Box{"Dalle", {x0, 0.0, z0}, {x1, 0.01, z1}, grey(60), 0.0f, false},
                                       Box{"Bord avant", {x0, 0.0, z0}, {x1, 0.02, z0 + Line}, yellow, 1.5f, false},
                                       Box{"Bord arrière", {x0, 0.0, z1 - Line}, {x1, 0.02, z1}, yellow, 1.5f, false},
                                       Box{"Bord gauche", {x0, 0.0, z0}, {x0 + Line, 0.02, z1}, yellow, 1.5f, false},
                                       Box{"Bord droit", {x1 - Line, 0.0, z0}, {x1, 0.02, z1}, yellow, 1.5f, false},
                                       // Néon au plafond, au-dessus du quai.
                                       Box{"Plafonnier", {x - 4.0, 29.7, DockZ - 4.0}, {x + 4.0, 30.0, DockZ + 4.0},
                                           Math::LinearColor{1.0f, 0.95f, 0.88f}, 3.0f, false}}) {
                    addBox(box, marks);
                }
                addLight(("Lumière du " + name).c_str(), {x, 26.0, DockZ}, 400.0f, 45.0f);
            }
            addLight("Lumière de l'entrée", {0.0, 20.0, -35.0}, 300.0f, 40.0f);
            addLight("Lumière du fond", {0.0, 20.0, 35.0}, 80.0f, 35.0f);
            addInterior("Intérieur du hangar", {0.0, 15.0, 0.0}, {140.0f, 30.0f, 100.0f}, 1.0f);

            // Derrière le hangar : un couloir (z 50,4 à 54,4) puis trois pièces de 14 x 12,4 m, sous un même plafond
            // de 3,5 m. Les vides entre les pièces restent fermés.
            const Entity decks = scene.CreateEntity("Coursives", station);
            constexpr double Height = 3.5;
            constexpr double Rooms[] = {-29.0, 0.0, 29.0}; // centre de chaque pièce et de sa porte
            for (const Box& box : {Box{"Sol", {-40.4, -1.0, 50.4}, {40.4, 0.0, 67.6}, grey(95)},
                                   Box{"Plafond", {-40.4, Height, 50.4}, {40.4, Height + 0.4, 67.6}, grey(150)},
                                   Box{"Paroi gauche", {-40.4, 0.0, 50.4}, {-40.0, Height, 67.6}, grey(160)},
                                   Box{"Paroi droite", {40.0, 0.0, 50.4}, {40.4, Height, 67.6}, grey(160)},
                                   Box{"Paroi du fond", {-40.0, 0.0, 67.2}, {40.0, Height, 67.6}, grey(160)},
                                   // Mur côté pièces, portes de 2 x 2,5 m en face de chaque pièce.
                                   Box{"Mur du couloir 1", {-40.0, 0.0, 54.4}, {-30.0, Height, 54.8}, grey(170)},
                                   Box{"Mur du couloir 2", {-28.0, 0.0, 54.4}, {-1.0, Height, 54.8}, grey(170)},
                                   Box{"Mur du couloir 3", {1.0, 0.0, 54.4}, {28.0, Height, 54.8}, grey(170)},
                                   Box{"Mur du couloir 4", {30.0, 0.0, 54.4}, {40.0, Height, 54.8}, grey(170)},
                                   Box{"Linteau 1", {-30.0, 2.5, 54.4}, {-28.0, Height, 54.8}, grey(170)},
                                   Box{"Linteau 2", {-1.0, 2.5, 54.4}, {1.0, Height, 54.8}, grey(170)},
                                   Box{"Linteau 3", {28.0, 2.5, 54.4}, {30.0, Height, 54.8}, grey(170)},
                                   // Cloisons des pièces (7 m de part et d'autre de leur centre).
                                   Box{"Cloison 1", {-36.4, 0.0, 54.8}, {-36.0, Height, 67.2}, grey(165)},
                                   Box{"Cloison 2", {-22.0, 0.0, 54.8}, {-21.6, Height, 67.2}, grey(165)},
                                   Box{"Cloison 3", {-7.4, 0.0, 54.8}, {-7.0, Height, 67.2}, grey(165)},
                                   Box{"Cloison 4", {7.0, 0.0, 54.8}, {7.4, Height, 67.2}, grey(165)},
                                   Box{"Cloison 5", {21.6, 0.0, 54.8}, {22.0, Height, 67.2}, grey(165)},
                                   Box{"Cloison 6", {36.0, 0.0, 54.8}, {36.4, Height, 67.2}, grey(165)}}) {
                addBox(box, decks);
            }
            for (const double x : {-25.0, 0.0, 25.0}) {
                addLight("Lumière du couloir", {x, Height - 0.3, 52.4}, 8.0f, 14.0f);
            }
            addInterior("Intérieur du couloir", {0.0, 0.5 * Height, 52.4}, {80.0f, static_cast<float>(Height), 4.0f}, 1.0f);

            // Boutiques : un comptoir, une étagère contre le mur du fond, et la place du marchand entre les deux.
            const Math::LinearColor wood = Math::LinearColor::FromSrgb8(120, 85, 55);
            for (int i = 0; i < 2; ++i) {
                const double x = Rooms[i];
                const std::string name = "Boutique " + std::to_string(i + 1);
                const Entity shop = scene.CreateEntity(name, station);
                for (const Box& box : {Box{"Comptoir", {x - 4.0, 0.0, 61.8}, {x + 4.0, 1.1, 62.6}, wood},
                                       Box{"Étagère", {x - 6.0, 0.0, 66.6}, {x + 6.0, 2.5, 67.2}, wood},
                                       Box{"Enseigne", {x - 2.0, 2.7, 67.1}, {x + 2.0, 3.2, 67.2},
                                           Math::LinearColor{1.0f, 0.5f, 0.15f}, 3.0f, false}}) {
                    addBox(box, shop);
                }
                const Entity merchant = scene.CreateEntity("Place du marchand", shop);
                scene.Get<TransformComponent>(merchant).position = {x, 0.0, 64.5};
                addLight(("Lumière de la " + name).c_str(), {x, Height - 0.3, 61.0}, 10.0f, 12.0f);
                addInterior(("Intérieur de la " + name).c_str(), {x, 0.5 * Height, 61.0}, {14.0f, static_cast<float>(Height), 12.4f}, 1.0f);
            }

            // Salle des terminaux : quatre bornes contre le mur du fond, écran allumé.
            const double terminals = Rooms[2];
            const Entity room = scene.CreateEntity("Salle des terminaux", station);
            for (int i = 0; i < 4; ++i) {
                const double x = terminals - 4.5 + 3.0 * i;
                const Entity terminal = scene.CreateEntity("Terminal " + std::to_string(i + 1), room);
                addBox({"Pied", {x - 0.35, 0.0, 66.6}, {x + 0.35, 1.0, 67.2}, grey(70)}, terminal);
                addBox({"Écran", {x - 0.45, 1.0, 66.75}, {x + 0.45, 1.6, 66.85}, Math::LinearColor{0.2f, 0.8f, 1.0f}, 2.5f}, terminal);
            }
            addLight("Lumière des terminaux", {terminals, Height - 0.3, 61.0}, 10.0f, 12.0f);
            addInterior("Intérieur des terminaux", {terminals, 0.5 * Height, 61.0}, {14.0f, static_cast<float>(Height), 12.4f}, 1.0f);
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
                                           {"Texte à l'écran", "Texte", UI::Icon::Text},
                                           {"Image à l'écran", "Image", UI::Icon::Image},
                                           {"Changement de scène", "Changement de scène", UI::Icon::SceneChange},
                                           {"Personnage...", "Joueur", UI::Icon::Character},
                                           {"Vaisseau (maquette)", "Vaisseau", UI::Icon::Ship},
                                           {"Chasseur", "Chasseur", UI::Icon::Ship},
                                           {"Cible", "Cible", UI::Icon::Cube},
                                           {"Lumière", "Lumière", UI::Icon::Light},
                                           {"Spot", "Spot", UI::Icon::Light},
                                           {"Ciel", "Ciel", UI::Icon::Sky},
                                           {"Volume intérieur", "Intérieur", UI::Icon::Collider},
                                           {"Interface...", "Interface", UI::Icon::Window},
                                           {"Son...", "Son", UI::Icon::Sound},
                                           {"Planète", "Planète", UI::Icon::Planet},
                                           {"Monde (gravité, air)", "Monde", UI::Icon::Planet},
                                           {"Soleil", "Soleil", UI::Icon::Light},
                                           {"Prefab", "Prefab", UI::Icon::Prefab},
                                           {"Station (maquette)", "Station", UI::Icon::Ship}};
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
                            const std::vector<std::filesystem::path>& prefabs) {
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
                choose(ui.Selectable(info.label, info.icon), {item, {}});
            }
        };
        // Catégorie repliable (dépliée au départ) ; en cherchant, toutes s'ouvrent et les vides disparaissent.
        const auto category = [&](std::string_view title, std::initializer_list<Item> items) {
            const bool any = std::ranges::any_of(items, [&](Item item) { return Matches(GetInfo(static_cast<int>(item)).label, m_filter); });
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
            category("Formes", {Item::Empty, Item::Cube, Item::Model});
            category("Lumières", {Item::Light, Item::Spot, Item::Sky, Item::Sun, Item::Interior});
            category("Espace", {Item::Planet, Item::World});
            category("Physique (en mode Jouer)", {Item::Character, Item::Ship, Item::Fighter, Item::Station, Item::Target});
            category("Interface", {Item::ScreenText, Item::ScreenImage, Item::Interface});
            category("Son", {Item::Sound});
            category("Logique", {Item::SceneChange});
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
                        choose(ui.Selectable(label, UI::Icon::Prefab), {Item::Prefab, prefab});
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
            const std::string label = m_held->item == Item::Prefab ? PrefabLabel(m_held->prefab)
                                                                    : GetInfo(static_cast<int>(m_held->item)).label;
            const Vector2f topLeft = mouse + Vector2f{14.0f, 10.0f};
            const Vector2f size = ui.MeasureText(label) + style.framePadding * 2.0f;
            list.AddRect(topLeft, topLeft + size, style.button);
            list.AddText(topLeft + style.framePadding, label, style.fontSize, style.text);
        }
    }

    void PlacePanel::Place(Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, const Choice& choice,
                           std::optional<Vector2f> drop, const UI::Rect& view) {
        history.Commit(engine); // un geste en cours est un pas à part, avant celui-ci
        Scene& scene = engine.GetScene();
        const Item item = choice.item;
        if (item == Item::Sky) {
            // Un seul ciel compte : s'il y en a déjà un, c'est lui qu'on sélectionne (à régler dans l'inspecteur).
            for (const auto [existing, sky] : scene.GetRegistry().view<const SkyComponent>().each()) {
                sceneEditor.Select(scene, existing);
                return;
            }
        } else if (item == Item::World) {
            for (const auto [existing, world] : scene.GetRegistry().view<const WorldComponent>().each()) {
                sceneEditor.Select(scene, existing);
                return;
            }
        } else if (item == Item::Sun) {
            for (const auto [existing, sun] : scene.GetRegistry().view<const SunComponent>().each()) {
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
        Entity entity = NullEntity;
        if (item == Item::Prefab) {
            // Nouvel exemplaire : ses pièces suivront le prefab, comme celles des autres exemplaires.
            const Project* project = engine.GetProject();
            const std::string path = project ? Platform::ToGenericUtf8(choice.prefab.lexically_relative(project->assetsDirectory))
                                             : std::string{};
            auto placed = SceneSerializer::Instantiate(scene, engine.GetComponents(), path, engine.GetPrefabReader());
            if (!placed) {
                Platform::ShowError(engine.GetWindow(), "Placer un prefab", placed.error());
                return;
            }
            entity = *placed;
            name = PrefabLabel(choice.prefab);
        } else {
            entity = scene.CreateEntity(name);
        }

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
        } else if (item == Item::World) {
            scene.Add<WorldComponent>(entity); // sa position ne compte pas
        } else if (item == Item::Sun) {
            // Fixe, au centre de la scène : le système se construit autour de lui (planètes en orbite, Orbits::Update).
            scene.Add<SunComponent>(entity);
        } else {
            if (item == Item::Cube) {
                // Solide dès sa création (immobile) : on marche dessus. « Simuler la physique » (inspecteur, Collision) le fait tomber.
                scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(180, 180, 185));
                scene.Add<ColliderComponent>(entity);
            } else if (item == Item::Target) {
                // Cube de 4 m à abattre : 100 points de vie, 10 obus du Gatling.
                scene.Get<TransformComponent>(entity).scale = {4.0f, 4.0f, 4.0f};
                scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(200, 70, 50));
                scene.Add<ColliderComponent>(entity);
                scene.Add<HealthComponent>(entity);
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
                scene.Add<HealthComponent>(entity, HealthComponent{.health = 500.0f});
                AddFighter(scene, entity);
            } else if (item == Item::Station) {
                // Immobile : ni moteurs ni chute ; sa gravité tient les personnages sur ses ponts.
                ShipComponent& ship = scene.Add<ShipComponent>(entity);
                ship.maxSpeed = 0.0f;
                ship.acceleration = 0.0f;
                ship.turnRate = 0.0f;
                ship.strafeSpeed = 0.0f;
                ship.levelAssist = false;
                ship.fallsWithoutPilot = false;
                AddStation(scene, entity);
            } else if (item == Item::Sound) {
                scene.Add<SoundComponent>(entity).file = std::move(*soundFile);
            } else if (item == Item::Interior) {
                scene.Add<InteriorComponent>(entity);
            } else if (item == Item::Planet) {
                scene.Add<PlanetComponent>(entity);
                scene.Add<CloudsComponent>(entity);
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
            if (item == Item::Station && !drop) {
                // À l'horizontale, l'entrée du hangar face à la caméra et à sa hauteur : regardée d'en haut, la
                // station s'enfoncerait dans le sol de la scène.
                direction = Math::Vector3d(direction.x, 0.0, direction.z);
                direction = Math::Length(direction) > 1.0e-6 ? Math::Normalize(direction) : Math::Vector3d(0.0, 0.0, -1.0);
                position += Math::Vector3d(0.0, -12.0, 0.0);
                scene.Get<TransformComponent>(entity).rotation = Math::Quaternionf::FromAxisAngle(
                    {0.0f, 1.0f, 0.0f}, static_cast<float>(std::atan2(direction.x, direction.z)));
            }
            // Un prefab de vaisseau est vu comme le chasseur : en entier.
            const bool bigPrefab = item == Item::Prefab && scene.TryGet<ShipComponent>(entity);
            double distance = item == Item::Ship ? ShipDistance : item == Item::Fighter || bigPrefab ? FighterDistance
                                                                            : item == Item::Station ? StationDistance : PlaceDistance;
            if (item == Item::Planet) {
                // Assez loin pour la voir en entier.
                distance = PlanetDistance * static_cast<double>(scene.Get<PlanetComponent>(entity).radius);
            }
            position += direction * distance;
            scene.Get<TransformComponent>(entity).position = position.offset;
            if (position.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, position.sector);
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
        // Avec ses enfants (maquette, chasseur, station, prefab) : annuler retire tout, rétablir remet tout.
        history.Push(std::string("Placer : ") + (item == Item::Prefab ? name : std::string(info.label)), scene.GetId(entity), {},
                     UndoHistory::Capture(engine, scene.GetId(entity),
                                          item == Item::Ship || item == Item::Fighter || item == Item::Station ||
                                              item == Item::Prefab));
        if (item == Item::Character) {
            sceneEditor.RequestClaimPlayer(); // le nouveau personnage devient le seul dirigé par le joueur
        }
    }
}
