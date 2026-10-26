#include "Renderer/TracerRenderer.hpp"

#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Renderer/TemporalView.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        constexpr std::uint32_t DataSlot = 0;
        // Longueur du trait d'une traçante (m) et sa largeur au plus près (le shader l'élargit au loin).
        constexpr float TracerLength = 12.0f;
        constexpr float TracerWidth = 0.12f;
        // Missile : un point plus gros, à la traîne plus longue (sa flamme et sa fumée chaude).
        constexpr float MissileLength = 30.0f;
        constexpr float MissileWidth = 0.4f;
        // Éclat du cœur : assez pour déborder en bloom, comme une flamme.
        constexpr float Brightness = 40.0f;
        // Éclat d'impact : largeur (m), éclat par rapport à une traçante, et durée sur laquelle il pâlit (s), celle
        // donnée par le moteur (Engine.cpp, ImpactSeconds).
        constexpr float ImpactWidth = 1.2f;
        constexpr float ImpactGlow = 2.0f;
        constexpr float ImpactFade = 0.12f;
    }

    std::expected<std::unique_ptr<TracerRenderer>, std::string> TracerRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                        RHI::Format motionFormat,
                                                                                        RHI::Format depthFormat,
                                                                                        RHI::Format reactiveFormat) {
        std::unique_ptr<TracerRenderer> renderer(new TracerRenderer());
        constexpr std::array bindings{RHI::Binding{DataSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Vertex}};
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = RHI::FramesInFlight});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_layout = std::move(*layout);
        for (std::uint32_t f = 0; f < RHI::FramesInFlight; ++f) {
            auto buffer = device.CreateBuffer({MaxTracers * sizeof(TracerData), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            renderer->m_groups[f] = device.CreateBindGroup(*renderer->m_layout);
            if (!buffer || !renderer->m_groups[f]) {
                return std::unexpected("création des ressources des traçantes");
            }
            renderer->m_buffers[f] = std::move(*buffer);
            renderer->m_groups[f]->SetBuffer(DataSlot, *renderer->m_buffers[f]);
        }

        // Comme les flammes : cachées par ce qui est devant, sans rien cacher, lumière ajoutée dans n'importe quel ordre.
        const RHI::BindGroupLayout* layouts[] = {renderer->m_layout.get()};
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Tracer.spv";
        desc.bindGroupLayouts = layouts;
        desc.pushConstantsSize = sizeof(TracerParams);
        desc.colorFormat = colorFormat;
        desc.secondColorFormat = motionFormat; // mouvement laissé à celui de ce qui est derrière
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
        desc.fragmentEntryPoint = "fragmentReactive";
        desc.colorFormat = reactiveFormat;
        desc.secondColorFormat = RHI::Format::Undefined;
        auto reactive = device.CreatePipeline(desc);
        if (!reactive) {
            return std::unexpected(reactive.error());
        }
        renderer->m_reactive = std::move(*reactive);
        renderer->m_tracers.reserve(MaxTracers);
        return renderer;
    }

    void TracerRenderer::Prepare(std::span<const Projectile> projectiles, std::span<const Impact> impacts, const Camera& camera, float aspect, float height,
                                 Math::Vector2f jitter, std::uint32_t frameIndex) {
        m_tracers.clear();
        m_frameIndex = frameIndex;
        const Math::Vector3f forward = camera.GetForward();
        // Trop de projectiles : ceux du début de la liste ne sont pas dessinés (ils volent quand même).
        const std::size_t first = projectiles.size() > MaxTracers ? projectiles.size() - MaxTracers : 0;
        for (std::size_t i = first; i < projectiles.size(); ++i) {
            const Projectile& projectile = projectiles[i];
            const float speed = Math::Length(projectile.velocity);
            if (speed <= 0.0f) {
                continue;
            }
            const Math::Vector3f head(Math::Delta(camera.position, projectile.position));
            const float length = std::min(projectile.missile ? MissileLength : TracerLength, projectile.age * speed);
            if (Math::Dot(head, forward) < -length) {
                continue; // derrière la caméra, le trait entier
            }
            const Math::Vector3f tail = head - projectile.velocity * (length / speed);
            const Math::LinearColor& color = projectile.color;
            m_tracers.push_back({{head.x, head.y, head.z, projectile.missile ? MissileWidth : TracerWidth},
                                 {tail.x, tail.y, tail.z, 0.0f},
                                 {color.r * Brightness, color.g * Brightness, color.b * Brightness, 0.0f}});
        }
        // Éclats : un trait de longueur nulle marqué rond (le shader en fait un disque de sa largeur), plus large et plus vif
        // qu'une traçante au début, qui pâlit.
        for (const Impact& impact : impacts) {
            if (m_tracers.size() >= MaxTracers) {
                break;
            }
            const Math::Vector3f at(Math::Delta(camera.position, impact.position));
            const float width = ImpactWidth * impact.size;
            if (Math::Dot(at, forward) < -width) {
                continue;
            }
            const float glow = Brightness * ImpactGlow * std::clamp(impact.life / ImpactFade, 0.0f, 1.0f);
            const Math::LinearColor& color = impact.color;
            m_tracers.push_back({{at.x, at.y, at.z, width},
                                 {at.x, at.y, at.z, 1.0f},
                                 {color.r * glow, color.g * glow, color.b * glow, 0.0f}});
        }
        if (m_tracers.empty()) {
            return;
        }
        m_buffers[frameIndex]->Write(m_tracers.data(), m_tracers.size() * sizeof(TracerData));
        m_params.viewToClip = Jittered(camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix(), jitter);
        m_params.options = {2.0f * std::tan(0.5f * camera.verticalFov) / std::max(height, 1.0f), 0.0f, 0.0f, 0.0f};
    }

    void TracerRenderer::Draw(RHI::CommandList& commands) const {
        Draw(commands, *m_pipeline);
    }

    void TracerRenderer::DrawReactive(RHI::CommandList& commands) const {
        Draw(commands, *m_reactive);
    }

    void TracerRenderer::Draw(RHI::CommandList& commands, const RHI::Pipeline& pipeline) const {
        if (m_tracers.empty()) {
            return;
        }
        commands.BindPipeline(pipeline);
        commands.SetBindGroup(0, *m_groups[m_frameIndex]);
        commands.PushConstants(&m_params, sizeof(m_params));
        commands.Draw(static_cast<std::uint32_t>(m_tracers.size()) * 6, 0);
    }
}
