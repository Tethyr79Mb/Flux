#pragma once

#include "Flux/Math/Matrix4.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Flux {
    struct GpuModel;

    // Scène vue par le ray tracing (RHI::Device::HasRayTracing) : la structure haute de l'image, une instance par
    // entité dont le modèle a sa structure basse, placée relativement à la caméra comme pour le dessin, et reconstruite
    // à chaque image (les objets bougent, la caméra aussi). Les structures basses des modèles (GpuModel::bottomLevel)
    // sont construites ici à leur première apparition, quelques-unes par image.
    // Une structure haute, ses instances et sa mémoire de travail par image en vol : celle d'une image se reconstruit
    // pendant que le GPU lit encore l'autre.
    class RayTracingScene {
        public:
        explicit RayTracingScene(RHI::Device& device) : m_device(device) {}

        // Début de la préparation d'une image : aucune instance.
        void Begin();
        // Une entité : `world` sa matrice relative à la caméra, `index` ses données dans l'image (DrawData), rendu aux
        // shaders. Sans effet si son modèle n'a pas de structure basse (animé, verre seul) ; si elle n'est pas encore
        // construite, demandée pour cette image dans la limite du budget.
        void Add(const GpuModel& model, const Math::Matrix4f& world, std::uint32_t index);
        // Fin de la préparation : prépare la structure haute et les mémoires de travail de l'image (agrandies au
        // besoin). false si la mémoire manque : rien n'est construit à cette image.
        bool End(std::uint32_t frameIndex);
        // Enregistre les constructions préparées par End (hors d'une passe) : structures basses, puis la haute.
        void BuildBottomLevels(RHI::CommandList& commands, std::uint32_t frameIndex);
        void BuildTopLevel(RHI::CommandList& commands, std::uint32_t frameIndex);
        // Structure haute de l'image, lisible par les shaders après BuildTopLevel ; nullptr avant la première.
        const RHI::AccelerationStructure* GetTopLevel(std::uint32_t frameIndex) const { return m_frames[frameIndex].topLevel.get(); }

        // Compteurs de la dernière image préparée, pour le panneau Moteur.
        struct Stats
        {
            std::uint32_t instances = 0;   // dans la structure haute
            std::uint32_t models = 0;      // structures basses différentes qu'elle désigne
            std::uint32_t built = 0;       // structures basses construites à cette image
            std::uint32_t waiting = 0;     // entités dont la structure basse attend une image suivante (budget)
            std::uint64_t modelBytes = 0;  // mémoire vidéo de ces structures basses
            std::uint64_t sceneBytes = 0;  // structures hautes et mémoires de travail, toutes images comprises
        };
        const Stats& GetStats() const { return m_stats; }

        private:
        // Grandit `buffer` jusqu'à `size` octets au moins (l'ancien détruit quand le GPU a fini).
        bool Reserve(std::unique_ptr<RHI::Buffer>& buffer, std::uint64_t size);

        struct Frame
        {
            std::unique_ptr<RHI::AccelerationStructure> topLevel;
            std::unique_ptr<RHI::Buffer> topScratch;
            std::unique_ptr<RHI::Buffer> bottomScratch;
        };

        RHI::Device& m_device;
        std::array<Frame, RHI::FramesInFlight> m_frames;
        // De l'image en préparation, gardés d'une image à l'autre pour ne pas allouer.
        std::vector<RHI::AccelerationInstance> m_instances;
        std::vector<RHI::AccelerationStructure*> m_toBuild;
        std::vector<const GpuModel*> m_toBuildModels; // leurs modèles (bottomLevelBuilt rendu faux si End échoue)
        std::uint64_t m_toBuildScratch = 0;
        std::vector<const RHI::AccelerationStructure*> m_used; // structures basses désignées (comptées une fois)
        bool m_ready = false; // End a réussi : les constructions de l'image peuvent s'enregistrer
        Stats m_stats;
    };
}
