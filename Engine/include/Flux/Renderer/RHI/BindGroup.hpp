#pragma once

// Groupes de ressources liés aux shaders et leurs layouts.

#include "Flux/Renderer/RHI/AccelerationStructure.hpp"
#include "Flux/Renderer/RHI/Buffer.hpp"
#include "Flux/Renderer/RHI/Texture.hpp"

#include <cstdint>
#include <span>

namespace Flux::RHI {
    struct Binding
    {
        std::uint32_t slot = 0;
        BindingType type = BindingType::Texture;
        ShaderStage stages = ShaderStage::Fragment;
        std::uint32_t count = 1; // TextureTable seulement : nombre de cases
    };

    struct BindGroupLayoutDesc
    {
        std::span<const Binding> bindings;
        // Groupes de ce type vivants en même temps, au plus (réservés d'avance) ; au-delà, CreateBindGroup échoue.
        std::uint32_t maxGroups = 1;
    };

    class BindGroupLayout {
        public:
        virtual ~BindGroupLayout() = default;
    };

    // Ressources liées ensemble aux shaders (un « descriptor set » Vulkan).
    class BindGroup {
        public:
        virtual ~BindGroup() = default;
        virtual void SetTexture(std::uint32_t slot, const Texture& texture, const Sampler& sampler) = 0;
        virtual void SetBuffer(std::uint32_t slot, const Buffer& buffer) = 0;
        // Case `element` d'une table de textures (BindingType::TextureTable).
        virtual void SetTableTexture(std::uint32_t slot, std::uint32_t element, const Texture& texture, const Sampler& sampler) = 0;
        // Niveau 0 d'une texture écrite par les compute shaders (BindingType::StorageTexture), en TextureAccess::Storage
        // quand ils la lisent.
        virtual void SetStorageTexture(std::uint32_t slot, const Texture& texture) = 0;
        // Structure haute (BindingType::AccelerationStructure), construite avant que les shaders la lisent.
        virtual void SetAccelerationStructure(std::uint32_t slot, const AccelerationStructure& structure) = 0;
    };
}
