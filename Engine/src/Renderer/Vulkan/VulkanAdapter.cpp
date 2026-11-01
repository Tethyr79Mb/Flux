#include "Renderer/Vulkan/VulkanAdapter.hpp"

#include "Flux/Platform/Environment.hpp"
#include "Renderer/Vulkan/VulkanInstance.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <vector>

namespace Flux::Vk {
    std::expected<Adapter, std::string> Adapter::Select(const Instance& instance, VkSurfaceKHR surface) {
        Adapter adapter;
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
        // Sémaphores à compteur : ordre entre la file de transfert et la file graphique (VulkanTransfer).
        features12.timelineSemaphore = VK_TRUE;

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

        vkb::PhysicalDeviceSelector selector(instance.Get());
        selector.set_minimum_version(1, 3)
                .set_required_features(features)
                .set_required_features_12(features12)
                .set_required_features_13(features13)
                .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete);
        if (surface == VK_NULL_HANDLE) {
            selector.defer_surface_initialization();
        } else {
            selector.set_surface(surface);
        }
        auto physicalDevice = selector.select();
        if (!physicalDevice) {
            return std::unexpected(std::format("aucun GPU compatible Vulkan 1.3 : {}", physicalDevice.error().message()));
        }
        adapter.m_physicalDevice = physicalDevice.value();
        // Filtrage anisotrope (sol vu de biais net au loin) : présent sur tous les GPU de bureau, facultatif quand même.
        VkPhysicalDeviceFeatures anisotropy{};
        anisotropy.samplerAnisotropy = VK_TRUE;
        adapter.m_anisotropy = adapter.m_physicalDevice.enable_features_if_present(anisotropy);
        // Textures compressées BC (sol des planètes) : tous les GPU de bureau ; sans elles, le sol reste en RGBA8.
        VkPhysicalDeviceFeatures compression{};
        compression.textureCompressionBC = VK_TRUE;
        adapter.m_textureCompressionBC = adapter.m_physicalDevice.enable_features_if_present(compression);
        // FSR (mise à l'échelle) choisit ses shaders selon ce que le GPU sait faire : calculs en demi-précision et
        // taille des groupes de threads imposée (wave64 des Radeon). Activés quand ils y sont (tous les GPU de bureau
        // récents) ; sinon FSR prend ses variantes simples.
        VkPhysicalDeviceVulkan12Features half{};
        half.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        half.shaderFloat16 = VK_TRUE;
        adapter.m_physicalDevice.enable_extension_features_if_present(half);
        VkPhysicalDeviceVulkan13Features subgroups{};
        subgroups.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        subgroups.subgroupSizeControl = VK_TRUE;
        subgroups.computeFullSubgroups = VK_TRUE;
        adapter.m_physicalDevice.enable_extension_features_if_present(subgroups);
        // Mesh shaders : le sol des planètes en paquets de triangles que le GPU écarte lui-même (shaders/PlanetMesh.slang).
        // RTX 20, RX 6000, Arc et plus récentes ; sans eux (GTX 1060, RX 5700), le chemin des sommets.
        // FLUX_MESH_SHADERS=0 les ignore : diagnostic d'un pilote fautif, et comparaison des deux chemins.
        const std::optional<std::string> meshSetting = Platform::ReadEnvironment("FLUX_MESH_SHADERS");
        if (adapter.m_physicalDevice.is_extension_present(VK_EXT_MESH_SHADER_EXTENSION_NAME) && meshSetting != "0") {
            VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
            mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
            mesh.taskShader = VK_TRUE;
            mesh.meshShader = VK_TRUE;
            adapter.m_meshShaders = adapter.m_physicalDevice.enable_extension_features_if_present(mesh)
                                    && adapter.m_physicalDevice.enable_extension_if_present(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        }
        if (adapter.m_meshShaders) {
            // Taille de groupe que le pilote préfère (NVIDIA plus petite qu'AMD) : choisit la variante des paquets du sol.
            VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties{};
            meshProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &meshProperties;
            vkGetPhysicalDeviceProperties2(adapter.m_physicalDevice.physical_device, &properties);
            adapter.m_meshPreferredThreads = meshProperties.maxPreferredMeshWorkGroupInvocations;
        }
        // Diagnostic d'un GPU bloqué, facultatif : repères écrits par le GPU (AMD) et description de la panne.
        adapter.m_bufferMarkers = adapter.m_physicalDevice.enable_extension_if_present(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
        if (adapter.m_physicalDevice.is_extension_present(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
            VkPhysicalDeviceFaultFeaturesEXT fault{};
            fault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            fault.deviceFault = VK_TRUE;
            adapter.m_deviceFault = adapter.m_physicalDevice.enable_extension_features_if_present(fault)
                                    && adapter.m_physicalDevice.enable_extension_if_present(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        }

        // Le device ne dépasse jamais la version demandée à l'instance, même si le pilote en accepte une plus récente.
        const uint32_t supported = adapter.m_physicalDevice.properties.apiVersion;
        const uint32_t used = std::min(instance.GetApiVersion(), supported);
        adapter.m_apiVersion = std::format("{}.{} (GPU : {}.{}.{})", VK_API_VERSION_MAJOR(used), VK_API_VERSION_MINOR(used),
                                           VK_API_VERSION_MAJOR(supported), VK_API_VERSION_MINOR(supported),
                                           VK_API_VERSION_PATCH(supported));
        return adapter;
    }

    float Adapter::GetTimestampPeriod(std::uint32_t family) const {
        const std::vector<VkQueueFamilyProperties> families = m_physicalDevice.get_queue_families();
        if (family >= families.size() || families[family].timestampValidBits == 0) {
            return 0.0f;
        }
        return m_physicalDevice.properties.limits.timestampPeriod;
    }
}
