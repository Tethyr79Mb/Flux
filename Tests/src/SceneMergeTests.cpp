// Tests de la fusion à trois versions (partie reprise dans une scène modifiée dans l'éditeur) : ce que la partie a
// changé est repris, ce que l'éditeur a changé reste, l'éditeur l'emporte sur un même champ.
#include "Check.hpp"

#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneMerge.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <string>

using namespace Flux;

namespace {
    // Scene ne se copie pas : chaque version est relue depuis le texte de la scène de départ.
    bool Copy(Scene& scene, const ComponentRegistry& components, const std::string& text) {
        return SceneSerializer::LoadFromString(scene, components, text).has_value();
    }
}

bool RunSceneMergeTests() {
    ComponentRegistry components;
    RegisterEngineComponents(components);

    // Scène de départ : un soleil, une station avec un quai, un vaisseau et son pilote, un pirate, une caisse.
    Scene start;
    const Entity sun = start.CreateEntity("Soleil");
    start.Add<LightComponent>(sun).type = LightType::Spot;
    const Entity station = start.CreateEntity("Station");
    start.Add<ShipComponent>(station).fallsWithoutPilot = false;
    const Entity dock = start.CreateEntity("Quai", station);
    const Entity ship = start.CreateEntity("Vaisseau");
    start.Add<ShipComponent>(ship);
    const Entity player = start.CreateEntity("Joueur", ship);
    start.Add<CharacterComponent>(player);
    const Entity pirate = start.CreateEntity("Pirate");
    start.Add<ShipComponent>(pirate);
    const Entity crate = start.CreateEntity("Caisse");
    const Uuid sunId = start.GetId(sun);
    const Uuid stationId = start.GetId(station);
    const Uuid dockId = start.GetId(dock);
    const Uuid shipId = start.GetId(ship);
    const Uuid playerId = start.GetId(player);
    const Uuid pirateId = start.GetId(pirate);
    const Uuid crateId = start.GetId(crate);
    const std::string text = SceneSerializer::SaveToString(start, components);

    // Partie sauvegardée : le vaisseau a volé, le joueur a marché, le pirate est abattu, un missile est apparu, la
    // caisse a été poussée.
    Scene saved;
    CHECK(Copy(saved, components, text));
    {
        const Entity savedShip = saved.FindById(shipId);
        saved.Get<TransformComponent>(savedShip).position = {100.0, 20.0, -3000.0};
        saved.Get<ShipComponent>(savedShip).velocity = {0.0f, 0.0f, -80.0f};
        saved.Get<TransformComponent>(saved.FindById(playerId)).position = {1.0, 0.0, 2.0};
        saved.DestroyEntity(saved.FindById(pirateId));
        saved.CreateEntity("Missile", savedShip);
        saved.Get<TransformComponent>(saved.FindById(crateId)).position = {5.0, 0.0, 0.0};
    }

    // Scène modifiée dans l'éditeur : soleil rouge, vitesse max du vaisseau, caisse déplacée aussi, quai supprimé.
    Scene edited;
    CHECK(Copy(edited, components, text));
    Scene base;
    CHECK(Copy(base, components, text));
    edited.Get<LightComponent>(edited.FindById(sunId)).color = {1.0f, 0.2f, 0.1f};
    edited.Get<ShipComponent>(edited.FindById(shipId)).maxSpeed = 250.0f;
    edited.Get<TransformComponent>(edited.FindById(crateId)).position = {0.0, 0.0, 7.0};
    edited.DestroyEntity(edited.FindById(dockId));

    CHECK(SceneMerge::ApplyChanges(edited, base, saved, components).empty());
    // Ce que l'éditeur a changé reste.
    CHECK(edited.Get<LightComponent>(edited.FindById(sunId)).color.g == 0.2f);
    CHECK(edited.FindById(dockId) == NullEntity);
    // Ce que la partie a changé est repris, champ par champ dans un même composant.
    const Entity mergedShip = edited.FindById(shipId);
    CHECK(edited.Get<TransformComponent>(mergedShip).position == Math::Vector3d(100.0, 20.0, -3000.0));
    CHECK(edited.Get<ShipComponent>(mergedShip).velocity.z == -80.0f);
    CHECK(edited.Get<ShipComponent>(mergedShip).maxSpeed == 250.0f);
    CHECK(edited.Get<TransformComponent>(edited.FindById(playerId)).position == Math::Vector3d(1.0, 0.0, 2.0));
    CHECK(edited.FindById(pirateId) == NullEntity);
    const Entity missile = edited.FindByName("Missile");
    CHECK(missile != NullEntity && edited.GetParent(missile) == mergedShip);
    // Même champ changé des deux côtés : l'éditeur l'emporte.
    CHECK(edited.Get<TransformComponent>(edited.FindById(crateId)).position == Math::Vector3d(0.0, 0.0, 7.0));

    // Rattachement (vaisseau amarré à la station pendant la partie) ; parent supprimé dans l'éditeur : signalé, rien
    // d'autre n'est perdu.
    {
        Scene docked;
        CHECK(Copy(docked, components, text));
        docked.SetParent(docked.FindById(shipId), docked.FindById(stationId));
        docked.Get<ShipComponent>(docked.FindById(pirateId)).velocity.x = 5.0f;
        docked.SetParent(docked.FindById(crateId), docked.FindById(dockId));

        Scene target;
        CHECK(Copy(target, components, text));
        target.DestroyEntity(target.FindById(dockId));
        const std::vector<std::string> problems = SceneMerge::ApplyChanges(target, base, docked, components);
        CHECK(problems.size() == 1 && problems[0].find("Caisse") != std::string::npos);
        CHECK(target.GetParent(target.FindById(shipId)) == target.FindById(stationId));
        CHECK(target.Get<ShipComponent>(target.FindById(pirateId)).velocity.x == 5.0f);
        CHECK(target.GetParent(target.FindById(crateId)) == NullEntity);
    }

    // Rien de changé par la partie : la scène modifiée reste telle quelle.
    {
        Scene target;
        CHECK(Copy(target, components, text));
        target.Get<LightComponent>(target.FindById(sunId)).intensity = 2.0f;
        CHECK(SceneMerge::ApplyChanges(target, base, base, components).empty());
        CHECK(SceneSerializer::SaveToString(target, components) != text);
        CHECK(target.Get<LightComponent>(target.FindById(sunId)).intensity == 2.0f);
        CHECK(target.GetEntityCount() == start.GetEntityCount());
    }
    return true;
}
