// Objets du jeu dans le panneau Placer de l'éditeur : chasseurs armés (Chasseur et Manta), cible, stations à quais et donneur de missions.
// Ils utilisent des composants du jeu (emports, armes, quais, missions), que l'éditeur ne connaît pas.
#include "PlaceItems.hpp"

#include "Docking.hpp"
#include "MissionGiver.hpp"
#include "Warp.hpp"
#include "Weapons.hpp"

#include <Flux/Core/EditorExtensions.hpp>
#include <Flux/Scene/Scene.hpp>

#include <cstdint>
#include <numbers>
#include <string>

namespace Game {
    using namespace Flux;

    namespace {
        constexpr double FighterDistance = 32.0; // m de la caméra : le chasseur fait 20 m de long et 15 m d'envergure
        constexpr double StationDistance = 250.0; // la station fait 142 x 31 x 119 m
        constexpr double OrbitalStationDistance = 600.0; // de la porte du hangar du joueur ; la tour fait 4 km de haut

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
            // Levier du train, au bout gauche du tableau de bord : pièce à part (origine sur le pivot, axe x local),
            // posée en bas (train sorti) ; le jeu le relèvera en le tournant autour de son x quand le train rentrera.
            const Entity lever = scene.CreateEntity("Levier du train", ship);
            TransformComponent& leverTransform = scene.Get<TransformComponent>(lever);
            leverTransform.position = {-0.8786, 0.5627, -0.305};
            leverTransform.rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, 68.0f * std::numbers::pi_v<float> / 180.0f);
            scene.Add<ModelComponent>(lever).model = "Modeles/Raie/Raie-levier-train.glb";
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

        // Chasseur « Manta » (nom de code Raie ; modèle du projet, dans Modeles/Raie ; scripts Blender dans Modeles/Chasseur2 des fichiers
        // du projet) : aile volante de 14 m de long et 18,8 m d'envergure, sans dérive. Origine : le siège du pilote,
        // l'avant vers -z ; les roues touchent le sol 1,66 m plus bas. Train et verrière sont des modèles à part pour
        // pouvoir les animer : le train (squelette, animation « Rentrer ») est affiché sorti tant que le jeu ne le
        // rentre pas, la verrière a son origine sur sa charnière arrière (elle s'ouvrira en tournant autour de x).
        void AddManta(Scene& scene, Entity ship) {
            const Entity hull = scene.CreateEntity("Coque", ship);
            ModelComponent& model = scene.Add<ModelComponent>(hull);
            model.model = "Modeles/Raie/Raie.glb";
            model.farModel = "Modeles/Raie/Raie-loin.glb"; // 5 300 triangles au lieu de 53 000
            model.farDistance = 1500.0f;
            ColliderComponent& collider = scene.Add<ColliderComponent>(hull);
            collider.shape = ColliderShape::Mesh;
            collider.mesh = "Modeles/Raie/Raie-collision.glb";
            RimLightComponent& rim = scene.Add<RimLightComponent>(hull);
            rim.intensity = 0.8f;
            rim.sharpness = 4.0f;
            const Entity glass = scene.CreateEntity("Verrière", ship);
            scene.Get<TransformComponent>(glass).position = {0.0, 1.5605, 2.48}; // charnière, derrière la vitre
            scene.Add<ModelComponent>(glass).model = "Modeles/Raie/Raie-verriere-mobile.glb";
            // Collision de la verrière fermée, dans le repère du vaisseau (comme pour le Chasseur).
            const Entity glassStop = scene.CreateEntity("Verrière (collision)", ship);
            ColliderComponent& glassCollider = scene.Add<ColliderComponent>(glassStop);
            glassCollider.shape = ColliderShape::Mesh;
            glassCollider.mesh = "Modeles/Raie/Raie-verriere.glb";
            const Entity gear = scene.CreateEntity("Train d'atterrissage", ship);
            scene.Add<ModelComponent>(gear).model = "Modeles/Raie/Raie-train.glb";
            // Une boîte par roue (roulette avant sous le nez, jambes principales sous l'aile), pour se poser dessus.
            struct Wheel { const char* name; Math::Vector3f center; Math::Vector3f size; };
            for (const Wheel& wheel : {Wheel{"Roue avant", {0.0f, -1.46f, -1.0f}, {0.3f, 0.4f, 0.6f}},
                                       Wheel{"Roue gauche", {-2.4f, -1.46f, 5.9f}, {0.5f, 0.4f, 0.7f}},
                                       Wheel{"Roue droite", {2.4f, -1.46f, 5.9f}, {0.5f, 0.4f, 0.7f}}}) {
                const Entity leg = scene.CreateEntity(wheel.name, ship);
                scene.Get<TransformComponent>(leg).position = {wheel.center.x, wheel.center.y, wheel.center.z};
                scene.Add<ColliderComponent>(leg).size = wheel.size;
            }
            // Levier du train, au bout gauche du tableau de bord : pièce à part (origine sur le pivot, axe x local),
            // posée en bas (train sorti) ; le jeu le relèvera en le tournant autour de son x quand le train rentrera.
            const Entity lever = scene.CreateEntity("Levier du train", ship);
            TransformComponent& leverTransform = scene.Get<TransformComponent>(lever);
            leverTransform.position = {-0.8786, 0.5627, -0.305};
            leverTransform.rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, 68.0f * std::numbers::pi_v<float> / 180.0f);
            scene.Add<ModelComponent>(lever).model = "Modeles/Raie/Raie-levier-train.glb";
            const Entity seat = scene.CreateEntity("Poste de pilotage", ship);
            SeatComponent& component = scene.Add<SeatComponent>(seat);
            component.pilot = true;
            component.reach = 10.0f;
            // En se relevant, le pilote se retrouve au sol devant le nez, côté gauche (sous l'aile, il n'y a que 1,3 m).
            component.exit = {-1.0f, -1.66f, -4.5f};
            // Même œil que le Chasseur : le nez plonge pour qu'il voie 14° sous l'horizon par-dessus le tableau de bord.
            component.eye = {0.0f, 1.15f, 0.2f};
            const Entity engines = scene.CreateEntity("Moteurs", ship);
            scene.Get<TransformComponent>(engines).position = {0.0, 0.1, 9.5};
            SoundComponent& sound = scene.Add<SoundComponent>(engines);
            sound.file = "Sons/Reacteur.wav";
            sound.loop = true;
            sound.followShipSpeed = true;
            sound.minDistance = 6.0f;
            sound.maxDistance = 150.0f;
            // Deux tuyères plates au bord de fuite (0,92 x 0,3 m, face arrière à z 10,8).
            for (const double x : {-0.82, 0.82}) {
                const Entity thruster = scene.CreateEntity(x < 0.0 ? "Réacteur gauche" : "Réacteur droit", ship);
                scene.Get<TransformComponent>(thruster).position = {x, 0.033, 10.81};
                ThrusterComponent& flame = scene.Add<ThrusterComponent>(thruster);
                flame.width = 0.82f;
                flame.height = 0.25f;
                flame.length = 5.0f;
            }
            // Deux canons fixes dans les emplantures (pas de modèle d'arme : le tube fait partie de la coque) ; l'emport
            // est sur la bouche du tube.
            for (const double x : {-1.9, 1.9}) {
                const Entity hardpoint = scene.CreateEntity(x < 0.0 ? "Canon gauche" : "Canon droit", ship);
                scene.Get<TransformComponent>(hardpoint).position = {x, 0.021, -1.81};
                scene.Add<HardpointComponent>(hardpoint, HardpointComponent{.kind = WeaponKind::Gun, .size = 1});
                const Entity weapon = scene.CreateEntity("Canon de 20 mm", hardpoint);
                WeaponComponent& arms = scene.Add<WeaponComponent>(weapon);
                arms.kind = WeaponKind::Gun;
                arms.size = 1;
                arms.cone = 0.0f;
                arms.muzzle = 0.0f;
                arms.group = 1;
            }
        }

        // Station orbitale (modèle du projet, dans Modeles/Station), 4 km de haut. Son origine est le seuil de la porte
        // du hangar du joueur, qui regarde vers -z ; la tour est derrière, vers +z. Derrière le sas de cette porte et
        // son champ bleu (qui se traverse), le hangar du joueur est une pièce de la station, sans chargement (choix de
        // Bruno du 2026-10-27) ; sa plateforme est le quai du pilote automatique (L), qui y pose le vaisseau.
        void AddOrbitalStation(Scene& scene, Entity station) {
            // Immobile (vaisseau sans moteur, comme la maquette) : ses quais servent au pilote automatique, ses enfants
            // (le hangar, ce qu'on y gare, qui y marche) sont à bord, dans sa gravité.
            ShipComponent& ship = scene.Add<ShipComponent>(station);
            ship.maxSpeed = 0.0f;
            ship.acceleration = 0.0f;
            ship.turnRate = 0.0f;
            ship.strafeSpeed = 0.0f;
            ship.levelAssist = false;
            ship.fallsWithoutPilot = false;

            // Hangar de 110 x 40 x 150 m, son entrée contre le fond du sas (z = 16 m), son sol à celui du sas.
            const Entity hangar = scene.CreateEntity("Hangar", station);
            scene.Get<TransformComponent>(hangar).position = {0.0, -0.5, 91.0};
            scene.Add<ModelComponent>(hangar).model = "Modeles/Station/Hangar.glb";
            ColliderComponent& hangarCollider = scene.Add<ColliderComponent>(hangar);
            hangarCollider.shape = ColliderShape::Mesh;
            hangarCollider.mesh = "Modeles/Station/Hangar-collision.glb";
            const Entity platform = scene.CreateEntity("Plateforme", hangar);
            scene.Get<TransformComponent>(platform).position = {0.0, 2.3, 0.0};
            DockComponent& dock = scene.Add<DockComponent>(platform);
            dock.approach = 250.0f;   // point d'approche à 160 m devant la porte
            dock.hover = 8.0f;
            dock.clearance = 2100.0f; // au-dessus de la tête de la tour, en arrivant de derrière
            const Entity inside = scene.CreateEntity("Intérieur du hangar", hangar);
            scene.Get<TransformComponent>(inside).position = {0.0, 20.0, 0.0};
            InteriorComponent& interior = scene.Add<InteriorComponent>(inside);
            interior.size = {110.0f, 40.0f, 150.0f};
            interior.luminance = 30.0f;
            interior.fade = 2.0f;
            for (const double x : {-30.0, 30.0}) {
                for (const double z : {-55.0, -20.0, 20.0, 55.0}) {
                    const Entity light = scene.CreateEntity("Plafonnier", hangar);
                    scene.Get<TransformComponent>(light).position = {x, 36.0, z};
                    LightComponent& component = scene.Add<LightComponent>(light);
                    component.color = {1.0f, 0.95f, 0.88f};
                    component.lumens = 2.0e6f; // ~150 lux au sol, 36 m plus bas (un éclairage de hangar)
                    component.range = 70.0f;
                }
            }

            ModelComponent& model = scene.Add<ModelComponent>(station);
            model.model = "Modeles/Station/Station.glb";
            model.farModel = "Modeles/Station/Station-loin.glb"; // 110 000 triangles au lieu de 850 000
            model.farDistance = 2000.0f;
            ColliderComponent& collider = scene.Add<ColliderComponent>(station);
            collider.shape = ColliderShape::Mesh;
            collider.mesh = "Modeles/Station/Station-collision.glb";
            RimLightComponent& rim = scene.Add<RimLightComponent>(station);
            rim.intensity = 0.5f;
            rim.sharpness = 4.0f;
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
                float luminance = 0.0f; // cd/m²
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
                mesh.luminance = box.luminance;
                if (box.solid) {
                    scene.Add<ColliderComponent>(entity);
                }
                return entity;
            };
            const auto addLight = [&](const char* name, Math::Vector3d position, float lumens, float range) {
                const Entity entity = scene.CreateEntity(name, station);
                scene.Get<TransformComponent>(entity).position = position;
                LightComponent& light = scene.Add<LightComponent>(entity);
                light.color = {1.0f, 0.95f, 0.88f};
                light.lumens = lumens;
                light.range = range;
            };
            const auto addInterior = [&](const char* name, Math::Vector3d center, Math::Vector3f size, float luminance) {
                const Entity entity = scene.CreateEntity(name, station);
                scene.Get<TransformComponent>(entity).position = center;
                InteriorComponent& interior = scene.Add<InteriorComponent>(entity);
                interior.size = size;
                interior.luminance = luminance;
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
                                       Math::LinearColor{0.2f, 0.6f, 1.0f}, 8000.0f, false},
                                   Box{"Balise droite", {49.6, 0.0, -51.2}, {50.0, 24.0, -50.8},
                                       Math::LinearColor{0.2f, 0.6f, 1.0f}, 8000.0f, false}}) {
                addBox(box, hangar);
            }

            // Quatre quais de 24 x 28 m en ligne, au milieu du hangar. Chaque « Quai N » (DockComponent) est le point d'amarrage :
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
                // Point d'approche à 100 m devant (50 m hors de l'entrée), arrivée à 6 m de haut (sous le linteau, à
                // 24 m) ; par-dessus la station (31 m) à 150 m.
                scene.Add<DockComponent>(dock);
                const Entity marks = scene.CreateEntity("Marquage du " + name, hangar);
                const double x0 = x - HalfWidth, x1 = x + HalfWidth, z0 = DockZ - HalfLength, z1 = DockZ + HalfLength;
                // Les bandes sont posées sur la dalle et ne se chevauchent pas (les côtés vont d'un bord à l'autre) : deux
                // faces au même endroit, l'une passe devant l'autre au hasard d'une image à l'autre (scintillement).
                // Lueur modérée (une peinture luminescente) : une bande très lumineuse vue de loin, plus fine qu'un
                // pixel, clignote elle aussi.
                constexpr double Top = 0.02, Paint = 0.04;
                constexpr float Glow = 150.0f;
                for (const Box& box : {Box{"Dalle", {x0, 0.0, z0}, {x1, Top, z1}, grey(60), 0.0f, false},
                                       Box{"Bord avant", {x0, Top, z0}, {x1, Paint, z0 + Line}, yellow, Glow, false},
                                       Box{"Bord arrière", {x0, Top, z1 - Line}, {x1, Paint, z1}, yellow, Glow, false},
                                       Box{"Bord gauche", {x0, Top, z0 + Line}, {x0 + Line, Paint, z1 - Line}, yellow, Glow, false},
                                       Box{"Bord droit", {x1 - Line, Top, z0 + Line}, {x1, Paint, z1 - Line}, yellow, Glow, false},
                                       // Néon au plafond, au-dessus du quai.
                                       Box{"Plafonnier", {x - 4.0, 29.7, DockZ - 4.0}, {x + 4.0, 30.0, DockZ + 4.0},
                                           Math::LinearColor{1.0f, 0.95f, 0.88f}, 6000.0f, false}}) {
                    addBox(box, marks);
                }
                // Lumens réels : ~300 lux sur le quai, 26 m plus bas (en vrai, une rangée de projecteurs).
                addLight(("Lumière du " + name).c_str(), {x, 26.0, DockZ}, 2.5e6f, 45.0f);
            }
            addLight("Lumière de l'entrée", {0.0, 20.0, -35.0}, 1.5e6f, 40.0f);
            addLight("Lumière du fond", {0.0, 20.0, 35.0}, 4.0e5f, 35.0f);
            addInterior("Intérieur du hangar", {0.0, 15.0, 0.0}, {140.0f, 30.0f, 100.0f}, 30.0f);

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
                addLight("Lumière du couloir", {x, Height - 0.3, 52.4}, 5000.0f, 14.0f);
            }
            addInterior("Intérieur du couloir", {0.0, 0.5 * Height, 52.4}, {80.0f, static_cast<float>(Height), 4.0f}, 15.0f);

            // Boutiques : un comptoir, une étagère contre le mur du fond, et la place du marchand entre les deux.
            const Math::LinearColor wood = Math::LinearColor::FromSrgb8(120, 85, 55);
            for (int i = 0; i < 2; ++i) {
                const double x = Rooms[i];
                const std::string name = "Boutique " + std::to_string(i + 1);
                const Entity shop = scene.CreateEntity(name, station);
                for (const Box& box : {Box{"Comptoir", {x - 4.0, 0.0, 61.8}, {x + 4.0, 1.1, 62.6}, wood},
                                       Box{"Étagère", {x - 6.0, 0.0, 66.6}, {x + 6.0, 2.5, 67.2}, wood},
                                       Box{"Enseigne", {x - 2.0, 2.7, 67.1}, {x + 2.0, 3.2, 67.2},
                                           Math::LinearColor{1.0f, 0.5f, 0.15f}, 3000.0f, false}}) {
                    addBox(box, shop);
                }
                const Entity merchant = scene.CreateEntity("Place du marchand", shop);
                scene.Get<TransformComponent>(merchant).position = {x, 0.0, 64.5};
                addLight(("Lumière de la " + name).c_str(), {x, Height - 0.3, 61.0}, 20000.0f, 12.0f);
                addInterior(("Intérieur de la " + name).c_str(), {x, 0.5 * Height, 61.0}, {14.0f, static_cast<float>(Height), 12.4f}, 30.0f);
            }

            // Donneur de missions derrière le comptoir de la première boutique (à 1,8 m du client qui s'y accoude), tourné
            // vers la porte. Il prend le modèle du personnage du joueur s'il y en a un dans la scène, sinon un corps en
            // cube. Personnage : enfant direct de la station (sa gravité, son monde physique).
            const Entity agent = scene.CreateEntity("Donneur de missions", station);
            TransformComponent& agentTransform = scene.Get<TransformComponent>(agent);
            agentTransform.position = {Rooms[0], 0.0, 63.3};
            agentTransform.rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, std::numbers::pi_v<float>); // le modèle regarde vers son +z
            scene.Add<CharacterComponent>(agent).player = false;
            scene.Add<MissionGiverComponent>(agent);
            Entity body = NullEntity;
            for (const auto [entity, character, model] : scene.GetRegistry().view<const CharacterComponent, const ModelComponent>().each()) {
                if (entity != agent && !model.model.empty()) {
                    body = entity;
                    break;
                }
            }
            if (body != NullEntity) {
                scene.Add<ModelComponent>(agent).model = scene.Get<ModelComponent>(body).model;
                if (const auto* animator = scene.TryGet<AnimatorComponent>(body)) {
                    AnimatorComponent idle;
                    idle.animation = animator->idleAnimation.empty() ? animator->animation : animator->idleAnimation;
                    idle.idleAnimation = idle.animation;
                    scene.Add<AnimatorComponent>(agent, std::move(idle));
                }
            } else {
                const Entity cube = scene.CreateEntity("Corps", agent);
                scene.Get<TransformComponent>(cube).position = {0.0, 0.9, 0.0};
                scene.Get<TransformComponent>(cube).scale = {0.5f, 1.8f, 0.3f};
                scene.Add<MeshRendererComponent>(cube, MeshType::Cube, Math::LinearColor::FromSrgb8(60, 110, 170));
            }

            // Salle des terminaux : quatre bornes contre le mur du fond, écran allumé.
            const double terminals = Rooms[2];
            const Entity room = scene.CreateEntity("Salle des terminaux", station);
            for (int i = 0; i < 4; ++i) {
                const double x = terminals - 4.5 + 3.0 * i;
                const Entity terminal = scene.CreateEntity("Terminal " + std::to_string(i + 1), room);
                addBox({"Pied", {x - 0.35, 0.0, 66.6}, {x + 0.35, 1.0, 67.2}, grey(70)}, terminal);
                addBox({"Écran", {x - 0.45, 1.0, 66.75}, {x + 0.45, 1.6, 66.85}, Math::LinearColor{0.2f, 0.8f, 1.0f}, 300.0f}, terminal);
            }
            addLight("Lumière des terminaux", {terminals, Height - 0.3, 61.0}, 20000.0f, 12.0f);
            addInterior("Intérieur des terminaux", {terminals, 0.5 * Height, 61.0}, {14.0f, static_cast<float>(Height), 12.4f}, 25.0f);
        }
    }

    void RegisterPlaceItems(EditorExtensions& extensions) {
        // Même ordre qu'avant dans le panneau : à la suite du personnage et de la maquette de vaisseau de l'éditeur.
        extensions.AddPlaceItem({.category = "Physique (en mode Jouer)", .label = "Chasseur", .name = "Chasseur",
                                 .icon = UI::Icon::Ship, .distance = FighterDistance, .build = [](Scene& scene, Entity entity) {
                                     // Vire vite, pour suivre la souris (30 °/s par défaut, celle d'un transport, paraît mou).
                                     scene.Add<ShipComponent>(entity).turnRate = 90.0f;
                                     scene.Add<HealthComponent>(entity, HealthComponent{.health = 500.0f});
                                     scene.Add<WarpDriveComponent>(entity);
                                     AddFighter(scene, entity);
                                 }});
        extensions.AddPlaceItem({.category = "Physique (en mode Jouer)", .label = "Chasseur Manta", .name = "Manta",
                                 .icon = UI::Icon::Ship, .distance = FighterDistance, .build = [](Scene& scene, Entity entity) {
                                     scene.Add<ShipComponent>(entity).turnRate = 90.0f;
                                     scene.Add<HealthComponent>(entity, HealthComponent{.health = 400.0f});
                                     scene.Add<WarpDriveComponent>(entity);
                                     AddManta(scene, entity);
                                 }});
        extensions.AddPlaceItem({.category = "Physique (en mode Jouer)", .label = "Station (maquette)", .name = "Station",
                                 .icon = UI::Icon::Ship, .distance = StationDistance, .level = true, .below = 12.0,
                                 .build = [](Scene& scene, Entity entity) {
                                     // Immobile : ni moteurs, ni chute, ni warp ; sa gravité tient les personnages sur ses ponts.
                                     ShipComponent& ship = scene.Add<ShipComponent>(entity);
                                     ship.maxSpeed = 0.0f;
                                     ship.acceleration = 0.0f;
                                     ship.turnRate = 0.0f;
                                     ship.strafeSpeed = 0.0f;
                                     ship.levelAssist = false;
                                     ship.fallsWithoutPilot = false;
                                     AddStation(scene, entity);
                                 }});
        extensions.AddPlaceItem({.category = "Physique (en mode Jouer)", .label = "Cible", .name = "Cible", .icon = UI::Icon::Cube,
                                 .build = [](Scene& scene, Entity entity) {
                                     // Cube de 4 m à abattre : 100 points de vie, 10 obus du Gatling.
                                     scene.Get<TransformComponent>(entity).scale = {4.0f, 4.0f, 4.0f};
                                     scene.Add<MeshRendererComponent>(entity, MeshType::Cube, Math::LinearColor::FromSrgb8(200, 70, 50));
                                     scene.Add<ColliderComponent>(entity);
                                     scene.Add<HealthComponent>(entity);
                                 }});
        // Porte du hangar : 34 m de haut, à hauteur de la caméra.
        extensions.AddPlaceItem({.category = "Espace", .label = "Station orbitale", .name = "Station orbitale", .icon = UI::Icon::Ship,
                                 .distance = OrbitalStationDistance, .level = true, .below = 17.0, .dropToGround = false,
                                 .build = [](Scene& scene, Entity entity) { AddOrbitalStation(scene, entity); }});
    }
}
