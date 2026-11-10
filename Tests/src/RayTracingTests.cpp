// Tests des structures du ray tracing (RHI::Device::HasRayTracing) : une structure basse construite depuis des sommets
// et des indices décalés dans leurs buffers (comme une page de géométrie), puis une structure haute qui la désigne
// deux fois, reconstruite avec plus puis moins d'instances. Ignorés (réussis) sur un GPU sans ray tracing.
#include "Check.hpp"

#include "Flux/Assets/Model.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

using namespace Flux;

bool RunRayTracingTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("ray tracing", device.error().c_str());
    }
    if (!(*device)->HasRayTracing()) {
        std::printf("       ray tracing ignoré : %s n'en a pas\n", std::string((*device)->GetName()).c_str());
        return true;
    }

    // Deux sommets de remplissage avant le modèle : les indices sont relatifs à son premier sommet.
    std::vector<ModelVertex> vertices(6);
    vertices[2].position = {0.0f, 0.0f, 0.0f};
    vertices[3].position = {1.0f, 0.0f, 0.0f};
    vertices[4].position = {0.0f, 1.0f, 0.0f};
    vertices[5].position = {1.0f, 1.0f, 0.0f};
    const std::array<std::uint32_t, 9> indices{7, 7, 7, 0, 1, 2, 2, 1, 3}; // trois indices de remplissage
    const RHI::BufferUsage input = RHI::BufferUsage::AccelerationInput;
    auto vertexBuffer = (*device)->CreateBuffer({vertices.size() * sizeof(ModelVertex), RHI::BufferUsage::Vertex | input},
                                                vertices.data());
    auto indexBuffer = (*device)->CreateBuffer({sizeof(indices), RHI::BufferUsage::Index | input}, indices.data());
    CHECK(vertexBuffer && indexBuffer);

    const std::array<RHI::AccelerationGeometry, 2> geometries{
        RHI::AccelerationGeometry{vertexBuffer->get(), 2 * sizeof(ModelVertex), sizeof(ModelVertex), 4, indexBuffer->get(),
                                  3 * sizeof(std::uint32_t), 3, true},
        RHI::AccelerationGeometry{vertexBuffer->get(), 2 * sizeof(ModelVertex), sizeof(ModelVertex), 4, indexBuffer->get(),
                                  6 * sizeof(std::uint32_t), 3, false}};
    auto bottom = (*device)->CreateBottomLevel(geometries);
    if (!bottom) {
        std::printf("ÉCHEC structure basse : %s\n", bottom.error().c_str());
        return false;
    }
    CHECK((*bottom)->GetSize() > 0 && (*bottom)->GetScratchSize() > 0 && (*bottom)->GetMaxInstances() == 0);
    CHECK(!(*device)->CreateBottomLevel({}).has_value()); // sans triangles : refusée

    auto top = (*device)->CreateTopLevel(4);
    CHECK(top.has_value());
    CHECK((*top)->GetMaxInstances() == 4 && (*top)->GetSize() > 0);
    const std::uint64_t scratchSize = std::max((*bottom)->GetScratchSize(), (*top)->GetScratchSize());
    auto scratch = (*device)->CreateBuffer({scratchSize, RHI::BufferUsage::AccelerationScratch});
    CHECK(scratch.has_value());

    std::array<RHI::AccelerationInstance, 2> instances{};
    for (std::size_t i = 0; i < instances.size(); ++i) {
        instances[i].transform = {{{1.0f, 0.0f, 0.0f, static_cast<float>(i) * 3.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, -5.0f}}};
        instances[i].customIndex = static_cast<std::uint32_t>(i);
        instances[i].bottom = bottom->get();
    }
    RHI::AccelerationStructure* toBuild = bottom->get();
    CHECK((*device)->Submit([&](RHI::CommandList& commands) { commands.BuildBottomLevels({&toBuild, 1}, **scratch); }));
    CHECK((*device)->Submit([&](RHI::CommandList& commands) { commands.BuildTopLevel(**top, instances, **scratch); }));
    // Reconstruite avec une seule instance, puis aucune : la structure haute se refait à chaque image.
    CHECK((*device)->Submit([&](RHI::CommandList& commands) {
        commands.BuildTopLevel(**top, std::span(instances).first(1), **scratch);
    }));
    CHECK((*device)->Submit([&](RHI::CommandList& commands) { commands.BuildTopLevel(**top, {}, **scratch); }));
    return true;
}
