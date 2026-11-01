#pragma once

// RHI::Pipeline sur un Vk::Pipeline.

#include "Flux/Renderer/RHI/Pipeline.hpp"
#include "Renderer/RHI/Vulkan/VulkanPipeline.hpp"

#include <utility>

namespace Flux::Vk {
    class RhiPipeline final : public RHI::Pipeline {
        public:
        explicit RhiPipeline(Vk::Pipeline pipeline) : m_pipeline(std::move(pipeline)) {}
        const Vk::Pipeline& Get() const { return m_pipeline; }

        private:
        Vk::Pipeline m_pipeline;
    };
}
