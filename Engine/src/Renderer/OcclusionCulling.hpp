#pragma once

#include "Flux/Math/Matrix4.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace Flux {
    // Entités cachées derrière d'autres, écartées par le GPU (shaders/Culling.slang) : la profondeur de chaque image
    // est réduite en pyramide (BuildPyramid) ; à l'image suivante, chaque entité des lots de la passe principale (Item)
    // y est comparée avant d'être dessinée (Record), et seules les visibles sont écrites dans les commandes de dessin.
    // Le test se fait là où l'entité était à l'image précédente (DrawData::previousWorld), vue de l'ancienne caméra :
    // exact pour ce qui bouge avec la caméra (l'intérieur d'un vaisseau) comme pour le décor.
    // Limite : ce qu'une porte ou un mur découvre d'une image à l'autre apparaît une image plus tard (test sur la
    // profondeur de l'image précédente, sans seconde passe).
    class OcclusionCulling {
        public:
        // Doit correspondre à CullItem dans shaders/Culling.slang : une entité d'une commande (ou un bloc d'un grand
        // modèle).
        struct Item
        {
            std::array<float, 4> sphere; // xyz : centre dans le repère du modèle ; w : rayon dans le monde
            std::uint32_t command;       // dans le buffer des commandes de l'image ; instanceCount envoyé à 0
            std::uint32_t draw;          // DrawData de l'entité
            std::uint32_t material;      // GpuMaterial::index
            std::uint32_t padding = 0;
        };

        struct Stats
        {
            std::uint32_t tested = 0;
            std::uint32_t hidden = 0;
        };

        static std::expected<std::unique_ptr<OcclusionCulling>, std::string> Create(RHI::Device& device);
        ~OcclusionCulling();
        OcclusionCulling(const OcclusionCulling&) = delete;
        OcclusionCulling& operator=(const OcclusionCulling&) = delete;

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // Début de la préparation de l'image `frameIndex` (après Device::BeginFrame), une fois par image. true si les
        // entités peuvent être testées : activé, pyramide faite par l'image précédente, caméra sans saut
        // (`continuous`) ; `previousViewProjection` est la vue de cette image précédente, sans décalage.
        bool Begin(std::uint32_t frameIndex, bool continuous, const Math::Matrix4f& previousViewProjection);
        // À remplir après un Begin qui a rendu true, avant Upload.
        std::vector<Item>& GetItems() { return m_items; }
        // Envoie les entités à tester et relie les buffers de l'image (draws : DrawData ; commands : commandes de
        // dessin, Storage et Indirect ; instances : celles lues par Mesh.slang). false si un buffer ne peut grandir :
        // rien n'est testé, l'appelant rend leurs entités aux commandes.
        bool Upload(std::uint32_t frameIndex, const RHI::Buffer& draws, const RHI::Buffer& commands, const RHI::Buffer& instances);
        // Le test, avant la passe principale (hors d'une passe), après un Upload réussi.
        void Record(RHI::CommandList& commands, std::uint32_t frameIndex);
        // Pyramide de la profondeur de la scène (rendue lisible par la fin de sa passe), de taille `size`, pour
        // l'image suivante ; hors d'une passe.
        void BuildPyramid(RHI::CommandList& commands, std::uint32_t frameIndex, const RHI::Texture& depth, RHI::Extent size);
        // Entités testées et cachées à la dernière image dont le GPU a fini (deux images plus tôt).
        Stats GetStats() const { return m_stats; }

        // Pyramide faite par l'image précédente, lue par l'image en préparation (après un Begin qui a rendu true) :
        // aussi par les mesh shaders du sol (shaders/PlanetMesh.slang).
        struct SourcePyramid
        {
            const RHI::Buffer* buffer = nullptr;
            RHI::Extent size;                     // de la profondeur source
            std::uint32_t levels = 0;
            std::array<std::uint32_t, 16> offsets{}; // premier texel de chaque niveau
        };
        SourcePyramid GetSourcePyramid() const {
            const Pyramid& source = m_pyramids[m_source];
            return {source.buffer.get(), source.size, source.levels, source.offsets};
        }

        private:
        // Une pyramide par image en vol : celle d'une image est lue par la suivante pendant que la sienne se fait.
        struct Pyramid
        {
            std::unique_ptr<RHI::Buffer> buffer;
            std::unique_ptr<RHI::Buffer> retired; // remplacée, lue peut-être encore par l'image en vol
            RHI::Extent size;                     // de la profondeur source
            std::uint32_t levels = 0;
            std::array<std::uint32_t, 16> offsets{};
            std::uint32_t frame = 0;              // numéro (m_frame) de l'image qui l'a faite ; 0 : aucune
        };
        struct Frame
        {
            std::unique_ptr<RHI::Buffer> frame;  // CullFrame
            std::unique_ptr<RHI::Buffer> items;
            std::unique_ptr<RHI::Buffer> stats;  // Readback
            // Même layout : le test (emplacements 0 à 6) et la pyramide (7 et 8), chacun relié quand il sert.
            std::unique_ptr<RHI::BindGroup> group;
            std::unique_ptr<RHI::BindGroup> pyramidGroup;
            std::uint32_t tested = 0; // entités envoyées au test ; ses résultats relus au retour de l'image
            bool testing = false;     // Upload réussi : Record les teste
        };

        explicit OcclusionCulling(RHI::Device& device) : m_device(device) {}

        RHI::Device& m_device;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Pipeline> m_clear;
        std::unique_ptr<RHI::Pipeline> m_cull;
        std::unique_ptr<RHI::Pipeline> m_pyramidPipeline;
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::array<Frame, RHI::FramesInFlight> m_frames;
        std::array<Pyramid, RHI::FramesInFlight> m_pyramids;
        std::vector<Item> m_items;
        Math::Matrix4f m_previousViewProjection;
        std::uint32_t m_frame = 0;
        std::uint32_t m_lastPyramid = 0; // indice de la dernière pyramide faite
        std::uint32_t m_source = 0;      // pyramide lue par l'image en préparation
        bool m_enabled = true;
        Stats m_stats;
    };
}
