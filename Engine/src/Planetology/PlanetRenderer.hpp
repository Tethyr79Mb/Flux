#pragma once

#include "Flux/Planetology/Planet.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Scene/Components.hpp"
#include "Planetology/PlanetTileCompute.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Flux::Planetology {
    // Sol d'une planète tel que le lit fragmentPlanet (PlanetData dans shaders/Mesh.slang).
    struct PlanetGpu
    {
        std::array<float, 4> shape{}; // rayon, relief, altitude de la neige (m) ; w : inutilisé
        std::array<float, 4> rock{};  // couleur linéaire (avec les textures du sol : teinte) ; w : rugosité
        std::array<float, 4> grass{};
        std::array<float, 4> sand{};
        std::array<float, 4> snow{};
        std::array<float, 4> water{}; // couleur linéaire ; w : rayon de la surface de l'océan (0 : aucun)
        std::array<float, 4> soil{};
        std::array<float, 4> climate{}; // température à l'équateur, aux pôles (°C), humidité, 1 si textures du sol (GroundTextures)
        std::array<float, 4> beach{};   // altitude du niveau de la mer, hauteur de la plage au-dessus (m)
        std::array<float, 4> rim{};     // couleur x intensité de la lumière de contour ; w : finesse (0 : aucune)
        // Nuages (CloudsComponent) : centre de la planète relatif à la caméra et rayon de leur couche (w, 0 : aucun) ;
        // rotation de la planète (quaternion) ; couverture, opacité, 1 / taille des formations, angle de dérive ; couleur et
        // épaisseur de la couche (w, m).
        std::array<float, 4> clouds{};
        std::array<float, 4> cloudRotation{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> cloudShape{};
        std::array<float, 4> cloudColor{};
        // Forêts (VegetationComponent) : part couverte, densité (0 : aucune), cosinus de la pente maximale, distance
        // où leur couleur commence sur le sol ; couleur linéaire de la cime.
        std::array<float, 4> forest{};
        std::array<float, 4> forestColor{};
    };
    static_assert(sizeof(PlanetGpu) == 256);

    // Tuiles des planètes (PlanetComponent) : pour chaque image, parcourt l'arbre de tuiles de chaque face et retient
    // celles à dessiner, plus fines près de la caméra (Flux/Planetology/Planet.hpp). Les tuiles manquantes sont calculées
    // par le GPU (PlanetTileCompute), les plus urgentes d'abord, au début de l'image suivante ; en attendant, leur
    // parente reste affichée (jamais de trou). Le CPU ne fait que choisir les tuiles et préparer quelques valeurs
    // par tuile.
    // Chaque tuile occupe une case des pages de PlanetTileCompute (les indices sont communs à toutes) ; MeshRenderer
    // dessine chaque page en un appel indirect.
    class PlanetRenderer {
        public:
        static std::expected<std::unique_ptr<PlanetRenderer>, std::string> Create(RHI::Device& device);

        // Une tuile à dessiner : le point de référence de ses sommets relatif à la caméra (calculé en double) et sa
        // sphère englobante.
        struct Tile
        {
            std::uint32_t tile; // case dans les pages de sommets (PlanetTileCompute)
            Math::Vector3d center;
            Math::Vector3d local; // son centre dans le repère de la planète
            Math::Vector3d sphere; // centre de la sphère englobante, relatif à la caméra
            float radius;
            // Glissement vers la forme parente (voir vertexPlanet dans shaders/Mesh.slang) : distance où il commence,
            // et 1 / sa longueur (0 : aucun).
            float morphStart;
            float morphScale;
            double lowest; // plus petite distance de ses sommets au centre de la planète (océan au-dessus ?)
        };
        // Une tuile du niveau des plaines semées d'arbres (VegetationRenderer), en vue, qu'elle soit dessinée ou coupée
        // en plus fines.
        struct Plain
        {
            TileKey key;
            Math::Vector3d origin; // point de référence de ses sommets (TileBounds::origin), relatif à la caméra
            double distance;       // de la caméra à sa sphère englobante
        };
        // Champ de la caméra : la sphère (relative à la caméra) le touche.
        using ViewTest = std::function<bool(const Math::Vector3f& center, float radius)>;

        // Début d'une image : avant les appels à Collect. Le GPU a fini l'image précédente de même indice.
        void BeginFrame(std::uint32_t frameIndex);
        // Ajoute à `tiles` les tuiles de la planète à dessiner. `position` : son centre relatif à la caméra. Avec
        // `plains`, y ajoute aussi les tuiles du niveau `plainLevel` en vue à moins de `plainRange` mètres : celles
        // visitées, et sous une tuile plus grossière dessinée, celles qu'elle couvre.
        void Collect(Entity entity, const PlanetComponent& planet, const Math::Vector3d& position,
                     const Math::Quaternionf& rotation, const ViewTest& inView, std::vector<Tile>& tiles,
                     std::vector<Plain>* plains = nullptr, std::uint32_t plainLevel = 0, double plainRange = 0.0);
        // Forme de la planète (après un Collect) ; nullptr si elle n'est pas dessinée.
        const Terrain* FindTerrain(Entity entity) const {
            const auto state = m_planets.find(entity);
            return state != m_planets.end() ? state->second.terrain.get() : nullptr;
        }
        // Fin de la préparation : choisit les tuiles demandées à calculer, oublie les planètes disparues et les
        // tuiles inutilisées en trop.
        void EndFrame(std::uint32_t frameIndex);
        // Calcule les tuiles choisies par EndFrame, hors d'une passe et avant les dessins.
        void Compute(RHI::CommandList& commands, std::uint32_t frameIndex) { m_compute->Record(commands, frameIndex); }

        // Pages de sommets des tuiles, et leurs indices (communs à toutes) ; nullptr avant la première image.
        const PlanetTileCompute& GetTiles() const { return *m_compute; }
        const RHI::Buffer* GetIndices() const { return m_indices.get(); }
        // Cases occupées par une tuile (gardée pour plus tard ou dessinée), pour le panneau Moteur.
        std::uint32_t GetStoredTileCount() const {
            return m_compute->GetTileCount() - static_cast<std::uint32_t>(m_freeTiles.size());
        }
        std::uint32_t GetIndexCount() const { return m_indexCount; }
        // Tuiles demandées à la dernière image et pas encore prêtes (0 : le sol est au détail voulu partout où on le
        // voit). Sert à l'écran de chargement.
        std::uint32_t GetPendingTileCount() const { return m_pendingTiles; }

        private:
        PlanetRenderer(RHI::Device& device, std::unique_ptr<PlanetTileCompute> compute)
            : m_device(device), m_compute(std::move(compute)) {}

        static constexpr std::uint32_t NoTile = ~0u;
        // Un nœud de l'arbre déjà visité : sa sphère englobante, et sa tuile si elle est calculée (ou le sera au début
        // de l'image suivante, avant tout dessin).
        struct Node
        {
            TileBounds bounds;
            std::uint32_t tile = NoTile; // sa case dans les pages ; NoTile : pas encore calculée
            double lowest = 0.0;   // au plus bas possible tant que le GPU n'a pas rendu le sien
            double highest = 0.0;  // plus grande distance d'un sommet au centre de la planète, une fois mesurée
            std::uint64_t lastUsed = 0;
            bool measured = false; // lowest, highest et bounds viennent des hauteurs mesurées par le GPU
            // bounds tirée de ses hauteurs mesurées ou de celles de sa parente ; sinon de tout le relief de la planète
            // (une petite tuile se croirait alors haute de tout le relief : proche de la caméra, découpée pour rien).
            bool tight = false;
        };
        struct Request
        {
            TileKey key;
            double distance;
        };
        struct PlanetState
        {
            std::shared_ptr<const Terrain> terrain;
            std::uint64_t owner = 0; // identifie cette planète et cette forme auprès du générateur
            float radius = 0.0f;
            float relief = -1.0f;
            std::int32_t seed = 0;
            float mountainWidth = 0.0f;
            std::unordered_map<std::uint64_t, Node> nodes;
            std::vector<Request> requests;
            std::uint64_t lastUsed = 0;
        };
        // Ce qu'un parcours de l'arbre garde d'une planète (évite de passer dix paramètres).
        struct Visit
        {
            PlanetState& state;
            Math::Vector3d camera; // repère de la planète
            Math::Vector3d position;
            Math::Quaternionf rotation;
            double horizon; // angle (radians) au-delà duquel une tuile est cachée par la planète ; < 0 : aucun
            double minRadius;
            const ViewTest& inView;
            std::vector<Tile>& tiles;
            std::vector<Plain>* plains;
            std::uint32_t plainLevel;
            double plainRange;
        };

        Node& GetNode(PlanetState& state, TileKey key, const Node* parent = nullptr);
        bool IsCulled(const Visit& visit, const TileBounds& bounds) const;
        bool IsCulled(const Visit& visit, const Node& node) const { return IsCulled(visit, node.bounds); }
        void VisitNode(Visit& visit, TileKey key, Node& node);
        void AddPlains(Visit& visit, TileKey key);
        PlanetState* FindOwner(std::uint64_t owner);
        bool Schedule(PlanetState& state, TileKey key, Node& node, std::uint32_t frameIndex);
        void Retire(Node& node);

        RHI::Device& m_device;
        std::unique_ptr<PlanetTileCompute> m_compute; // avant les nœuds : leurs groupes sont rendus à son layout
        std::unique_ptr<RHI::Buffer> m_indices; // communs à toutes les tuiles
        std::uint32_t m_indexCount = 0;
        std::unordered_map<Entity, PlanetState> m_planets;
        std::uint64_t m_frame = 0;
        bool m_poolFullLogged = false; // « trop de tuiles » signalé une seule fois
        std::uint32_t m_pendingTiles = 0;
        // Cases libérées, réutilisables quand le GPU ne les lit plus (image `frame` + FramesInFlight).
        struct FreeTile
        {
            std::uint32_t tile;
            std::uint64_t frame;
        };
        std::vector<FreeTile> m_freeTiles;
        // Tuiles calculées avec chaque image en vol, dans l'ordre : leurs mesures reviennent quand le GPU a fini.
        struct Computed
        {
            std::uint64_t owner;
            std::uint64_t key;
        };
        std::array<std::vector<Computed>, RHI::FramesInFlight> m_computed;
        std::uint64_t m_nextOwner = 1;
    };
}
