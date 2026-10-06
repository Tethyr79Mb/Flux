#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <filesystem>
#include <memory>

namespace Flux {
    class PhysicsContext;
    class Scene;

    // Commandes du joueur pour les personnages qu'il dirige (CharacterComponent::player).
    struct CharacterInput
    {
        Math::Vector3f move; // direction voulue dans le plan du sol (repère du monde), longueur 0 à 1
        bool run = false;
        bool jump = false;
    };

    // Monde physique (Jolt) d'une scène : les entités avec ColliderComponent et CharacterComponent y ont
    // un corps, créé par Prepare (au chargement de la scène) ou à leur première mise à jour. Aucun type Jolt ici.
    //
    // Échelle galaxie : la simulation se fait en float autour d'une ancre (le secteur du premier objet physique
    // rencontré), précise au millimètre à des kilomètres de là. Chaque vaisseau ou station (ShipComponent) a son
    // propre monde, dans son repère, pour ses descendants : on y marche pendant qu'il vole ou tourne. Une entité
    // change de monde quand elle change de parent (sortir d'un vaisseau : la détacher de lui). Un personnage le fait
    // de lui-même : il passe à bord en montant sur le décor d'un vaisseau (rampe, pont), et en sort quand il quitte
    // ce décor au-dessus du sol du dehors ; il devient alors enfant du vaisseau, ou de ce qui l'entoure.
    //
    // Planètes (PlanetComponent) : leur sol est fait autour de ce qui peut le toucher, et elles attirent vers leur
    // centre (les personnages se tiennent debout selon elles). Loin de l'ancre, le monde est recentré sur le joueur.
    class FLUX_API PhysicsWorld {
        public:
        // context : celui du moteur, partagé par tous ses mondes (voir PhysicsContext) ; vide : le monde a le sien.
        explicit PhysicsWorld(std::shared_ptr<PhysicsContext> context = nullptr);
        ~PhysicsWorld();

        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        // Crée les corps de toutes les entités de la scène et l'enveloppe des vaisseaux, sans avancer la simulation :
        // à appeler au chargement de la scène, pour que la première image de jeu n'ait rien à construire.
        void Prepare(Scene& scene);

        // Crée les corps des nouvelles entités et retire ceux des entités détruites (ou sans collision),
        // avance la simulation par pas fixes de 1/60 s, puis reporte positions et rotations dans la scène
        // (corps dynamiques et personnages). Les réglages d'une forme ou d'un corps modifiés après sa création
        // sont ignorés jusqu'au prochain monde.
        void Update(Scene& scene, double deltaSeconds, const CharacterInput& input);

        // Dossier des assets de son contexte (voir PhysicsContext::SetAssetsDirectory).
        void SetAssetsDirectory(const std::filesystem::path& directory);

        // « Bas » de ce monde : (0, -9.81, 0) par défaut. Gravité artificielle d'une station : le sol du pont.
        void SetGravity(const Math::Vector3f& gravity);
        Math::Vector3f GetGravity() const;

        // « Haut » là où se trouve une entité (unitaire, repère du monde) : contre la gravité de son vaisseau, de la
        // planète qui l'attire, ou du monde. La caméra qui suit le joueur garde l'horizon droit selon lui.
        Math::Vector3f GetUp(const Scene& scene, Entity entity) const;
        // Gravité du dehors là où se trouve une entité (m/s², repère du monde) : celle des planètes ou du monde,
        // sans celle des vaisseaux. Nulle dans l'espace (aide au pilotage, ShipComponent::levelAssist).
        Math::Vector3f GetOutsideGravity(const Scene& scene, Entity entity) const;
        // Personnage dirigé par le joueur (le premier trouvé), NullEntity s'il n'y en a pas : la caméra le suit.
        Entity GetPlayer() const;
        // Distance libre (0 à `distance`) pour une sphère de rayon `radius` partant de `from` dans `direction`
        // (unitaire) : où poser la caméra qui suit `player` sans qu'elle entre dans un mur. Seuls les décors
        // immobiles et les coques l'arrêtent : ni les objets simulés, ni les personnages, ni la coque du vaisseau
        // où se trouve le joueur. ignoredShip : vaisseau entier ignoré (coque, décor de bord), pour la caméra qui le
        // suit de l'extérieur quand le joueur le pilote.
        float CastCamera(const Scene& scene, Entity player, const Math::WorldPosition& from, const Math::Vector3f& direction,
                         float distance, float radius, Entity ignoredShip = NullEntity) const;
        // Nombre de corps (hors personnages), pour les tests et le panneau Moteur.
        int GetBodyCount() const;

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
