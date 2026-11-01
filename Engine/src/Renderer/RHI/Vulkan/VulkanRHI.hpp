#pragma once

// Backend Vulkan du RHI : chaque interface de Renderer/RHI y est implémentée, une classe par fichier, sur les
// enveloppes Vk:: (Device, Swapchain, Buffer, Image, Pipeline, Transfer). Celui-ci les inclut toutes.

#include "Renderer/RHI/Vulkan/VulkanRhiBindGroup.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiBuffer.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiCommandList.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiDevice.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiPipeline.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiTexture.hpp"
#include "Renderer/RHI/Vulkan/VulkanTypes.hpp"
