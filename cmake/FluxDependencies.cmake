# Dépendances tierces, figées sur des versions précises et compilées depuis les sources
# (les binaires précompilés du SDK Vulkan visent MSVC).
include(FetchContent)

# Headers Vulkan figés : la compilation ne dépend pas du SDK installé.
# Le SDK reste utile à l'exécution pour les couches de validation.
FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.363
        GIT_SHALLOW TRUE
        SYSTEM)

# Chargeur de fonctions Vulkan : pas de lien avec vulkan-1.lib.
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(VOLK_STATIC_DEFINES "VK_USE_PLATFORM_WIN32_KHR" CACHE STRING "" FORCE)
FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG vulkan-sdk-1.4.357.0
        GIT_SHALLOW TRUE
        SYSTEM)

# Création d'instance, device et swapchain.
set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vk_bootstrap
        GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
        GIT_TAG v1.4.363
        GIT_SHALLOW TRUE
        SYSTEM)

# Allocation mémoire GPU.
set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(VulkanMemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.3.0
        GIT_SHALLOW TRUE
        SYSTEM)

# Vulkan-Headers d'abord : vk-bootstrap réutilise sa cible Vulkan::Headers au lieu de chercher le SDK.
FetchContent_MakeAvailable(VulkanHeaders)
FetchContent_MakeAvailable(volk vk_bootstrap VulkanMemoryAllocator)

target_link_libraries(volk PUBLIC Vulkan::Headers)
# Bibliothèques statiques liées dans FluxEngine.dll.
set_target_properties(volk vk-bootstrap PROPERTIES POSITION_INDEPENDENT_CODE ON)
