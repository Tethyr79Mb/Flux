// Armes des vaisseaux (Weapons) : emports, visée dans le cône, cadence, projectiles, sauvegarde.
#include "Check.hpp"

#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Weapons.hpp"

#include <cmath>
#include <numbers>
#include <string>
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
    // Visée à 45° sur la droite : hors du cône, la tourelle s'arrête à 20°, à 90°/s (pas encore là en 0,1 s).
    WeaponInput input;
    input.aim = true;
    input.aimPoint = scene.GetWorldPosition(mount);
    input.aimPoint += Math::Vector3d{100.0, 0.0, -100.0};
    Weapons::Update(scene, ship, 0.1, input, projectiles);
    CHECK(Near(std::acos(-Barrel(scene, gun).z), 9.0 * std::numbers::pi / 180.0, 1.0e-3));
    for (int i = 0; i < 10; ++i) {
        Weapons::Update(scene, ship, 0.1, input, projectiles);
    }
    const Math::Vector3f aimed = Barrel(scene, gun);
    CHECK(Near(std::acos(-aimed.z), 20.0 * std::numbers::pi / 180.0, 1.0e-3));
    CHECK(aimed.x > 0.0f && Near(aimed.y, 0.0, 1.0e-4));
    CHECK(projectiles.empty()); // visée sans tir

    // Tir : 10 par seconde, depuis la bouche du canon, vitesse du vaisseau comprise.
    input.fire = true;
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
    return true;
}
