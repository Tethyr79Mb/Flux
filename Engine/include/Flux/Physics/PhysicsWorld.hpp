#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <memory>

namespace Flux {
    class Scene;

    // Commandes du joueur pour les personnages qu'il dirige (CharacterComponent::player).
    struct CharacterInput
    {
        Math::Vector3f move; // direction voulue dans le plan du sol (repère du monde), longueur 0 à 1
        bool run = false;
        bool jump = false;
    };

    // Monde physique (Jolt) d'une scène : les entités avec ColliderComponent, RigidBodyComponent et
    // CharacterComponent y ont un corps, créé à leur première mise à jour. Aucun type Jolt ici.
    //
    // Échelle galaxie : la simulation se fait en float autour d'une ancre (le secteur du premier objet physique
    // rencontré), précise au millimètre à des kilomètres de là. Plus tard, un vaisseau ou une station aura son
    // propre monde, ancré sur lui : on y marche pendant qu'il vole.
    class FLUX_API PhysicsWorld {
        public:
        PhysicsWorld();
        ~PhysicsWorld();

        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        // Crée les corps des nouvelles entités et retire ceux des entités détruites (ou sans collision),
        // avance la simulation par pas fixes de 1/60 s, puis reporte positions et rotations dans la scène
        // (corps dynamiques et personnages). Les réglages d'une forme ou d'un corps modifiés après sa création
        // sont ignorés jusqu'au prochain monde.
        void Update(Scene& scene, double deltaSeconds, const CharacterInput& input);

        // « Bas » de ce monde : (0, -9.81, 0) par défaut. Gravité artificielle d'une station : le sol du pont.
        void SetGravity(const Math::Vector3f& gravity);
        Math::Vector3f GetGravity() const;

        // Personnage dirigé par le joueur (le premier trouvé), NullEntity s'il n'y en a pas : la caméra le suit.
        Entity GetPlayer() const;
        // Nombre de corps rigides (hors personnages), pour les tests et le panneau Moteur.
        int GetBodyCount() const;

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
