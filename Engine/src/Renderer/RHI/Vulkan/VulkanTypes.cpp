#include "Renderer/RHI/Vulkan/VulkanTypes.hpp"

namespace Flux::Vk {
    VkFormat ToVulkan(RHI::Format format) {
        switch (format) {
            case RHI::Format::Undefined: return VK_FORMAT_UNDEFINED;
            case RHI::Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case RHI::Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
            case RHI::Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
            case RHI::Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
            case RHI::Format::D32Float: return VK_FORMAT_D32_SFLOAT;
            case RHI::Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
            case RHI::Format::RGB32Float: return VK_FORMAT_R32G32B32_SFLOAT;
            case RHI::Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case RHI::Format::RGBA16Uint: return VK_FORMAT_R16G16B16A16_UINT;
            case RHI::Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RHI::Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
            case RHI::Format::R8Unorm: return VK_FORMAT_R8_UNORM;
            case RHI::Format::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
            case RHI::Format::BC7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
        }
        return VK_FORMAT_UNDEFINED;
    }

    RHI::Format FromVulkan(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: return RHI::Format::RGBA8Unorm;
            case VK_FORMAT_R8G8B8A8_SRGB: return RHI::Format::RGBA8Srgb;
            case VK_FORMAT_B8G8R8A8_UNORM: return RHI::Format::BGRA8Unorm;
            case VK_FORMAT_B8G8R8A8_SRGB: return RHI::Format::BGRA8Srgb;
            case VK_FORMAT_D32_SFLOAT: return RHI::Format::D32Float;
            default: return RHI::Format::Undefined;
        }
    }

    VkBufferUsageFlags ToVulkan(RHI::BufferUsage usage) {
        VkBufferUsageFlags flags = 0;
        if (RHI::HasFlag(usage, RHI::BufferUsage::Vertex)) {
            flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        }
        if (RHI::HasFlag(usage, RHI::BufferUsage::Index)) {
            flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        }
        if (RHI::HasFlag(usage, RHI::BufferUsage::Storage)) {
            flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        if (RHI::HasFlag(usage, RHI::BufferUsage::Indirect)) {
            flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        }
        return flags;
    }

    VkShaderStageFlags ToVulkan(RHI::ShaderStage stages, bool meshShaders) {
        VkShaderStageFlags flags = 0;
        if (RHI::HasFlag(stages, RHI::ShaderStage::Vertex)) {
            flags |= VK_SHADER_STAGE_VERTEX_BIT;
            if (meshShaders) {
                flags |= VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
            }
        }
        if (RHI::HasFlag(stages, RHI::ShaderStage::Fragment)) {
            flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        if (RHI::HasFlag(stages, RHI::ShaderStage::Compute)) {
            flags |= VK_SHADER_STAGE_COMPUTE_BIT;
        }
        return flags;
    }

    VkDescriptorType ToVulkan(RHI::BindingType type) {
        switch (type) {
            case RHI::BindingType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case RHI::BindingType::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case RHI::BindingType::Texture:
            case RHI::BindingType::TextureTable: break;
        }
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }

    VkFilter ToVulkan(RHI::Filter filter) {
        return filter == RHI::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    }

    VkCompareOp ToVulkan(RHI::CompareOp op) {
        switch (op) {
            case RHI::CompareOp::Greater: return VK_COMPARE_OP_GREATER;
            case RHI::CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case RHI::CompareOp::Less: return VK_COMPARE_OP_LESS;
            case RHI::CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case RHI::CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_ALWAYS;
    }

    VkCullModeFlags ToVulkan(RHI::CullMode mode) {
        switch (mode) {
            case RHI::CullMode::None: return VK_CULL_MODE_NONE;
            case RHI::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
            case RHI::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        }
        return VK_CULL_MODE_NONE;
    }

    RHI::PresentMode FromVulkan(VkPresentModeKHR mode) {
        switch (mode) {
            case VK_PRESENT_MODE_FIFO_KHR: return RHI::PresentMode::Fifo;
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return RHI::PresentMode::FifoRelaxed;
            case VK_PRESENT_MODE_MAILBOX_KHR: return RHI::PresentMode::Mailbox;
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return RHI::PresentMode::Immediate;
            default: return RHI::PresentMode::Other;
        }
    }
}
