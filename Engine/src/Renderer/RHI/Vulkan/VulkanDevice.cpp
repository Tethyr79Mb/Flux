#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Environment.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Flux::Vk {
    std::expected<std::unique_ptr<Device>, std::string> Device::Create(void* nativeWindowHandle) {
        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<Device> device(new Device());

        // Sans fenêtre (calculs, tests) : pas de surface ni de file de présentation.
        const bool headless = nativeWindowHandle == nullptr;
        auto instance = Instance::Create(headless);
        if (!instance) {
            return std::unexpected(instance.error());
        }
        device->m_instance = std::move(*instance);
        if (!headless) {
            auto surface = Surface::Create(*device->m_instance, nativeWindowHandle);
            if (!surface) {
                return std::unexpected(surface.error());
            }
            device->m_surface = std::move(*surface);
        }
        auto adapter = Adapter::Select(*device->m_instance, device->GetSurface());
        if (!adapter) {
            return std::unexpected(adapter.error());
        }
        device->m_adapter = std::move(*adapter);

        vkb::DeviceBuilder deviceBuilder(device->m_adapter.Get());
        auto logicalDevice = deviceBuilder.build();
        if (!logicalDevice) {
            return std::unexpected(std::format("création du device Vulkan : {}", logicalDevice.error().message()));
        }
        device->m_device = logicalDevice.value();
        volkLoadDevice(device->m_device.device);

        auto graphicsQueue = device->m_device.get_queue(vkb::QueueType::graphics);
        auto graphicsFamily = device->m_device.get_queue_index(vkb::QueueType::graphics);
        if (!graphicsQueue || !graphicsFamily) {
            return std::unexpected("le GPU n'expose pas de file graphique");
        }
        device->m_graphicsQueue = graphicsQueue.value();
        device->m_graphicsQueueFamily = graphicsFamily.value();
        if (!headless) {
            auto presentQueue = device->m_device.get_queue(vkb::QueueType::present);
            if (!presentQueue) {
                return std::unexpected("le GPU n'expose pas de file de présentation");
            }
            device->m_presentQueue = presentQueue.value();
        }
        // File de transfert : d'une famille sans graphique ni calcul de préférence (moteur de copie des Radeon et
        // des GeForce), sinon d'une famille sans graphique. Sans elle (ou si vk-bootstrap rend la famille graphique),
        // les copies passent par la file graphique.
        // FLUX_TRANSFER_QUEUE=0 l'ignore : diagnostic d'un pilote fautif, et comparaison des deux chemins.
        Transfer::Setup transferSetup;
        transferSetup.device = device->m_device.device;
        transferSetup.graphicsQueue = device->m_graphicsQueue;
        transferSetup.graphicsFamily = device->m_graphicsQueueFamily;
        if (Platform::ReadEnvironment("FLUX_TRANSFER_QUEUE") != "0") {
            auto queue = device->m_device.get_dedicated_queue(vkb::QueueType::transfer);
            auto family = device->m_device.get_dedicated_queue_index(vkb::QueueType::transfer);
            if (!queue || !family) {
                queue = device->m_device.get_queue(vkb::QueueType::transfer);
                family = device->m_device.get_queue_index(vkb::QueueType::transfer);
            }
            if (queue && family) {
                transferSetup.transferQueue = queue.value();
                transferSetup.transferFamily = family.value();
            }
        }

        VmaVulkanFunctions vmaFunctions{};
        vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.instance = device->m_instance->GetHandle();
        allocatorInfo.physicalDevice = device->m_adapter.GetHandle();
        allocatorInfo.device = device->m_device.device;
        allocatorInfo.pVulkanFunctions = &vmaFunctions;
        // Ray tracing : les structures d'accélération désignent leurs buffers par adresse.
        if (device->m_adapter.HasRayTracing()) {
            allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        }
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
        transferSetup.allocator = device->m_allocator;
        auto transfer = Transfer::Create(transferSetup);
        if (!transfer) {
            return std::unexpected(transfer.error());
        }
        device->m_transfer = std::move(*transfer);

        Log::Info("Vulkan {} : {}", device->m_adapter.GetApiVersion(), device->m_adapter.GetName());
        return device;
    }

    Device::~Device() {
        // Ordre inverse de la création ; chaque étape tolère un objet jamais créé.
        if (m_device.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device.device);
        }
        m_transfer.reset(); // avant l'allocateur : son anneau et ses buffers d'envoi y sont pris
        if (m_immediatePool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device.device, m_immediatePool, nullptr);
        }
        if (m_allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(m_allocator);
        }
        if (m_device.device != VK_NULL_HANDLE) {
            vkb::destroy_device(m_device);
        }
        // Puis la surface et l'instance (membres, dans l'ordre inverse de leur déclaration).
    }

    float Device::GetTimestampPeriod() const {
        return m_adapter.GetTimestampPeriod(m_graphicsQueueFamily);
    }

    std::string Device::DescribeFault() const {
        if (!m_adapter.HasDeviceFault() || !vkGetDeviceFaultInfoEXT) {
            return {};
        }
        VkDeviceFaultCountsEXT counts{};
        counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        if (vkGetDeviceFaultInfoEXT(m_device.device, &counts, nullptr) < VK_SUCCESS) {
            return {};
        }
        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
        counts.vendorBinarySize = 0; // données brutes du fabricant : illisibles sans ses outils
        VkDeviceFaultInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        info.pAddressInfos = addresses.data();
        info.pVendorInfos = vendors.data();
        if (vkGetDeviceFaultInfoEXT(m_device.device, &counts, &info) < VK_SUCCESS) {
            return {};
        }
        std::string text = info.description;
        for (std::uint32_t i = 0; i < counts.addressInfoCount; ++i) {
            const VkDeviceFaultAddressInfoEXT& address = addresses[i];
            const char* type = "adresse";
            switch (address.addressType) {
                case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: type = "lecture invalide"; break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: type = "écriture invalide"; break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: type = "exécution invalide"; break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: type = "instruction"; break;
                default: break;
            }
            text += std::format("\n  {} : 0x{:x}", type, address.reportedAddress);
        }
        for (std::uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
            text += std::format("\n  {} (code 0x{:x}, données 0x{:x})", vendors[i].description, vendors[i].vendorFaultCode,
                                vendors[i].vendorFaultData);
        }
        return text;
    }

    void Device::WaitIdle() const {
        Check(vkDeviceWaitIdle(m_device.device), "vkDeviceWaitIdle");
    }

    bool Device::SubmitAndWait(std::span<const VkCommandBuffer> commandBuffers, bool recorded) {
        bool success = recorded;

        // Les envois en attente d'abord : ces commandes les lisent peut-être.
        Transfer::GraphicsPart uploads = m_transfer->TakeGraphics();
        if (success) {
            std::vector<VkCommandBufferSubmitInfo> commandInfos;
            commandInfos.reserve(uploads.commands.size() + commandBuffers.size());
            for (VkCommandBuffer commands : uploads.commands) {
                commandInfos.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, commands, 0});
            }
            for (VkCommandBuffer commands : commandBuffers) {
                commandInfos.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, commands, 0});
            }
            const auto count = static_cast<std::uint32_t>(commandInfos.size());
            const VkSemaphoreSubmitInfo wait = m_transfer->GetWait(uploads);
            const VkSemaphoreSubmitInfo signal = m_transfer->GetSignal();
            VkSubmitInfo2 submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submitInfo.waitSemaphoreInfoCount = uploads.transferValue > 0 ? 1 : 0;
            submitInfo.pWaitSemaphoreInfos = &wait;
            submitInfo.commandBufferInfoCount = count;
            submitInfo.pCommandBufferInfos = commandInfos.data();
            submitInfo.signalSemaphoreInfoCount = 1;
            submitInfo.pSignalSemaphoreInfos = &signal;
            success = Check(vkQueueSubmit2(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2");
        }
        m_transfer->GraphicsSubmitted(uploads, success);
        // Seulement cette soumission (et ce qui la précède), pas toute la file comme vkQueueWaitIdle.
        success = success && m_transfer->WaitGraphics();
        if (!success) {
            WaitIdle(); // rien ne doit plus lire les command buffers, que l'appelant va réutiliser
        }
        m_transfer->Collect();
        return success;
    }

    bool Device::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
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
        const bool recorded = Check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
        const bool success = SubmitAndWait({&commandBuffer, 1}, recorded);
        vkFreeCommandBuffers(m_device.device, m_immediatePool, 1, &commandBuffer);
        return success;
    }

    bool Device::UploadBuffer(VkBuffer target, VkDeviceSize offset, const void* data, VkDeviceSize size, bool inUse) {
        if (!m_transfer->WriteBuffer(target, offset, data, size, inUse)) {
            return false;
        }
        return m_deferUploads || ImmediateSubmit([](VkCommandBuffer) {});
    }

    bool Device::UploadImage(const Transfer::ImageWrite& write, std::span<const std::uint8_t> pixels) {
        if (!m_transfer->WriteImage(write, pixels)) {
            return false;
        }
        return m_deferUploads || ImmediateSubmit([](VkCommandBuffer) {});
    }

    bool Device::UploadImage(const Transfer::ImageWrite& write, VkBuffer source, VkDeviceSize sourceOffset) {
        if (!m_transfer->WriteImage(write, source, sourceOffset)) {
            return false;
        }
        return m_deferUploads || ImmediateSubmit([](VkCommandBuffer) {});
    }
}
