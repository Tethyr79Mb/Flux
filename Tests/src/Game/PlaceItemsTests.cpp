// Objets du jeu dans le panneau Placer (Game/Module/src/PlaceItems.cpp) : le jeu les déclare, l'éditeur les construit
// sans connaître leurs composants.
#include "../Check.hpp"

#include "MissionGiver.hpp"
#include "PlaceItems.hpp"

#include "Flux/Core/EditorExtensions.hpp"
#include "Flux/Scene/Scene.hpp"

#include <cstddef>
#include <iterator>
#include <string_view>

using namespace Flux;

namespace {
    template<typename T>
    std::size_t Count(const Scene& scene) {
        const auto view = scene.GetRegistry().view<const T>();
        return static_cast<std::size_t>(std::ranges::distance(view.begin(), view.end()));
    }

    // Construit l'objet comme l'éditeur : une entité à son nom, que build remplit.
    bool Build(const EditorExtensions& extensions, std::string_view label, Scene& scene, Entity& entity) {
        const PlaceItem* item = extensions.FindPlaceItem(label);
        CHECK(item != nullptr && item->build);
        entity = scene.CreateEntity(item->name);
        item->build(scene, entity);
        return true;
    }
}

bool RunPlaceItemsTests() {
    EditorExtensions extensions;
    Game::RegisterPlaceItems(extensions);
    CHECK(extensions.GetPlaceItems().size() == 4);
    CHECK(extensions.FindPlaceItem("Rien de tel") == nullptr);

    {
        Scene scene;
        Entity fighter = NullEntity;
        CHECK(Build(extensions, "Chasseur", scene, fighter));
        CHECK(scene.Get<ShipComponent>(fighter).turnRate == 90.0f);
        CHECK(scene.Get<HealthComponent>(fighter).health == 500.0f);
        CHECK(Count<HardpointComponent>(scene) > 0 && Count<WeaponComponent>(scene) == Count<HardpointComponent>(scene));
        CHECK(Count<SeatComponent>(scene) == 1);
    }
    {
        // Maquette de station : immobile, quatre quais et un donneur de missions, placée à l'horizontale.
        Scene scene;
        Entity station = NullEntity;
        CHECK(Build(extensions, "Station (maquette)", scene, station));
        CHECK(!scene.Get<ShipComponent>(station).fallsWithoutPilot && scene.Get<ShipComponent>(station).warpSpeed == 0.0f);
        CHECK(Count<DockComponent>(scene) == 4);
        CHECK(Count<Game::MissionGiverComponent>(scene) == 1);
        CHECK(extensions.FindPlaceItem("Station (maquette)")->level);
    }
    {
        Scene scene;
        Entity station = NullEntity;
        CHECK(Build(extensions, "Station orbitale", scene, station));
        CHECK(Count<DockComponent>(scene) > 0);
        CHECK(!extensions.FindPlaceItem("Station orbitale")->dropToGround);
        CHECK(extensions.FindPlaceItem("Station orbitale")->category == "Espace");
    }

    extensions.Clear(); // déchargement du jeu
    CHECK(extensions.GetPlaceItems().empty());
    return true;
}
