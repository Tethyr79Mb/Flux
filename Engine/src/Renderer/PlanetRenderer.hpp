#pragma once

#include "Flux/Graphics/Planet.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Scene/Components.hpp"
#include "Renderer/ModelCache.hpp"
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
    };
    static_assert(sizeof(PlanetGpu) == 80);

    // Tuiles des planètes (PlanetComponent) : pour chaque image, parcourt l'arbre de tuiles de chaque face et retient
    // celles à dessiner, plus fines près de la caméra (Flux/Graphics/Planet.hpp). Les tuiles manquantes sont générées
    // en tâches de fond (Planet::TileGenerator) et envoyées au GPU dès qu'elles sont prêtes ; en attendant, leur
    // parente reste affichée (jamais de trou).
    // Chaque tuile est un GpuModel (ses sommets ; les indices communs à toutes), dessiné par MeshRenderer.
    class PlanetRenderer {
        public:
        explicit PlanetRenderer(RHI::Device& device)
            : m_device(device), m_generator(std::make_unique<Planet::TileGenerator>()) {}

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
        };
        // Champ de la caméra : la sphère (relative à la caméra) le touche.
        using ViewTest = std::function<bool(const Math::Vector3f& center, float radius)>;

        // Début d'une image : avant les appels à Collect.
        void BeginFrame();
        // Ajoute à `tiles` les tuiles de la planète à dessiner. `position` : son centre relatif à la caméra.
        void Collect(Entity entity, const PlanetComponent& planet, const Math::Vector3d& position,
                     const Math::Quaternionf& rotation, const ViewTest& inView, std::vector<Tile>& tiles);
        // Fin de la préparation : génère des tuiles demandées dans le budget de temps, oublie les planètes disparues
        // et les tuiles inutilisées en trop.
        void EndFrame();

        private:
        // Un nœud de l'arbre déjà visité : sa sphère englobante, et sa tuile si elle est générée.
        struct Node
        {
            Planet::TileBounds bounds;
            GpuModel model; // vertices nul : pas encore générée
            Math::Vector3d center; // de ses sommets, repère de la planète
            float radius = 0.0f;
            std::uint64_t lastUsed = 0;
            bool queued = false; // demandée au générateur (en attente ou en cours)
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

        Node& GetNode(PlanetState& state, Planet::TileKey key);
        bool IsCulled(const Visit& visit, const Node& node) const;
        void VisitNode(Visit& visit, Planet::TileKey key, Node& node);
        PlanetState* FindOwner(std::uint64_t owner);
        bool Upload(Node& node, const Planet::TileMesh& mesh);
        void Retire(Node& node);

        RHI::Device& m_device;
        std::shared_ptr<RHI::Buffer> m_indices; // communs à toutes les tuiles
        std::uint32_t m_indexCount = 0;
        std::unordered_map<Entity, PlanetState> m_planets;
        std::uint64_t m_frame = 0;
        // Buffers de sommets libérés, réutilisables quand le GPU ne les lit plus (image `frame` + FramesInFlight).
        struct FreeBuffer
        {
            std::unique_ptr<RHI::Buffer> vertices;
            std::unique_ptr<RHI::Buffer> morph;
            std::uint64_t frame;
        };
        std::vector<FreeBuffer> m_freeBuffers;
        Planet::TileMesh m_mesh; // racines, générées sur le fil principal
        std::unique_ptr<Planet::TileGenerator> m_generator;
        std::uint64_t m_nextOwner = 1;
        // Réutilisés d'une image à l'autre.
        std::vector<Planet::TileGenerator::Job> m_jobs;
        std::vector<Planet::TileGenerator::Result> m_results;
    };
}
