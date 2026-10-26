#include "Planetology/CloudRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Renderer/MeshRenderer.hpp"

namespace Flux::Planetology {
    namespace {
        // Doit correspondre à DrawParams dans shaders/Mesh.slang (MeshRenderer.cpp), dont Clouds.slang lit ses réglages :
        // color.xy, 1 / taille de la demi-résolution ; color.z, 1 si l'historique est valable ; color.w, part de
        // l'image actuelle ; emissive.xy, taille de la pleine résolution ; emissive.zw, de la demi-résolution.
        struct CloudParams
        {
            std::array<float, 4> color;
            std::array<float, 4> emissive;
            std::array<std::uint32_t, 12> unused;
        };
        static_assert(sizeof(CloudParams) == 80);

        // Part de l'image actuelle mêlée à l'historique : la moyenne d'une dizaine d'images.
        constexpr float CurrentWeight = 0.1f;
    }

    std::expected<std::unique_ptr<CloudRenderer>, std::string> CloudRenderer::Create(RHI::Device& device, const MeshRenderer& meshes,
                                                                                    RHI::Format sceneFormat) {
        auto noise = CloudNoise::Create(device);
        if (!noise) {
            return std::unexpected(noise.error());
        }
        std::unique_ptr<CloudRenderer> renderer(new CloudRenderer(device, meshes, std::move(*noise)));
        auto noiseSampler = device.CreateSampler({});
        auto linearSampler = device.CreateSampler({.address = RHI::AddressMode::ClampToEdge});
        auto pointSampler = device.CreateSampler({RHI::Filter::Nearest, RHI::Filter::Nearest, RHI::AddressMode::ClampToEdge});
        if (!noiseSampler || !linearSampler || !pointSampler) {
            return std::unexpected("nuages : samplers");
        }
        renderer->m_noiseSampler = std::move(*noiseSampler);
        renderer->m_linearSampler = std::move(*linearSampler);
        renderer->m_pointSampler = std::move(*pointSampler);

        std::array<RHI::Binding, 6> bindings{};
        for (std::uint32_t slot = 0; slot < bindings.size(); ++slot) {
            bindings[slot] = {slot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment};
        }
        auto layout = device.CreateBindGroupLayout({bindings, 5});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_layout = std::move(*layout);

        const std::array<const RHI::BindGroupLayout*, 3> scene = meshes.GetSceneLayouts();
        const std::array<const RHI::BindGroupLayout*, 4> layouts{scene[0], scene[1], scene[2], renderer->m_layout.get()};
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Clouds.spv";
        desc.vertexEntryPoint = "vertexCloudPass";
        desc.bindGroupLayouts = layouts;
        desc.pushConstantsSize = sizeof(CloudParams);
        const auto create = [&](std::unique_ptr<RHI::Pipeline>& pipeline, const char* fragment) {
            desc.fragmentEntryPoint = fragment;
            auto created = device.CreatePipeline(desc);
            if (created) {
                pipeline = std::move(*created);
            }
            return created.has_value() ? std::expected<void, std::string>{} : std::unexpected(created.error());
        };
        desc.colorFormat = LightFormat;
        desc.secondColorFormat = DistanceFormat;
        desc.writeSecondColor = true;
        auto created = create(renderer->m_march, "fragmentCloudMarch");
        desc.secondColorFormat = RHI::Format::Undefined;
        desc.writeSecondColor = false;
        if (created) {
            created = create(renderer->m_resolve, "fragmentCloudResolve");
        }
        desc.colorFormat = sceneFormat;
        desc.blend = RHI::BlendMode::Alpha;
        if (created) {
            created = create(renderer->m_composite, "fragmentCloudComposite");
        }
        if (!created) {
            return std::unexpected(created.error());
        }
        return renderer;
    }

    void CloudRenderer::Bind(const Targets& targets) {
        m_targets = targets;
        m_historyValid = false;
        // Les anciens groupes rendus d'abord : le pool n'en tient que cinq, les nouveaux n'y entreraient pas.
        m_marchGroup.reset();
        m_resolveGroups = {};
        m_compositeGroups = {};
        m_marchGroup = m_device.CreateBindGroup(*m_layout);
        m_marchGroup->SetTexture(0, m_noise.GetShape(), *m_noiseSampler);
        m_marchGroup->SetTexture(1, m_noise.GetDetail(), *m_noiseSampler);
        m_marchGroup->SetTexture(2, *targets.sceneDepth, *m_pointSampler);
        for (std::uint32_t i = 0; i < 2; ++i) {
            m_resolveGroups[i] = m_device.CreateBindGroup(*m_layout);
            m_resolveGroups[i]->SetTexture(3, *targets.light, *m_linearSampler);
            m_resolveGroups[i]->SetTexture(4, *targets.distances, *m_pointSampler);
            m_resolveGroups[i]->SetTexture(5, *targets.history[1 - i], *m_linearSampler);
            m_compositeGroups[i] = m_device.CreateBindGroup(*m_layout);
            m_compositeGroups[i]->SetTexture(2, *targets.sceneDepth, *m_pointSampler);
            m_compositeGroups[i]->SetTexture(4, *targets.distances, *m_pointSampler);
            m_compositeGroups[i]->SetTexture(5, *targets.history[i], *m_linearSampler);
        }
    }

    void CloudRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex, const RHI::Pipeline& pipeline,
                             const RHI::BindGroup& group, float historyValid) const {
        const RHI::Extent full = m_targets.sceneDepth->GetExtent();
        const RHI::Extent half = m_targets.light->GetExtent();
        const CloudParams params{{1.0f / static_cast<float>(half.width), 1.0f / static_cast<float>(half.height), historyValid, CurrentWeight},
                                 {static_cast<float>(full.width), static_cast<float>(full.height), static_cast<float>(half.width),
                                  static_cast<float>(half.height)},
                                 {}};
        commands.BindPipeline(pipeline);
        m_meshes.BindScene(commands, frameIndex);
        commands.SetBindGroup(3, group);
        commands.PushConstants(&params, sizeof(params));
        commands.Draw(3, 0);
    }

    void CloudRenderer::DrawMarch(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        Draw(commands, frameIndex, *m_march, *m_marchGroup, 0.0f);
    }

    void CloudRenderer::DrawResolve(RHI::CommandList& commands, std::uint32_t frameIndex, std::uint32_t target) {
        Draw(commands, frameIndex, *m_resolve, *m_resolveGroups[target], m_historyValid ? 1.0f : 0.0f);
        m_historyValid = true;
    }

    void CloudRenderer::DrawComposite(RHI::CommandList& commands, std::uint32_t frameIndex, std::uint32_t target) const {
        Draw(commands, frameIndex, *m_composite, *m_compositeGroups[target], 0.0f);
    }
}
