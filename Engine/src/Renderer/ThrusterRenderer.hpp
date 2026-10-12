#pragma once

#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    struct Camera;

    // Un réacteur à dessiner (ThrusterComponent), relevé par MeshRenderer::Prepare.
    struct ThrusterDraw
    {
        RelativeTransform transform;
        const ThrusterComponent* thruster = nullptr;
        float throttle = 0.0f; // poussée montrée, de 0 (ralenti) à 1, ralenti compris
    };

    // Flammes des réacteurs : un volume de gaz lumineux calculé au pixel (shaders/Flame.slang) dans la boîte qui
    // l'enferme, ajouté à l'image après les transparents. Un appel de dessin par flamme (36 sommets, sans buffer) ;
    // le coût est au pixel couvert par la boîte : une vingtaine de pas de bruit chacun.
    class ThrusterRenderer {
        public:
        static std::expected<std::unique_ptr<ThrusterRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                     RHI::Format depthFormat);

        // Flammes de l'image : boîtes relatives à la caméra, celles hors du champ écartées. `exposure` : celle des
        // maillages (SkyLighting).
        void Prepare(std::span<const ThrusterDraw> thrusters, const Camera& camera, float aspect, float exposure);
        // Dans la passe principale, après les transparents.
        void Draw(RHI::CommandList& commands) const;

        // Boîte de la flamme dans le repère de l'entité : demi-largeur, demi-hauteur (un peu plus grandes que la
        // tuyère : la flamme remue) et longueur (selon la poussée).
        static Math::Vector3f GetBox(const ThrusterComponent& thruster, float throttle);

        private:
        // Doit correspondre à FlameParams dans shaders/Flame.slang.
        struct FlameParams
        {
            Math::Matrix4f boxToClip;
            std::array<float, 4> camera;
            std::array<float, 4> color;
            std::array<float, 4> size;
            std::array<float, 4> options;
        };
        static_assert(sizeof(FlameParams) == 128);

        ThrusterRenderer() = default;

        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::vector<FlameParams> m_flames;
        std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
    };
}
