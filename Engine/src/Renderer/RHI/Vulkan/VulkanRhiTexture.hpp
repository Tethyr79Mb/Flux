#pragma once

// RHI::Texture sur une Vk::Image, et RHI::Sampler.

#include "Flux/Renderer/RHI/Texture.hpp"
#include "Renderer/RHI/Vulkan/VulkanImage.hpp"

#include <utility>

namespace Flux::Vk {
    class RhiTexture final : public RHI::Texture {
        public:
        // access : usage de départ, Sampled pour une image envoyée (déjà lisible), None pour une cible.
        RhiTexture(Image image, RHI::Format format, VkImageAspectFlags aspect, RHI::TextureAccess access)
            : m_image(std::move(image)), m_format(format), m_aspect(aspect), m_access(access) {}
        RHI::Extent GetExtent() const override { return {m_image.GetExtent().width, m_image.GetExtent().height}; }
        RHI::Format GetFormat() const override { return m_format; }
        const Image& GetImage() const { return m_image; }
        VkImageAspectFlags GetAspect() const { return m_aspect; }
        // Dernier usage enregistré (CommandList::UseTextures). Les commandes passent dans l'ordre d'enregistrement,
        // sur une seule file : c'est l'usage que le GPU lui aura donné quand la commande suivante s'exécutera.
        RHI::TextureAccess GetAccess() const { return m_access; }
        void SetAccess(RHI::TextureAccess access) const { m_access = access; }

        private:
        Image m_image;
        RHI::Format m_format;
        VkImageAspectFlags m_aspect;
        mutable RHI::TextureAccess m_access;
    };

    class RhiSampler final : public RHI::Sampler {
        public:
        RhiSampler(VkDevice device, VkSampler sampler) : m_device(device), m_sampler(sampler) {}
        ~RhiSampler() override { vkDestroySampler(m_device, m_sampler, nullptr); }
        RhiSampler(const RhiSampler&) = delete;
        RhiSampler& operator=(const RhiSampler&) = delete;
        VkSampler GetHandle() const { return m_sampler; }

        private:
        VkDevice m_device;
        VkSampler m_sampler;
    };
}
