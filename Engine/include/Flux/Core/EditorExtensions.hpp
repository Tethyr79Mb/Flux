#pragma once

#include "Flux/Export.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/UI/Icons.hpp"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    class Scene;

    // Objet que le jeu ajoute au panneau Placer de l'éditeur (chasseur armé, station à quais...). L'éditeur ne connaît
    // pas les composants du jeu : il crée l'entité, laisse build la remplir, puis la place devant la caméra.
    struct PlaceItem
    {
        std::string category; // catégorie du panneau : une de l'éditeur (« Espace »...) ou une nouvelle, à la suite
        std::string label;    // ligne du panneau et nom du pas d'annulation
        std::string name;     // nom de l'entité créée
        UI::Icon icon = UI::Icon::Cube;
        double distance = 5.0; // m devant la caméra : de quoi le voir en entier
        // Posé à l'horizontale, tourné vers la caméra et below mètres sous elle (une station : regardée d'en haut,
        // elle s'enfoncerait dans le sol de la scène). Seulement sur un clic : lâché dans la vue, il garde sa rotation.
        bool level = false;
        double below = 0.0;
        bool dropToGround = true; // le bouton « Poser au sol » de la vue s'applique (pas à une tour de 4 km)
        std::function<void(Scene& scene, Entity entity)> build;
    };

    // Ce que le jeu ajoute à l'éditeur. Rempli par le jeu (FluxRegisterGame), vidé par GameModule::Unregister avant le
    // déchargement de la DLL : les fonctions build sont dans son code. Le client du jeu le remplit aussi, sans s'en servir.
    class FLUX_API EditorExtensions {
        public:
        void AddPlaceItem(PlaceItem item);
        std::span<const PlaceItem> GetPlaceItems() const { return m_placeItems; }
        const PlaceItem* FindPlaceItem(std::string_view label) const;
        void Clear();

        private:
        std::vector<PlaceItem> m_placeItems;
    };
}
