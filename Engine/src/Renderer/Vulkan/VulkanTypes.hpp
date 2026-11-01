#pragma once

// Conversions entre les types du RHI (RHITypes.hpp) et ceux de Vulkan, regroupées ici pour tout le backend.

#include "Renderer/RHI/RHITypes.hpp"
#include "Renderer/Vulkan/Vulkan.hpp"

namespace Flux::Vk {
    VkFormat ToVulkan(RHI::Format format);
    RHI::Format FromVulkan(VkFormat format);
    VkBufferUsageFlags ToVulkan(RHI::BufferUsage usage);
    // Avec les mesh shaders, la géométrie (Vertex) comprend les étapes de tâche et de maillage : les mêmes groupes
    // et constantes servent aux deux chemins, et les pipelines restent compatibles d'un chemin à l'autre.
    VkShaderStageFlags ToVulkan(RHI::ShaderStage stages, bool meshShaders);
    VkDescriptorType ToVulkan(RHI::BindingType type);
    VkFilter ToVulkan(RHI::Filter filter);
    VkCompareOp ToVulkan(RHI::CompareOp op);
    VkCullModeFlags ToVulkan(RHI::CullMode mode);
    RHI::PresentMode FromVulkan(VkPresentModeKHR mode);
}
