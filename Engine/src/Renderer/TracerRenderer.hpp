#pragma once

#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Weapons.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    struct Camera;

    // Traçantes des projectiles (Weapons) : un trait lumineux par projectile (shaders/Tracer.slang), toutes en un
    // seul appel de dessin dans la passe de la scène, après les flammes. Au plus MaxTracers par image : au-delà,
    // ceux du début de la liste ne sont pas dessinés.
    class TracerRenderer {
        public:
        static constexpr std::uint32_t MaxTracers = 4096;

        // `motionFormat` : seconde cible de la passe de la scène (PostRenderer::MotionFormat), laissée telle quelle ;
        // `reactiveFormat` : celui du masque réactif de FSR (PostRenderer::ReactiveFormat).
        static std::expected<std::unique_ptr<TracerRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                   RHI::Format motionFormat,
                                                                                   RHI::Format depthFormat,
                                                                                   RHI::Format reactiveFormat);

        // Traçantes de l'image, relatives à la caméra, envoyées dans le buffer de l'image en vol. `jitter` :
        // TemporalView::jitter ; `height` : hauteur de la vue en pixels (largeur minimale d'une traçante).
        // `impacts` : un éclat rond par impact, qui pâlit en s'éteignant.
        void Prepare(std::span<const Projectile> projectiles, std::span<const Impact> impacts, const Camera& camera, float aspect, float height,
                     Math::Vector2f jitter, std::uint32_t frameIndex);
        // Dans la passe de la scène, après les transparents.
        void Draw(RHI::CommandList& commands) const;
        // En FSR, dans la passe du masque réactif : les mêmes traçantes, leur éclat ajouté au masque.
        void DrawReactive(RHI::CommandList& commands) const;

        private:
        // Doit correspondre à TracerData dans shaders/Tracer.slang.
        struct TracerData
        {
            std::array<float, 4> head;
            std::array<float, 4> tail;
            std::array<float, 4> color;
        };
        static_assert(sizeof(TracerData) == 48);
        // Doit correspondre à TracerParams dans shaders/Tracer.slang.
        struct TracerParams
        {
            Math::Matrix4f viewToClip;
            std::array<float, 4> options;
        };
        static_assert(sizeof(TracerParams) == 80);

        TracerRenderer() = default;
        void Draw(RHI::CommandList& commands, const RHI::Pipeline& pipeline) const;

        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::unique_ptr<RHI::Pipeline> m_reactive;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_buffers;
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_groups;
        std::vector<TracerData> m_tracers; // réservé à MaxTracers : pas d'allocation par image
        TracerParams m_params{};
        std::uint32_t m_frameIndex = 0;
    };
}
