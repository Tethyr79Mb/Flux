#include "Renderer/ThrusterRenderer.hpp"

#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Flux {
    namespace {
        // Boîte un peu plus large que la tuyère : les bords de la flamme remuent au-delà (voir Emission dans
        // shaders/Flame.slang, qui reçoit la sortie de la tuyère dans la boîte : 1 / BoxMargin).
        constexpr float BoxMargin = 1.25f;
        // Longueur de la flamme au ralenti, en part de sa longueur à fond.
        constexpr float IdleLength = 0.15f;
        // Lumière du gaz : un mètre de cœur traversé fait à peu près le blanc à éclat 1.
        constexpr float Gain = 2.5f;
        // Le temps du bruit repart à zéro toutes les heures : en float, il perdrait sa précision au fil des jours.
        constexpr double TimeWrap = 3600.0;
    }

    std::expected<std::unique_ptr<ThrusterRenderer>, std::string> ThrusterRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                            RHI::Format depthFormat) {
        std::unique_ptr<ThrusterRenderer> renderer(new ThrusterRenderer());
        // Caché par ce qui est devant (test de profondeur), sans cacher ce qui est derrière (pas d'écriture). Les
        // deux faces : caméra dans la boîte, seules celles du fond sont vues (le shader garde celles où le rayon
        // entre dans la boîte, ou en sort quand il part de dedans).
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Flame.spv";
        desc.pushConstantsSize = sizeof(FlameParams);
        desc.colorFormat = colorFormat;
        desc.depthFormat = depthFormat;
        desc.depthTest = true;
        desc.depthWrite = false;
        desc.depthCompare = RHI::CompareOp::GreaterOrEqual;
        desc.blend = RHI::BlendMode::Additive;
        auto pipeline = device.CreatePipeline(desc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        renderer->m_pipeline = std::move(*pipeline);
        return renderer;
    }

    Math::Vector3f ThrusterRenderer::GetBox(const ThrusterComponent& thruster, float throttle) {
        return {0.5f * std::max(thruster.width, 0.01f) * BoxMargin, 0.5f * std::max(thruster.height, 0.01f) * BoxMargin,
                std::max(thruster.length, 0.01f) * (IdleLength + (1.0f - IdleLength) * std::clamp(throttle, 0.0f, 1.0f))};
    }

    void ThrusterRenderer::Prepare(std::span<const ThrusterDraw> thrusters, const Camera& camera, float aspect, float exposure) {
        m_flames.clear();
        const Math::Matrix4f viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
        const double seconds = std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count(), TimeWrap);
        const Math::Vector3f forward = camera.GetForward();
        for (const ThrusterDraw& draw : thrusters) {
            const ThrusterComponent& thruster = *draw.thruster;
            const float throttle = std::clamp(draw.throttle, 0.0f, 1.0f);
            if (thruster.intensity <= 0.0f || throttle <= 0.0f) {
                continue;
            }
            const RelativeTransform& transform = draw.transform;
            const Math::Vector3f box = GetBox(thruster, throttle);
            const Math::Vector3f scale{std::abs(transform.scale.x), std::abs(transform.scale.y), std::abs(transform.scale.z)};
            const Math::Vector3f meters{box.x * scale.x, box.y * scale.y, box.z * scale.z};
            if (meters.x < 1.0e-4f || meters.y < 1.0e-4f || meters.z < 1.0e-4f) {
                continue;
            }
            // Derrière la caméra (la sphère de la boîte en entier) : rien à dessiner.
            const Math::Vector3f position(transform.position);
            const Math::Vector3f center = position + transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 0.5f * meters.z});
            if (Math::Dot(center, forward) < -Math::Length(Math::Vector3f{meters.x, meters.y, 0.5f * meters.z})) {
                continue;
            }
            // Repère normalisé : la boîte de -1 à 1 en x et y, de 0 à 1 en z (voir shaders/Flame.slang).
            const Math::Matrix4f boxToWorld = Math::Matrix4f::Translation(position) * transform.rotation.ToMatrix()
                                            * Math::Matrix4f::Scale(Math::Vector3f{box.x * transform.scale.x, box.y * transform.scale.y,
                                                                                    box.z * transform.scale.z});
            const Math::Vector3f local = transform.rotation.Conjugate().Rotate(-position);
            const auto divide = [](float value, float size) { return value / (std::abs(size) > 1.0e-12f ? size : 1.0e-12f); };
            const Math::Vector3f cameraInBox{divide(local.x, box.x * transform.scale.x), divide(local.y, box.y * transform.scale.y),
                                             divide(local.z, box.z * transform.scale.z)};
            // Une flamme courte traverse moins de gaz : plus vive par mètre, pour rester bien visible au ralenti
            // (sinon, à 2 m de long et moitié moins vive, elle disparaissait derrière la tuyère éclairée).
            const float shortness = std::sqrt(IdleLength + (1.0f - IdleLength) * throttle);
            const float brightness = thruster.intensity * (0.6f + 0.4f * throttle) / shortness;
            const std::uint32_t seed = static_cast<std::uint32_t>(transform.entity) * 0x9e3779b9u;
            m_flames.push_back({viewProjection * boxToWorld,
                                {cameraInBox.x, cameraInBox.y, cameraInBox.z, static_cast<float>(seconds)},
                                {thruster.color.r * brightness, thruster.color.g * brightness, thruster.color.b * brightness, throttle},
                                {meters.x, meters.y, meters.z, 1.0f / BoxMargin},
                                {std::bit_cast<float>(seed), exposure * Gain, 0.0f, 0.0f}});
        }
    }

    void ThrusterRenderer::Draw(RHI::CommandList& commands) const {
        if (m_flames.empty()) {
            return;
        }
        commands.BindPipeline(*m_pipeline);
        for (const FlameParams& flame : m_flames) {
            commands.PushConstants(&flame, sizeof(flame));
            commands.Draw(36, 0);
        }
    }
}
