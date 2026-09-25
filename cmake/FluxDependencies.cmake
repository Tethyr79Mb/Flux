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

# Lecture des polices (contours des glyphes). Aucune dépendance optionnelle.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-14-3
        GIT_SHALLOW TRUE
        SYSTEM)

# Génération des champs de distance multicanaux (MSDF) : seulement le cœur, sans dépendance.
set(MSDFGEN_CORE_ONLY ON CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(msdfgen
        GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
        GIT_TAG v1.13
        GIT_SHALLOW TRUE
        SYSTEM)

# ECS (entités et composants) de la scène, en headers seulement. Exposé aux modules de jeu.
FetchContent_Declare(EnTT
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG v3.16.0
        GIT_SHALLOW TRUE
        SYSTEM)

# Lecture et écriture JSON (fichiers de scène). Archive officielle : bien plus légère que le dépôt complet.
FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)

# Vulkan-Headers d'abord : vk-bootstrap réutilise sa cible Vulkan::Headers au lieu de chercher le SDK.
FetchContent_MakeAvailable(VulkanHeaders)
FetchContent_MakeAvailable(volk vk_bootstrap VulkanMemoryAllocator freetype msdfgen EnTT nlohmann_json)

target_link_libraries(volk PUBLIC Vulkan::Headers)
# Bibliothèques statiques liées dans FluxEngine.dll.
set_target_properties(volk vk-bootstrap PROPERTIES POSITION_INDEPENDENT_CODE ON)
