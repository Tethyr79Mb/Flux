// Tests des prefabs : un objet tout monté enregistré à part, dont chaque exemplaire d'une scène suit le fichier et ne
// garde que ce qui en diffère.
#include "Check.hpp"

#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <map>
#include <string>

using namespace Flux;

namespace {
    Entity FindByName(const Scene& scene, std::string_view name) {
        for (const auto [entity, component] : scene.GetRegistry().view<const NameComponent>().each()) {
            if (component.name == name) {
                return entity;
            }
        }
        return NullEntity;
    }

    // Le premier enfant de `parent` qui porte ce nom.
    Entity FindChild(const Scene& scene, Entity parent, std::string_view name) {
        Entity found = NullEntity;
        scene.ForEachChild(parent, [&](Entity child) {
            if (found == NullEntity && scene.Get<NameComponent>(child).name == name) {
                found = child;
            }
        });
        return found;
    }
}

bool RunPrefabTests() {
    ComponentRegistry components;
    RegisterEngineComponents(components);

    // Un vaisseau : coque (racine), siège, lampe sous le siège.
    Scene scene;
    const Entity ship = scene.CreateEntity("Vaisseau");
    scene.Get<TransformComponent>(ship).position = {100.0, 2.0, -50.0};
    scene.Add<ShipComponent>(ship);
    const Entity seat = scene.CreateEntity("Siège", ship);
    scene.Get<TransformComponent>(seat).position = {0.0, 1.0, -3.0};
    scene.Add<SeatComponent>(seat).reach = 2.0f;
    const Entity lamp = scene.CreateEntity("Lampe", seat);
    scene.Add<LightComponent>(lamp).intensity = 3.0f;

    // Prefab tiré du vaisseau : la racine à l'origine ; le vaisseau devient son exemplaire sans changer d'identifiants.
    const std::string path = "Prefabs/Vaisseau.prefab.json";
    const auto prefab = SceneSerializer::SavePrefab(scene, components, ship, SceneSerializer::PrefabIds::ForInstance);
    CHECK(prefab.has_value());
    std::map<std::string, std::string, std::less<>> files{{path, *prefab}};
    const SceneSerializer::PrefabReader reader = [&](std::string_view file) -> std::expected<std::string, std::string> {
        const auto found = files.find(file);
        return found != files.end() ? std::expected<std::string, std::string>(found->second) : std::unexpected("absent");
    };
    const Uuid shipId = scene.GetId(ship);
    const Uuid seatId = scene.GetId(seat);
    const Uuid lampId = scene.GetId(lamp);
    SceneSerializer::LinkPrefab(scene, ship, path);
    CHECK(scene.Get<PrefabLinkComponent>(lamp).instance == ship);

    // Dans la scène : la racine, le chemin du prefab et sa seule différence (sa place) ; pas les pièces.
    const std::string saved = SceneSerializer::SaveToString(scene, components, reader);
    CHECK(saved.find("\"prefab\": \"Prefabs/Vaisseau.prefab.json\"") != std::string::npos);
    CHECK(saved.find("Lampe") == std::string::npos && saved.find("\"reach\"") == std::string::npos);
    CHECK(saved.find("100") != std::string::npos);
    Scene loaded;
    CHECK(SceneSerializer::LoadFromString(loaded, components, saved, reader).has_value());
    CHECK(loaded.GetEntityCount() == 3);
    const Entity loadedLamp = loaded.FindById(lampId);
    CHECK(loadedLamp != NullEntity && loaded.GetParent(loadedLamp) == loaded.FindById(seatId));
    CHECK(loaded.Get<LightComponent>(loadedLamp).intensity == 3.0f);
    CHECK(loaded.Get<TransformComponent>(loaded.FindById(shipId)).position == (Math::Vector3d{100.0, 2.0, -50.0}));
    CHECK(loaded.Get<PrefabInstanceComponent>(loaded.FindById(shipId)).prefab == path);
    CHECK(SceneSerializer::SaveToString(loaded, components, reader) == saved); // fichier stable
    // Sans de quoi lire le prefab : refusé, la scène n'est pas touchée.
    CHECK(!SceneSerializer::LoadFromString(loaded, components, saved).has_value());
    CHECK(loaded.GetEntityCount() == 3);

    // Réglages changés sur l'exemplaire : seuls ces champs sont gardés ; une entité et un composant ajoutés, un
    // composant retiré.
    scene.Get<SeatComponent>(seat).reach = 5.0f;
    scene.Get<NameComponent>(lamp).name = "Plafonnier";
    const Entity added = scene.CreateEntity("Ajout", seat);
    scene.Add<RimLightComponent>(seat);
    scene.Remove<LightComponent>(lamp);
    const std::string edited = SceneSerializer::SaveToString(scene, components, reader);
    CHECK(edited.find("\"reach\": 5") != std::string::npos && edited.find("\"pilot\"") == std::string::npos);
    CHECK(edited.find("Plafonnier") != std::string::npos && edited.find("\"removed\": [\n") != std::string::npos);

    // Le prefab change : ses nouveaux réglages arrivent, ceux changés sur l'exemplaire restent.
    {
        Scene source;
        CHECK(SceneSerializer::LoadFromString(source, components, files[path]).has_value());
        const Entity root = FindByName(source, "Vaisseau");
        source.Get<SeatComponent>(FindByName(source, "Siège")).reach = 9.0f;
        source.Get<SeatComponent>(FindByName(source, "Siège")).pilot = true;
        source.Get<ShipComponent>(root).maxSpeed = 321.0f;
        source.CreateEntity("Moteur", root);
        const auto changed = SceneSerializer::SavePrefab(source, components, root, SceneSerializer::PrefabIds::Keep);
        CHECK(changed.has_value());
        files[path] = *changed;
    }
    Scene reloaded;
    CHECK(SceneSerializer::LoadFromString(reloaded, components, edited, reader).has_value());
    const Entity reloadedShip = reloaded.FindById(shipId);
    const Entity reloadedSeat = reloaded.FindById(seatId);
    CHECK(reloaded.Get<ShipComponent>(reloadedShip).maxSpeed == 321.0f);
    CHECK(reloaded.Get<SeatComponent>(reloadedSeat).reach == 5.0f && reloaded.Get<SeatComponent>(reloadedSeat).pilot);
    CHECK(reloaded.Get<NameComponent>(reloaded.FindById(lampId)).name == "Plafonnier");
    CHECK(!reloaded.TryGet<LightComponent>(reloaded.FindById(lampId)));
    CHECK(reloaded.TryGet<RimLightComponent>(reloadedSeat));
    CHECK(FindChild(reloaded, reloadedShip, "Moteur") != NullEntity);
    const Entity reloadedAdded = reloaded.FindById(scene.GetId(added));
    CHECK(reloadedAdded != NullEntity && reloaded.GetParent(reloadedAdded) == reloadedSeat
          && !reloaded.TryGet<PrefabLinkComponent>(reloadedAdded));

    // Un autre exemplaire : ses propres identifiants, les réglages du prefab.
    const auto second = SceneSerializer::Instantiate(reloaded, components, path, reader);
    CHECK(second.has_value());
    CHECK(reloaded.Get<NameComponent>(*second).name == "Vaisseau" && reloaded.GetId(*second) != shipId);
    const Entity secondSeat = FindChild(reloaded, *second, "Siège");
    CHECK(secondSeat != NullEntity && reloaded.Get<SeatComponent>(secondSeat).reach == 9.0f);
    CHECK(reloaded.Get<TransformComponent>(*second).position == Math::Vector3d{});
    const std::string twoShips = SceneSerializer::SaveToString(reloaded, components, reader);
    Scene twice;
    CHECK(SceneSerializer::LoadFromString(twice, components, twoShips, reader).has_value());
    CHECK(twice.GetEntityCount() == reloaded.GetEntityCount());
    CHECK(SceneSerializer::SaveToString(twice, components, reader) == twoShips);

    // Annuler : l'instantané d'une pièce garde son lien au prefab.
    {
        const std::string snapshot = SceneSerializer::SaveEntity(reloaded, components, reloadedSeat, true);
        reloaded.DestroyEntity(reloadedSeat);
        CHECK(SceneSerializer::RestoreEntity(reloaded, components, snapshot).has_value());
        const Entity restored = reloaded.FindById(seatId);
        CHECK(restored != NullEntity && reloaded.Get<PrefabLinkComponent>(restored).instance == reloadedShip);
        CHECK(reloaded.Get<PrefabLinkComponent>(reloaded.FindById(lampId)).instance == reloadedShip);
        CHECK(SceneSerializer::SaveToString(reloaded, components, reader) == twoShips);
    }

    // Délié : des entités ordinaires, écrites en entier.
    SceneSerializer::UnlinkPrefab(reloaded, *second);
    CHECK(!reloaded.TryGet<PrefabInstanceComponent>(*second) && !reloaded.TryGet<PrefabLinkComponent>(secondSeat));
    const std::string unlinked = SceneSerializer::SaveToString(reloaded, components, reader);
    Scene plain;
    CHECK(SceneSerializer::LoadFromString(plain, components, unlinked, reader).has_value());
    CHECK(plain.Get<SeatComponent>(plain.FindById(reloaded.GetId(secondSeat))).reach == 9.0f);

    // Un prefab ne contient pas d'exemplaire de prefab (pas encore).
    CHECK(!SceneSerializer::SavePrefab(reloaded, components, reloadedShip, SceneSerializer::PrefabIds::ForInstance));
    return true;
}
