#pragma once

#include "Flux/Scene/Components.hpp"

#include <vector>

namespace Flux {
    class Scene;

    // Rendu entre deux pas de simulation (SimulationClock) : ce qui a bougé pendant le dernier pas est dessiné entre
    // sa place d'avant et celle d'après, selon le temps écoulé depuis. Sans cela, à 60 pas par seconde, un écran
    // plus rapide montrerait la même place deux images de suite, puis un saut.
    //
    // Les places interpolées sont écrites dans la scène pour l'image (rendu, caméra, son les lisent sans rien
    // changer), puis Restore remet l'état simulé avant le pas suivant. Une place changée entre-temps par un autre
    // (inspecteur de l'éditeur pendant le jeu) est gardée telle quelle : c'est la nouvelle place simulée.
    class TransformInterpolation {
        public:
        // Début d'image, avant les pas : l'état simulé revient dans la scène.
        void Restore(Scene& scene);
        // Juste avant le dernier pas de l'image : retient les places d'avant.
        void BeginStep(const Scene& scene);
        // Juste après : retient ce qui a bougé, et où.
        void EndStep(const Scene& scene);
        // Écrit les places à `alpha` entre les deux (0 : avant le pas, 1 : après).
        void Apply(Scene& scene, double alpha);
        // Scène changée ou jeu arrêté : plus rien à remettre.
        void Clear();

        private:
        struct Snapshot
        {
            Entity entity = NullEntity;
            Entity parent = NullEntity;
            Math::Vector3i64 sector;
            TransformComponent transform;
        };
        struct Moving
        {
            Entity entity = NullEntity;
            TransformComponent before;
            TransformComponent after;
            TransformComponent drawn; // ce qu'Apply a écrit : Restore reconnaît une place que personne n'a touchée
            bool applied = false;
        };

        // Gardés d'une image à l'autre : aucune allocation une fois la scène vue.
        std::vector<Snapshot> m_before;
        std::vector<Moving> m_moving;
    };
}
