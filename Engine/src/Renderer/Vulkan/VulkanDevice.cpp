#include "Renderer/Vulkan/VulkanDevice.hpp"

#include "Flux/Core/Log.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <string_view>
#include <vector>

namespace Flux::Vk {
    std::expected<std::unique_ptr<Device>, std::string> Device::Create(void* nativeWindowHandle) {
        if (volkInitialize() != VK_SUCCESS) {
            return std::unexpected("vulkan-1.dll introuvable : installe ou mets à jour le pilote graphique");
        }

        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<Device> device(new Device());

        // Sans fenêtre (calculs, tests) : pas de surface ni de file de présentation, ni les extensions de fenêtre de
        // l'instance (lavapipe sous Windows, le GPU logiciel de la CI, n'a pas VK_KHR_win32_surface).
        const bool headless = nativeWindowHandle == nullptr;
        vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);
        instanceBuilder.set_app_name("Flux")
                       .set_engine_name("FluxEngine")
                       .set_headless(headless)
                       .require_api_version(1, 3, 0);
#ifndef NDEBUG
        // Actives seulement si le SDK Vulkan est installé ; sinon on continue sans.
        instanceBuilder.request_validation_layers(true)
                       .use_default_debug_messenger();
#else
        // Noms des étapes (BeginStep) visibles dans Radeon GPU Profiler, Nsight ou RenderDoc, même en Release.
        if (const auto system = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
            system && system->is_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            device->m_debugLabels = true;
        }
#endif
        auto instance = instanceBuilder.build();
        if (!instance) {
            return std::unexpected(std::format("création de l'instance Vulkan : {}", instance.error().message()));
        }
        device->m_instance = instance.value();
#ifndef NDEBUG
        device->m_debugLabels = device->m_instance.debug_messenger != VK_NULL_HANDLE; // le messager exige l'extension
#endif
        volkLoadInstance(device->m_instance.instance);

        if (!headless) {
            VkWin32SurfaceCreateInfoKHR surfaceInfo{};
            surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            surfaceInfo.hinstance = GetModuleHandleW(nullptr);
            surfaceInfo.hwnd = static_cast<HWND>(nativeWindowHandle);
            if (!Check(vkCreateWin32SurfaceKHR(device->m_instance.instance, &surfaceInfo, nullptr, &device->m_surface),
                       "vkCreateWin32SurfaceKHR")) {
                return std::unexpected("création de la surface Vulkan");
            }
        }

        // Tables de textures (RHI::BindingType::TextureTable) : toutes les images des matériaux dans un seul groupe,
        // lues par indice ; et nombre de dessins d'un appel indirect lu dans un buffer (élimination sur le GPU).
        // Vulkan 1.2, présents sur tous les GPU de bureau de Vulkan 1.3.
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.descriptorIndexing = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.drawIndirectCount = VK_TRUE;

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        // Dessin indirect (toutes les tuiles d'une planète en un appel) : plusieurs commandes par appel, chacune avec
        // son indice d'instance. Présents sur tous les GPU de bureau.
        VkPhysicalDeviceFeatures features{};
        features.multiDrawIndirect = VK_TRUE;
        features.drawIndirectFirstInstance = VK_TRUE;
        // Cible des vecteurs de mouvement (TAA) jamais mélangée, à côté d'une couleur mélangée (transparents,
        // réacteurs) : deux états de mélange différents dans un même pipeline. Présent sur tous les GPU de bureau.
        features.independentBlend = VK_TRUE;

        vkb::PhysicalDeviceSelector selector(device->m_instance);
        selector.set_minimum_version(1, 3)
                .set_required_features(features)
                .set_required_features_12(features12)
                .set_required_features_13(features13)
                .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete);
        if (headless) {
            selector.defer_surface_initialization();
        } else {
            selector.set_surface(device->m_surface);
        }
        auto physicalDevice = selector.select();
        if (!physicalDevice) {
            return std::unexpected(std::format("aucun GPU compatible Vulkan 1.3 : {}", physicalDevice.error().message()));
        }
        device->m_physicalDevice = physicalDevice.value();
        // Filtrage anisotrope (sol vu de biais net au loin) : présent sur tous les GPU de bureau, facultatif quand même.
        VkPhysicalDeviceFeatures anisotropy{};
        anisotropy.samplerAnisotropy = VK_TRUE;
        device->m_anisotropy = device->m_physicalDevice.enable_features_if_present(anisotropy);
        // Textures compressées BC (sol des planètes) : tous les GPU de bureau ; sans elles, le sol reste en RGBA8.
        VkPhysicalDeviceFeatures compression{};
        compression.textureCompressionBC = VK_TRUE;
        device->m_textureCompressionBC = device->m_physicalDevice.enable_features_if_present(compression);
        // Doubles dans les shaders : la déformation du relief des grandes planètes (PlanetTile64.slang). GTX 1060 et
        // RX 5700 les ont ; sans eux, les tuiles restent en float.
        VkPhysicalDeviceFeatures float64{};
        float64.shaderFloat64 = VK_TRUE;
        device->m_float64 = device->m_physicalDevice.enable_features_if_present(float64);
        // Mesh shaders : le sol des planètes en paquets de triangles que le GPU écarte lui-même (shaders/PlanetMesh.slang).
        // RTX 20, RX 6000, Arc et plus récentes ; sans eux (GTX 1060, RX 5700), le chemin des sommets.
        // FLUX_MESH_SHADERS=0 les ignore : diagnostic d'un pilote fautif, et comparaison des deux chemins.
#ifdef _MSC_VER
#pragma warning(suppress : 4996) // getenv : lu avant tout fil
#endif
        const char* meshSetting = std::getenv("FLUX_MESH_SHADERS");
        if (device->m_physicalDevice.is_extension_present(VK_EXT_MESH_SHADER_EXTENSION_NAME)
            && !(meshSetting && std::string_view(meshSetting) == "0")) {
            VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
            mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
            mesh.taskShader = VK_TRUE;
            mesh.meshShader = VK_TRUE;
            device->m_meshShaders = device->m_physicalDevice.enable_extension_features_if_present(mesh)
                                    && device->m_physicalDevice.enable_extension_if_present(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        }
        if (device->m_meshShaders) {
            // Taille de groupe que le pilote préfère (NVIDIA plus petite qu'AMD) : choisit la variante des paquets du sol.
            VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties{};
            meshProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &meshProperties;
            vkGetPhysicalDeviceProperties2(device->m_physicalDevice.physical_device, &properties);
            device->m_meshPreferredThreads = meshProperties.maxPreferredMeshWorkGroupInvocations;
        }
        // Diagnostic d'un GPU bloqué, facultatif : repères écrits par le GPU (AMD) et description de la panne.
        device->m_bufferMarkers = device->m_physicalDevice.enable_extension_if_present(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
        if (device->m_physicalDevice.is_extension_present(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
            VkPhysicalDeviceFaultFeaturesEXT fault{};
            fault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            fault.deviceFault = VK_TRUE;
            device->m_deviceFault = device->m_physicalDevice.enable_extension_features_if_present(fault)
                                    && device->m_physicalDevice.enable_extension_if_present(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        }

        vkb::DeviceBuilder deviceBuilder(device->m_physicalDevice);
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

        // Le device ne dépasse jamais la version demandée à l'instance, même si le pilote en accepte une plus récente.
        const uint32_t supported = device->m_physicalDevice.properties.apiVersion;
        const uint32_t used = std::min(device->m_instance.api_version, supported);
        device->m_apiVersion = std::format("{}.{} (GPU : {}.{}.{})", VK_API_VERSION_MAJOR(used), VK_API_VERSION_MINOR(used),
                                           VK_API_VERSION_MAJOR(supported), VK_API_VERSION_MINOR(supported),
                                           VK_API_VERSION_PATCH(supported));
        Log::Info("Vulkan {} : {}", device->m_apiVersion, device->m_physicalDevice.name);
        return device;
    }

    Device::~Device() {
        // Ordre inverse de la création ; chaque étape tolère un objet jamais créé.
        if (m_device.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device.device);
        }
        if (m_immediatePool != VK_NULL_HANDLE) {
            if (m_uploads.commands != VK_NULL_HANDLE) {
                vkEndCommandBuffer(m_uploads.commands);
            }
            ReleaseUploads(m_uploads); // avant l'allocateur : ses buffers y sont pris
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

    float Device::GetTimestampPeriod() const {
        const std::vector<VkQueueFamilyProperties> families = m_physicalDevice.get_queue_families();
        if (m_graphicsQueueFamily >= families.size() || families[m_graphicsQueueFamily].timestampValidBits == 0) {
            return 0.0f;
        }
        return m_physicalDevice.properties.limits.timestampPeriod;
    }

    std::string Device::DescribeFault() const {
        if (!m_deviceFault || !vkGetDeviceFaultInfoEXT) {
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

    namespace {
        // Barrière globale : tout ce qui précède (étapes `from`) avant tout ce qui suit (étapes `to`).
        void GlobalBarrier(VkCommandBuffer commands, VkPipelineStageFlags2 from, VkAccessFlags2 fromAccess,
                           VkPipelineStageFlags2 to, VkAccessFlags2 toAccess) {
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = from;
            barrier.srcAccessMask = fromAccess;
            barrier.dstStageMask = to;
            barrier.dstAccessMask = toAccess;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commands, &dependency);
        }
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
        bool success = Check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

        UploadBatch uploads = TakeUploads();
        if (success) {
            // Les envois en attente d'abord : ces commandes les lisent peut-être.
            std::array<VkCommandBufferSubmitInfo, 2> commandInfos{};
            std::uint32_t count = 0;
            for (VkCommandBuffer commands : {uploads.commands, commandBuffer}) {
                if (commands != VK_NULL_HANDLE) {
                    commandInfos[count].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
                    commandInfos[count].commandBuffer = commands;
                    ++count;
                }
            }
            VkSubmitInfo2 submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submitInfo.commandBufferInfoCount = count;
            submitInfo.pCommandBufferInfos = commandInfos.data();
            success = Check(vkQueueSubmit2(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2")
                   && Check(vkQueueWaitIdle(m_graphicsQueue), "vkQueueWaitIdle");
        }
        if (!success) {
            WaitIdle(); // rien ne doit plus lire les buffers rendus ci-dessous
        }
        ReleaseUploads(uploads);
        vkFreeCommandBuffers(m_device.device, m_immediatePool, 1, &commandBuffer);
        return success;
    }

    bool Device::Upload(const std::function<void(VkCommandBuffer)>& record, Buffer staging) {
        if (m_uploads.commands == VK_NULL_HANDLE) {
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = m_immediatePool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(m_device.device, &allocateInfo, &m_uploads.commands), "vkAllocateCommandBuffers")) {
                m_uploads.commands = VK_NULL_HANDLE;
                return false;
            }
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(m_uploads.commands, &beginInfo);
        }
        // Une image soumise avant (ou un envoi précédent) lit ou écrit peut-être ce qui va être réécrit : table des
        // matériaux, page de géométrie.
        GlobalBarrier(m_uploads.commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        record(m_uploads.commands);
        m_uploads.staging.push_back(std::move(staging));
        if (!m_deferUploads) {
            return ImmediateSubmit([](VkCommandBuffer) {});
        }
        return true;
    }

    Device::UploadBatch Device::TakeUploads() {
        UploadBatch batch = std::move(m_uploads);
        m_uploads = {};
        if (batch.commands != VK_NULL_HANDLE) {
            GlobalBarrier(batch.commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            if (!Check(vkEndCommandBuffer(batch.commands), "vkEndCommandBuffer")) {
                ReleaseUploads(batch); // jamais soumis : ses envois sont perdus (message dans la console)
            }
        }
        return batch;
    }

    void Device::ReleaseUploads(UploadBatch& batch) {
        if (batch.commands != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_device.device, m_immediatePool, 1, &batch.commands);
        }
        batch = {};
    }
}
