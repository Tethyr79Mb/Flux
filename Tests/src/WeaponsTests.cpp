// Armes des vaisseaux (Weapons) : emports, visée dans le cône, cadence, projectiles, dégâts, missiles guidés, sauvegarde.
#include "Check.hpp"

#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Weapons.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Flux;

namespace {
    bool Near(double a, double b, double tolerance = 1.0e-3) {
        return std::abs(a - b) <= tolerance;
    }

    // Direction (monde) du canon de l'arme.
    Math::Vector3f Barrel(const Scene& scene, Entity weapon) {
        return scene.GetWorldRotation(weapon).Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
    }
}

namespace {
    // Missiles : verrouillage au centre de la vue (gardé hors du viseur), tir alterné des deux lance-missiles jusqu'à épuisement, poursuite
    // (en avance sur un vaisseau qui vole), cible détruite en vol, sauvegarde du nombre emporté.
    bool RunMissileTests() {
        Scene scene;
        const Entity ship = scene.CreateEntity("Chasseur");
        scene.Add<ShipComponent>(ship);
        scene.Add<HealthComponent>(ship);
        const Entity hull = scene.CreateEntity("Coque", ship);
        scene.Add<HealthComponent>(hull); // sa propre pièce : jamais verrouillée
        std::vector<Entity> launchers;
        for (const double x : {-5.0, 5.0}) {
            const Entity mount = scene.CreateEntity("Emport", ship);
            scene.Get<TransformComponent>(mount).position = {x, 0.0, 0.0};
            scene.Add<HardpointComponent>(mount).kind = WeaponKind::Missile;
            const Entity launcher = scene.CreateEntity("Lance-missiles", mount);
            WeaponComponent& weapon = scene.Add<WeaponComponent>(launcher);
            weapon.kind = WeaponKind::Missile;
            weapon.cone = 0.0f;
            weapon.fireRate = 1.0f;
            weapon.projectileSpeed = 100.0f;
            weapon.range = 2000.0f;
            weapon.ammo = 2;
            launchers.push_back(launcher);
        }
        const Entity ahead = scene.CreateEntity("Devant");
        scene.Get<TransformComponent>(ahead).position = {0.0, 0.0, -500.0};
        scene.Add<HealthComponent>(ahead);
        const Entity aside = scene.CreateEntity("De côté");
        scene.Get<TransformComponent>(aside).position = {300.0, 0.0, -500.0};
        scene.Add<HealthComponent>(aside);
        const Entity wreck = scene.CreateEntity("Épave");
        scene.Get<TransformComponent>(wreck).position = {0.0, 0.0, -400.0};
        scene.Add<HealthComponent>(wreck).health = 0.0f; // plus de points de vie : pas une cible
        const Entity far = scene.CreateEntity("Trop loin");
        scene.Get<TransformComponent>(far).position = {0.0, 0.0, -5000.0};
        scene.Add<HealthComponent>(far);

        // Verrouillage : la cible au centre de la vue, une autre à 31° seulement si on la regarde, rien en l'air.
        const Math::WorldPosition eye = scene.GetWorldPosition(ship);
        CHECK(Weapons::FindLockTarget(scene, ship, eye, {0.0f, 0.0f, -1.0f}) == ahead);
        CHECK(Weapons::FindLockTarget(scene, ship, eye, Math::Normalize(Math::Vector3f{300.0f, 0.0f, -500.0f})) == aside);
        CHECK(Weapons::FindLockTarget(scene, ship, eye, {0.0f, 1.0f, 0.0f}) == NullEntity);
        CHECK(Weapons::FindLockTarget(scene, NullEntity, eye, {0.0f, 0.0f, -1.0f}) == NullEntity);
        CHECK(Weapons::GetMissileCount(scene, ship) == 4);

        // Un missile par appui, les deux ailes à tour de rôle, le temps de rechargement de chacune respecté.
        std::vector<Projectile> projectiles;
        WeaponInput input;
        input.pressed = 1; // groupe 1 : les deux lance-missiles
        input.target = far; // hors de portée : pas de tir
        Weapons::Update(scene, ship, 0.01, input, projectiles);
        CHECK(projectiles.empty());
        input.target = ahead;
        Weapons::Update(scene, ship, 0.01, input, projectiles);
        CHECK(projectiles.size() == 1);
        CHECK(projectiles[0].missile && projectiles[0].target == ahead && projectiles[0].ship == ship);
        CHECK(Near(projectiles[0].guidance, 90.0 * std::numbers::pi / 180.0, 1.0e-4));
        Weapons::Update(scene, ship, 0.01, input, projectiles);
        CHECK(projectiles.size() == 2);
        CHECK(Near(Math::Delta(projectiles[0].position, projectiles[1].position).x, 10.0, 1.0e-3)
              || Near(Math::Delta(projectiles[0].position, projectiles[1].position).x, -10.0, 1.0e-3));
        Weapons::Update(scene, ship, 0.01, input, projectiles);
        CHECK(projectiles.size() == 2); // les deux rechargent
        Weapons::Update(scene, ship, 0.01, WeaponInput{}, projectiles);
        CHECK(projectiles.size() == 2);
        CHECK(Weapons::GetMissileCount(scene, ship) == 2);
        Weapons::Update(scene, ship, 1.0, input, projectiles);
        Weapons::Update(scene, ship, 0.01, input, projectiles);
        CHECK(projectiles.size() == 4 && Weapons::GetMissileCount(scene, ship) == 0);
        Weapons::Update(scene, ship, 1.0, input, projectiles);
        CHECK(projectiles.size() == 4); // vides
        CHECK(Weapons::FindLockTarget(scene, ship, eye, {0.0f, 0.0f, -1.0f}) == ahead); // le verrou ne demande pas de munitions

        // Le verrou tient hors du viseur, tant que la cible a des points de vie et reste à portée.
        CHECK(Weapons::KeepsLock(scene, ship, aside, eye));
        CHECK(!Weapons::KeepsLock(scene, ship, far, eye));
        CHECK(!Weapons::KeepsLock(scene, ship, wreck, eye));
        CHECK(!Weapons::KeepsLock(scene, ship, NullEntity, eye));
        CHECK(!Weapons::KeepsLock(scene, NullEntity, aside, eye));

        // Poursuite : le missile vire d'au plus son guidage, garde sa vitesse et finit sur la cible.
        projectiles.clear();
        Projectile missile;
        missile.position = eye;
        missile.velocity = {0.0f, 0.0f, -100.0f};
        missile.life = 20.0f;
        missile.missile = true;
        missile.target = aside;
        missile.guidance = 90.0f * std::numbers::pi_v<float> / 180.0f;
        projectiles.push_back(missile);
        Weapons::GuideMissiles(scene, projectiles, 0.1);
        CHECK(Near(std::acos(-projectiles[0].velocity.z / 100.0f), 9.0 * std::numbers::pi / 180.0, 1.0e-3));
        CHECK(projectiles[0].velocity.x > 0.0f && Near(Math::Length(projectiles[0].velocity), 100.0, 1.0e-3));
        double closest = 1.0e9;
        for (int i = 0; i < 200 && !projectiles.empty(); ++i) {
            Weapons::GuideMissiles(scene, projectiles, 0.05);
            Weapons::UpdateProjectiles(projectiles, 0.05);
            if (!projectiles.empty()) {
                closest = std::min(closest, Math::Length(Math::Delta(projectiles[0].position, scene.GetWorldPosition(aside))));
            }
        }
        CHECK(closest < 5.0);

        // Un vaisseau qui file sur le côté : visé en avance (sans avance, le missile irait tout droit vers -z).
        const Entity fleeing = scene.CreateEntity("Fuyard");
        scene.Get<TransformComponent>(fleeing).position = {0.0, 0.0, -500.0};
        scene.Add<ShipComponent>(fleeing).velocity = {50.0f, 0.0f, 0.0f};
        projectiles.assign(1, missile);
        projectiles[0].target = fleeing;
        projectiles[0].guidance = 100.0f; // vire d'un coup : on lit la direction visée
        Weapons::GuideMissiles(scene, projectiles, 0.1);
        CHECK(Near(projectiles[0].velocity.x / -projectiles[0].velocity.z, 0.5, 0.05));

        // Cible détruite en vol : le missile continue tout droit.
        const Math::Vector3f before = projectiles[0].velocity;
        scene.DestroyEntity(fleeing);
        Weapons::GuideMissiles(scene, projectiles, 0.1);
        CHECK(projectiles[0].target == NullEntity && Math::Length(projectiles[0].velocity - before) < 1.0e-4f);

        // Un missile qui touche : l'impact le dit (explosion plus grande).
        std::vector<ProjectileHit> hits;
        Weapons::UpdateProjectiles(
            projectiles, 0.1,
            [&](const Math::WorldPosition&, const Math::Vector3f&, Entity) -> std::optional<std::pair<Entity, float>> {
                return std::pair{ahead, 0.5f};
            },
            &hits);
        CHECK(hits.size() == 1 && hits[0].missile && projectiles.empty());

        // Sauvegarde : le nombre emporté, pas les missiles déjà tirés.
        ComponentRegistry components;
        RegisterEngineComponents(components);
        const std::string text = SceneSerializer::SaveToString(scene, components);
        CHECK(text.find("\"fired\"") == std::string::npos);
        Scene loaded;
        CHECK(SceneSerializer::LoadFromString(loaded, components, text).has_value());
        const auto* loadedLauncher = loaded.TryGet<WeaponComponent>(loaded.FindByName("Lance-missiles"));
        CHECK(loadedLauncher && loadedLauncher->ammo == 2 && loadedLauncher->fired == 0 && loadedLauncher->guidance == 90.0f);
        return true;
    }
}

namespace {
    // Projectiles contre les corps de la physique : le tireur n'est pas touché, la cible perd ses points de vie et
    // disparaît à 0, sauf si elle porte le joueur.
    bool RunWeaponHitTests() {
        Scene scene;
        const Entity shooter = scene.CreateEntity("Tireur");
        scene.Add<ShipComponent>(shooter);
        const Entity hull = scene.CreateEntity("Coque", shooter);
        scene.Get<TransformComponent>(hull).scale = {4.0f, 2.0f, 6.0f};
        scene.Add<ColliderComponent>(hull);
        const Entity target = scene.CreateEntity("Cible");
        TransformComponent& transform = scene.Get<TransformComponent>(target);
        transform.position = {0.0, 0.0, -50.0};
        transform.scale = {4.0f, 4.0f, 4.0f}; // face avant à z = -48
        scene.Add<HealthComponent>(target).health = 25.0f;
        const Entity part = scene.CreateEntity("Blindage", target); // un enfant touché abîme la cible
        scene.Get<TransformComponent>(part).position = {0.0, 3.0, 0.0};
        scene.Add<ColliderComponent>(part);
        scene.Add<ColliderComponent>(target);

        PhysicsWorld world;
        world.Update(scene, 0.0, {});
        const Math::WorldPosition origin{};
        // Le rayon part de la coque du tireur : elle compte (vue du dehors, c'est le vaisseau), sauf pour ses tirs.
        const auto own = world.CastRay(scene, origin, {0.0f, 0.0f, -100.0f});
        CHECK(own && own->entity == shooter && own->fraction == 0.0f);
        const auto aimed = world.CastRay(scene, origin, {0.0f, 0.0f, -100.0f}, shooter);
        CHECK(aimed && aimed->entity == target && Near(aimed->fraction, 0.48, 1.0e-3));
        CHECK(!world.CastRay(scene, origin, {0.0f, 0.0f, -40.0f}, shooter));
        CHECK(Weapons::FindTarget(scene, part) == target && Weapons::FindTarget(scene, hull) == NullEntity);

        const ProjectileCast cast = [&](const Math::WorldPosition& from, const Math::Vector3f& segment,
                                                 Entity ship) -> std::optional<std::pair<Entity, float>> {
            if (const auto hit = world.CastRay(scene, from, segment, ship)) {
                return std::pair{hit->entity, hit->fraction};
            }
            return std::nullopt;
        };
        std::vector<Projectile> projectiles;
        Projectile shot;
        shot.velocity = {0.0f, 0.0f, -1000.0f};
        shot.life = 1.0f;
        shot.damage = 10.0f;
        shot.ship = shooter;
        std::vector<ProjectileHit> hits;
        std::vector<std::string> destroyed;
        // 20 m par image : rien les deux premières, l'impact à la troisième, sur la face avant.
        projectiles.push_back(shot);
        Weapons::UpdateProjectiles(projectiles, 0.02, cast, &hits);
        Weapons::UpdateProjectiles(projectiles, 0.02, cast, &hits);
        CHECK(hits.empty() && projectiles.size() == 1);
        Weapons::UpdateProjectiles(projectiles, 0.02, cast, &hits);
        CHECK(hits.size() == 1 && projectiles.empty());
        CHECK(hits[0].entity == target && Near(hits[0].position.offset.z, -48.0, 1.0e-2) && hits[0].damage == 10.0f);
        Weapons::ApplyHits(scene, hits, NullEntity, &destroyed);
        CHECK(scene.Get<HealthComponent>(target).health == 15.0f && destroyed.empty());

        // Deux impacts de plus : détruite, avec ses enfants ; un impact sur une entité disparue ne casse rien.
        const std::vector<ProjectileHit> two{hits[0], hits[0], hits[0]};
        Weapons::ApplyHits(scene, two, NullEntity, &destroyed);
        CHECK(!scene.IsValid(target) && !scene.IsValid(part));
        CHECK(destroyed.size() == 1 && destroyed[0] == "Cible");

        // Une cible qui porte le joueur reste à 0 sans disparaître.
        const Entity carrier = scene.CreateEntity("Porteur");
        scene.Add<HealthComponent>(carrier).health = 5.0f;
        const Entity pilot = scene.CreateEntity("Pilote", carrier);
        const ProjectileHit onCarrier{carrier, {}, 10.0f, {}};
        Weapons::ApplyHits(scene, std::span(&onCarrier, 1), pilot, nullptr);
        CHECK(scene.IsValid(carrier) && scene.Get<HealthComponent>(carrier).health == 0.0f);

        // Les éclats d'impact s'éteignent.
        std::vector<Impact> impacts{{{}, {}, 0.1f}, {{}, {}, 0.3f}};
        Weapons::UpdateImpacts(impacts, 0.2);
        CHECK(impacts.size() == 1);
        return true;
    }
}

bool RunWeaponsTests() {
    // Emport : même genre, pas plus grande que lui ; le refus dit pourquoi.
    const HardpointComponent gunMount{.kind = WeaponKind::Gun, .size = 2};
    CHECK(Weapons::Fits(gunMount, WeaponComponent{.kind = WeaponKind::Gun, .size = 1}));
    CHECK(Weapons::Fits(gunMount, WeaponComponent{.kind = WeaponKind::Gun, .size = 2}));
    CHECK(!Weapons::Fits(gunMount, WeaponComponent{.kind = WeaponKind::Gun, .size = 3}));
    CHECK(!Weapons::Fits(gunMount, WeaponComponent{.kind = WeaponKind::Missile, .size = 1}));
    CHECK(Weapons::GetRefusal(gunMount, WeaponComponent{.kind = WeaponKind::Gun, .size = 1}).empty());
    CHECK(Weapons::GetRefusal(gunMount, WeaponComponent{.kind = WeaponKind::Missile}).find("lance-missiles") != std::string::npos);
    CHECK(Weapons::GetRefusal(gunMount, WeaponComponent{.kind = WeaponKind::Gun, .size = 3}).find("taille 3") != std::string::npos);

    // Un vaisseau qui file vers -z à 100 m/s, un emport 2 m sous lui, une tourelle dessus.
    Scene scene;
    const Entity ship = scene.CreateEntity("Chasseur");
    scene.Add<ShipComponent>(ship).velocity = {0.0f, 0.0f, -100.0f};
    const Entity mount = scene.CreateEntity("Emport", ship);
    scene.Get<TransformComponent>(mount).position = {0.0, -2.0, 0.0};
    scene.Add<HardpointComponent>(mount);
    const Entity gun = scene.CreateEntity("Canon", mount);
    WeaponComponent& weapon = scene.Add<WeaponComponent>(gun);
    weapon.cone = 20.0f;
    weapon.turnRate = 90.0f;
    weapon.fireRate = 10.0f;
    weapon.muzzle = 2.0f;

    std::vector<Projectile> projectiles;
    // Cible verrouillée à 45° sur la droite (un peu plus avec l'avance) : hors du cône, la tourelle s'arrête à 20°, à
    // 90°/s (pas encore là en 0,1 s).
    const Entity target = scene.CreateEntity("Cible");
    scene.Get<TransformComponent>(target).position = {100.0, -2.0, -100.0};
    scene.Add<HealthComponent>(target);
    WeaponInput input;
    input.target = target;
    Weapons::Update(scene, ship, 0.1, input, projectiles);
    CHECK(Near(std::acos(-Barrel(scene, gun).z), 9.0 * std::numbers::pi / 180.0, 1.0e-3));
    for (int i = 0; i < 10; ++i) {
        Weapons::Update(scene, ship, 0.1, input, projectiles);
    }
    const Math::Vector3f aimed = Barrel(scene, gun);
    CHECK(Near(std::acos(-aimed.z), 20.0 * std::numbers::pi / 180.0, 1.0e-3));
    CHECK(aimed.x > 0.0f && Near(aimed.y, 0.0, 1.0e-4));
    CHECK(projectiles.empty()); // visée sans tir

    // Touche tenue sans cible verrouillée, ou cible hors de portée : rien ne part.
    input.held = 1;
    input.target = NullEntity;
    Weapons::Update(scene, ship, 0.1, input, projectiles);
    CHECK(projectiles.empty());
    input.target = target;
    weapon.range = 100.0f;
    Weapons::Update(scene, ship, 0.1, input, projectiles);
    CHECK(projectiles.empty());
    weapon.range = 2500.0f;

    // Tir : 10 par seconde, depuis la bouche du canon, vitesse du vaisseau comprise.
    for (int i = 0; i < 10; ++i) {
        Weapons::Update(scene, ship, 0.1, input, projectiles);
    }
    CHECK(projectiles.size() == 10);
    const Projectile& first = projectiles.front();
    const Math::Vector3d fromMount = Math::Delta(scene.GetWorldPosition(mount), first.position);
    CHECK(Near(Math::Length(fromMount), 2.0, 1.0e-3));
    const Math::Vector3f expected = Math::Vector3f{0.0f, 0.0f, -100.0f} + aimed * weapon.projectileSpeed;
    CHECK(Math::Length(first.velocity - expected) < 1.0e-2f);
    CHECK(Near(first.life, weapon.range / weapon.projectileSpeed));
    CHECK(first.ship == ship);
    // Une longue image ne rattrape pas toute la cadence d'un coup.
    projectiles.clear();
    Weapons::Update(scene, ship, 5.0, input, projectiles);
    CHECK(projectiles.size() <= 4);

    // Les projectiles avancent puis disparaissent en bout de portée.
    projectiles.clear();
    weapon.cooldown = 0.0f;
    Weapons::Update(scene, ship, 0.01, input, projectiles);
    CHECK(projectiles.size() == 1);
    const Math::WorldPosition start = projectiles.front().position;
    Weapons::UpdateProjectiles(projectiles, 0.5);
    CHECK(Near(Math::Length(Math::Delta(start, projectiles.front().position)), 0.5 * Math::Length(expected), 1.0e-2));
    CHECK(Near(projectiles.front().age, 0.5));
    Weapons::UpdateProjectiles(projectiles, 2.0);
    CHECK(projectiles.empty());

    // Sans visée, la tourelle revient vers l'avant de l'emport.
    for (int i = 0; i < 10; ++i) {
        Weapons::Update(scene, ship, 0.1, WeaponInput{}, projectiles);
    }
    CHECK(Near(Barrel(scene, gun).z, -1.0, 1.0e-4));

    // Arme refusée par son emport, ou vaisseau qui n'est pas piloté : ni visée ni tir.
    projectiles.clear();
    weapon.kind = WeaponKind::Missile;
    weapon.cooldown = 0.0f;
    Weapons::Update(scene, ship, 0.1, input, projectiles);
    CHECK(projectiles.empty() && Near(Barrel(scene, gun).z, -1.0, 1.0e-4));
    weapon.kind = WeaponKind::Gun;
    // Touche d'un autre groupe que celui de l'arme : elle ne tire pas.
    weapon.group = 2;
    Weapons::Update(scene, ship, 0.1, input, projectiles);
    CHECK(projectiles.empty());
    input.held = 1u << 1;
    Weapons::Update(scene, ship, 0.1, input, projectiles);
    CHECK(projectiles.size() == 1);
    projectiles.clear();
    input.held = 1;
    weapon.group = 1;
    Weapons::Update(scene, NullEntity, 0.1, input, projectiles);
    CHECK(projectiles.empty());
    const Entity other = scene.CreateEntity("Autre vaisseau");
    scene.Add<ShipComponent>(other);
    Weapons::Update(scene, other, 0.1, input, projectiles);
    CHECK(projectiles.empty());

    // Sauvegarde : emport et arme, sans le temps avant le prochain tir.
    weapon.cooldown = 0.3f;
    ComponentRegistry components;
    RegisterEngineComponents(components);
    const std::string text = SceneSerializer::SaveToString(scene, components);
    CHECK(text.find("cooldown") == std::string::npos);
    Scene loaded;
    CHECK(SceneSerializer::LoadFromString(loaded, components, text).has_value());
    const auto* loadedMount = loaded.TryGet<HardpointComponent>(loaded.FindByName("Emport"));
    const auto* loadedGun = loaded.TryGet<WeaponComponent>(loaded.FindByName("Canon"));
    CHECK(loadedMount && loadedMount->kind == WeaponKind::Gun && loadedMount->size == 1);
    CHECK(loadedGun && loadedGun->fireRate == 10.0f && loadedGun->cone == 20.0f && loadedGun->muzzle == 2.0f);
    CHECK(loadedGun->cooldown == 0.0f);
    return RunWeaponHitTests() && RunMissileTests();
}
