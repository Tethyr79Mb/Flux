#pragma once

#include "Flux/Graphics/Planet.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Scene/Components.hpp"
#include "Renderer/ModelCache.hpp"
#include "Renderer/PlanetTileCompute.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Flux {
    // Sol d'une planète tel que le lit fragmentPlanet (PlanetData dans shaders/Mesh.slang).
    struct PlanetGpu
    {
        std::array<float, 4> shape{}; // rayon, relief, altitude de la neige, du sable (m)
        std::array<float, 4> rock{};  // couleur linéaire ; w : rugosité
        std::array<float, 4> grass{};
        std::array<float, 4> sand{};
        std::array<float, 4> snow{};
        std::array<float, 4> water{}; // couleur linéaire ; w : rayon de la surface de l'océan (0 : aucun)
        std::array<float, 4> soil{};
        std::array<float, 4> climate{}; // température à l'équateur, aux pôles (°C), humidité, inutilisé
    };
    static_assert(sizeof(PlanetGpu) == 128);

    // Tuiles des planètes (PlanetComponent) : pour chaque image, parcourt l'arbre de tuiles de chaque face et retient
    // celles à dessiner, plus fines près de la caméra (Flux/Graphics/Planet.hpp). Les tuiles manquantes sont calculées
    // par le GPU (PlanetTileCompute), les plus urgentes d'abord, au début de l'image suivante ; en attendant, leur
    // parente reste affichée (jamais de trou). Le CPU ne fait que choisir les tuiles et préparer quelques valeurs
    // par tuile.
    // Chaque tuile est un GpuModel (ses sommets ; les indices communs à toutes), dessiné par MeshRenderer.
    class PlanetRenderer {
        public:
        static std::expected<std::unique_ptr<PlanetRenderer>, std::string> Create(RHI::Device& device);

        // Une tuile à dessiner : son centre relatif à la caméra (calculé en double) et sa sphère englobante.
        struct Tile
        {
            const GpuModel* model;
            Math::Vector3d center;
            float radius;
            // Glissement vers la forme parente (voir vertexPlanet dans shaders/Mesh.slang) : distance où il commence,
            // et 1 / sa longueur (0 : aucun).
            float morphStart;
            float morphScale;
            double lowest; // plus petite distance de ses sommets au centre de la planète (océan au-dessus ?)
        };
        // Champ de la caméra : la sphère (relative à la caméra) le touche.
        using ViewTest = std::function<bool(const Math::Vector3f& center, float radius)>;

        // Début d'une image : avant les appels à Collect. Le GPU a fini l'image précédente de même indice.
        void BeginFrame(std::uint32_t frameIndex);
        // Ajoute à `tiles` les tuiles de la planète à dessiner. `position` : son centre relatif à la caméra.
        void Collect(Entity entity, const PlanetComponent& planet, const Math::Vector3d& position,
                     const Math::Quaternionf& rotation, const ViewTest& inView, std::vector<Tile>& tiles);
        // Fin de la préparation : choisit les tuiles demandées à calculer, oublie les planètes disparues et les
        // tuiles inutilisées en trop.
        void EndFrame(std::uint32_t frameIndex);
        // Calcule les tuiles choisies par EndFrame, hors d'une passe et avant les dessins.
        void Compute(RHI::CommandList& commands, std::uint32_t frameIndex) { m_compute->Record(commands, frameIndex); }

        private:
        PlanetRenderer(RHI::Device& device, std::unique_ptr<PlanetTileCompute> compute)
            : m_device(device), m_compute(std::move(compute)) {}

        // Un nœud de l'arbre déjà visité : sa sphère englobante, et sa tuile si elle est calculée (ou le sera au début
        // de l'image suivante, avant tout dessin).
        struct Node
        {
            Planet::TileBounds bounds;
            GpuModel model; // vertices nul : pas encore calculée
            std::unique_ptr<RHI::BindGroup> group; // ses buffers, écrits par le GPU
            Math::Vector3d center; // de ses sommets, repère de la planète
            float radius = 0.0f;   // celui de `bounds` tant que le GPU n'a pas rendu le sien
            double lowest = 0.0;   // au plus bas possible tant que le GPU n'a pas rendu le sien
            double highest = 0.0;  // plus grande distance d'un sommet au centre de la planète, une fois mesurée
            std::uint64_t lastUsed = 0;
            bool measured = false; // lowest, highest et radius viennent du GPU
            // bounds tirée des hauteurs mesurées de la parente ; sinon de tout le relief de la planète (une petite
            // tuile se croirait alors haute de tout le relief : proche de la caméra, découpée pour rien).
            bool tight = false;
        };
        struct Request
        {
            Planet::TileKey key;
            double distance;
        };
        struct PlanetState
        {
            std::shared_ptr<const Planet::Terrain> terrain;
            std::uint64_t owner = 0; // identifie cette planète et cette forme auprès du générateur
            float radius = 0.0f;
            float relief = -1.0f;
            std::int32_t seed = 0;
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
        };

        Node& GetNode(PlanetState& state, Planet::TileKey key, const Node* parent = nullptr);
        bool IsCulled(const Visit& visit, const Node& node) const;
        void VisitNode(Visit& visit, Planet::TileKey key, Node& node);
        PlanetState* FindOwner(std::uint64_t owner);
        bool Schedule(PlanetState& state, Planet::TileKey key, Node& node, std::uint32_t frameIndex);
        void Retire(Node& node);

        RHI::Device& m_device;
        std::unique_ptr<PlanetTileCompute> m_compute; // avant les nœuds : leurs groupes sont rendus à son layout
        std::shared_ptr<RHI::Buffer> m_indices; // communs à toutes les tuiles
        std::uint32_t m_indexCount = 0;
        std::unordered_map<Entity, PlanetState> m_planets;
        std::uint64_t m_frame = 0;
        bool m_poolFullLogged = false; // « trop de tuiles » signalé une seule fois
        // Buffers de sommets libérés, réutilisables quand le GPU ne les lit plus (image `frame` + FramesInFlight).
        struct FreeBuffer
        {
            PlanetTileCompute::TileBuffers buffers;
            std::uint64_t frame;
        };
        std::vector<FreeBuffer> m_freeBuffers;
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
