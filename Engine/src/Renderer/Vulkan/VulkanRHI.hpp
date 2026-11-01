#pragma once

// Backend Vulkan du RHI : chaque interface de Renderer/RHI y est implémentée, une classe par fichier, sur les
// enveloppes Vk:: (Device, Swapchain, Buffer, Image, Pipeline, Transfer). Celui-ci les inclut toutes.

#include "Renderer/Vulkan/VulkanRhiBindGroup.hpp"
#include "Renderer/Vulkan/VulkanRhiBuffer.hpp"
#include "Renderer/Vulkan/VulkanRhiCommandList.hpp"
#include "Renderer/Vulkan/VulkanRhiDevice.hpp"
#include "Renderer/Vulkan/VulkanRhiPipeline.hpp"
#include "Renderer/Vulkan/VulkanRhiTexture.hpp"
#include "Renderer/Vulkan/VulkanTypes.hpp"
