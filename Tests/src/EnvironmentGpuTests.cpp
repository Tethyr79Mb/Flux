// Reflets et lumière du ciel calculés par le GPU (EnvironmentRenderer) depuis un dégradé connu, relus dans les six
// directions des axes : lumière au-dessus de 1 gardée (HDR), reflet net égal au ciel, lumière diffuse égale à sa
// moyenne pondérée par le cosinus, reflet rugueux entre les deux. Passe aussi par les textures cube à plusieurs niveaux
// de mip du RHI (dessin dans un niveau, GenerateMipmaps). Ignorés (réussis) sur une machine sans GPU Vulkan 1.3, sauf
// avec FLUX_TESTS_GPU=1 (la CI, sur lavapipe).
#include "Check.hpp"

#include "Flux/Renderer/RHI/RHI.hpp"
#include "Renderer/EnvironmentRenderer.hpp"

#include <array>
#include <cmath>
#include <cstdio>

using namespace Flux;

namespace {
    using Texel = std::array<float, 4>;
    using Directions = std::array<Texel, 6>; // +X, -X, +Y, -Y, +Z, -Z

    // La texture cube vue dans les six directions des axes, au niveau `level` ; vide (zéros) en cas d'échec.
    bool ReadCube(RHI::Device& device, const RHI::Texture& cube, float level, Directions& texels) {
        auto output = device.CreateBuffer({sizeof(Directions), RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
        constexpr std::array bindings{RHI::Binding{0, RHI::BindingType::Texture, RHI::ShaderStage::Compute},
                                      RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute}};
        auto layout = device.CreateBindGroupLayout({bindings, 1});
        auto sampler = device.CreateSampler({RHI::Filter::Linear, RHI::Filter::Linear});
        if (!output || !layout || !sampler) {
            return false;
        }
        const std::array<const RHI::BindGroupLayout*, 1> layouts{layout->get()};
        auto pipeline = device.CreateComputePipeline({"shaders/EnvironmentTests.spv", "computeMain", layouts, sizeof(float)});
        if (!pipeline) {
            std::printf("ÉCHEC %s\n", pipeline.error().c_str());
            return false;
        }
        auto group = device.CreateBindGroup(**layout);
        group->SetTexture(0, cube, **sampler);
        group->SetBuffer(1, **output);
        const bool submitted = device.Submit([&](RHI::CommandList& commands) {
            commands.BindPipeline(**pipeline);
            commands.SetBindGroup(0, *group);
            commands.PushConstants(&level, sizeof(level));
            commands.Dispatch(1);
            commands.ComputeBarrier();
        });
        if (!submitted) {
            return false;
        }
        (*output)->Read(texels.data(), sizeof(Directions));
        return true;
    }

    bool Near(float value, float expected, float tolerance) {
        if (std::abs(value - expected) <= tolerance) {
            return true;
        }
        std::printf("       %.4f au lieu de %.4f (à %.4f près)\n", value, expected, tolerance);
        return false;
    }
}

bool RunEnvironmentGpuTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("environnement", device.error().c_str());
    }
    auto environment = EnvironmentRenderer::Create(**device);
    if (!environment) {
        std::printf("ÉCHEC %s\n", environment.error().c_str());
        return false;
    }
    // Ciel en dégradé : 0 en bas, 4 en haut (au-dessus de 1 : l'ancien cube 8 bits l'aurait coupé), soit 2 (y + 1)
    // dans la direction d'altitude y.
    EnvironmentSource source;
    source.top = {4.0f, 4.0f, 4.0f};
    source.bottom = {0.0f, 0.0f, 0.0f};
    (*environment)->Prepare(source, 0);
    CHECK((*environment)->Pending());
    const RHI::Texture& reflections = (*environment)->GetReflections();
    const RHI::Texture& irradiance = (*environment)->GetIrradiance();
    const bool baked = (**device).Submit([&](RHI::CommandList& commands) {
        const std::array targets{RHI::TextureUse{&reflections, RHI::TextureAccess::ColorTarget, true},
                                 RHI::TextureUse{&irradiance, RHI::TextureAccess::ColorTarget, true}};
        commands.UseTextures(targets);
        (*environment)->Bake(commands, 0);
        const std::array read{RHI::TextureUse{&reflections, RHI::TextureAccess::Sampled},
                              RHI::TextureUse{&irradiance, RHI::TextureAccess::Sampled}};
        commands.UseTextures(read);
    });
    CHECK(baked);
    CHECK(!(*environment)->Pending());
    // Même source : rien à refaire.
    (*environment)->Prepare(source, 0);
    CHECK(!(*environment)->Pending());

    Directions sharp{};
    Directions rough{};
    Directions diffuse{};
    CHECK(ReadCube(**device, reflections, 0.0f, sharp));
    CHECK(ReadCube(**device, reflections, static_cast<float>((*environment)->GetReflectionMips() - 1), rough));
    CHECK(ReadCube(**device, irradiance, 0.0f, diffuse));
    for (int i = 0; i < 6; ++i) {
        std::printf("       direction %d : net %.3f, rugueux %.3f, diffus %.3f\n", i, sharp[i][0], rough[i][0], diffuse[i][0]);
    }
    // Reflet net : le ciel lui-même (au milieu d'une face, à un demi-texel près).
    CHECK(Near(sharp[2][0], 4.0f, 0.05f));
    CHECK(Near(sharp[3][0], 0.0f, 0.05f));
    CHECK(Near(sharp[0][0], 2.0f, 0.05f));
    // Lumière diffuse divisée par pi : 2 (moyenne de y pondérée par le cosinus + 1), cette moyenne valant 2/3 vers le
    // haut, -2/3 vers le bas et 0 à l'horizontale.
    CHECK(Near(diffuse[2][0], 10.0f / 3.0f, 0.1f));
    CHECK(Near(diffuse[3][0], 2.0f / 3.0f, 0.1f));
    CHECK(Near(diffuse[4][0], 2.0f, 0.1f));
    // Reflet le plus rugueux : moins net que le ciel, mais pas moins que la lumière diffuse.
    CHECK(rough[2][0] < sharp[2][0] - 0.1f && rough[2][0] > diffuse[2][0] - 0.2f);
    CHECK(rough[3][0] > sharp[3][0] + 0.1f);
    return true;
}
