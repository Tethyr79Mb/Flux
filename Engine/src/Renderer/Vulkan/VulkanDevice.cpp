#include "Renderer/Vulkan/VulkanDevice.hpp"

#include <windows.h>

#include <format>

namespace Flux::Vk {
    std::expected<std::unique_ptr<Device>, std::string> Device::Create(void* nativeWindowHandle) {
        if (volkInitialize() != VK_SUCCESS) {
            return std::unexpected("vulkan-1.dll introuvable : installe ou mets à jour le pilote graphique");
        }

        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<Device> device(new Device());

        vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);
        instanceBuilder.set_app_name("Flux")
                       .set_engine_name("FluxEngine")
                       .require_api_version(1, 3, 0);
#ifndef NDEBUG
        // Actives seulement si le SDK Vulkan est installé ; sinon on continue sans.
        instanceBuilder.request_validation_layers(true)
                       .use_default_debug_messenger();
#endif
        auto instance = instanceBuilder.build();
        if (!instance) {
            return std::unexpected(std::format("création de l'instance Vulkan : {}", instance.error().message()));
        }
        device->m_instance = instance.value();
        volkLoadInstance(device->m_instance.instance);

        VkWin32SurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = static_cast<HWND>(nativeWindowHandle);
        if (!Check(vkCreateWin32SurfaceKHR(device->m_instance.instance, &surfaceInfo, nullptr, &device->m_surface),
                   "vkCreateWin32SurfaceKHR")) {
            return std::unexpected("création de la surface Vulkan");
        }

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        vkb::PhysicalDeviceSelector selector(device->m_instance);
        auto physicalDevice = selector.set_minimum_version(1, 3)
                                      .set_required_features_13(features13)
                                      .set_surface(device->m_surface)
                                      .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                                      .select();
        if (!physicalDevice) {
            return std::unexpected(std::format("aucun GPU compatible Vulkan 1.3 : {}", physicalDevice.error().message()));
        }
        device->m_physicalDevice = physicalDevice.value();

        vkb::DeviceBuilder deviceBuilder(device->m_physicalDevice);
        auto logicalDevice = deviceBuilder.build();
        if (!logicalDevice) {
            return std::unexpected(std::format("création du device Vulkan : {}", logicalDevice.error().message()));
        }
        device->m_device = logicalDevice.value();
        volkLoadDevice(device->m_device.device);

        auto graphicsQueue = device->m_device.get_queue(vkb::QueueType::graphics);
        auto graphicsFamily = device->m_device.get_queue_index(vkb::QueueType::graphics);
        auto presentQueue = device->m_device.get_queue(vkb::QueueType::present);
        if (!graphicsQueue || !graphicsFamily || !presentQueue) {
            return std::unexpected("le GPU n'expose pas de file graphique et de présentation");
        }
        device->m_graphicsQueue = graphicsQueue.value();
        device->m_graphicsQueueFamily = graphicsFamily.value();
        device->m_presentQueue = presentQueue.value();

        VmaVulkanFunctions vmaFunctions{};
        vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.instance = device->m_instance.instance;
        allocatorInfo.physicalDevice = device->m_physicalDevice.physical_device;
        allocatorInfo.device = device->m_device.device;
        allocatorInfo.pVulkanFunctions = &vmaFunctions;
        if (!Check(vmaCreateAllocator(&allocatorInfo, &device->m_allocator), "vmaCreateAllocator")) {
            return std::unexpected("création de l'allocateur VMA");
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = device->m_graphicsQueueFamily;
        if (!Check(vkCreateCommandPool(device->m_device.device, &poolInfo, nullptr, &device->m_immediatePool), "vkCreateCommandPool")) {
            return std::unexpected("création du command pool de chargement");
        }

        std::fprintf(stderr, "Vulkan : %s\n", device->m_physicalDevice.name.c_str());
        return device;
    }

    Device::~Device() {
        // Ordre inverse de la création ; chaque étape tolère un objet jamais créé.
        if (m_device.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device.device);
        }
        if (m_immediatePool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device.device, m_immediatePool, nullptr);
        }
        if (m_allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(m_allocator);
        }
        if (m_device.device != VK_NULL_HANDLE) {
            vkb::destroy_device(m_device);
        }
        if (m_surface != VK_NULL_HANDLE) {
            vkb::destroy_surface(m_instance, m_surface);
        }
        if (m_instance.instance != VK_NULL_HANDLE) {
            vkb::destroy_instance(m_instance);
        }
    }

    void Device::WaitIdle() const {
        Check(vkDeviceWaitIdle(m_device.device), "vkDeviceWaitIdle");
    }

    bool Device::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record) const {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = m_immediatePool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (!Check(vkAllocateCommandBuffers(m_device.device, &allocateInfo, &commandBuffer), "vkAllocateCommandBuffers")) {
            return false;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        record(commandBuffer);
        bool success = Check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

        if (success) {
            VkCommandBufferSubmitInfo commandInfo{};
            commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            commandInfo.commandBuffer = commandBuffer;
            VkSubmitInfo2 submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submitInfo.commandBufferInfoCount = 1;
            submitInfo.pCommandBufferInfos = &commandInfo;
            success = Check(vkQueueSubmit2(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2")
                   && Check(vkQueueWaitIdle(m_graphicsQueue), "vkQueueWaitIdle");
        }
        vkFreeCommandBuffers(m_device.device, m_immediatePool, 1, &commandBuffer);
        return success;
    }
}
