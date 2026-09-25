add_library(FluxEngine SHARED
        src/Core/Engine.cpp
        src/Core/GameModule.cpp
        src/Core/Uuid.cpp
        src/Platform/Input.cpp
        src/Renderer/Renderer.cpp
        src/Scene/Camera.cpp
        src/Scene/FlyCameraController.cpp
        src/Scene/Scene.cpp
        src/Scene/SceneSerializer.cpp
        src/UI/Font.cpp
        src/UI/DrawList.cpp
        src/RHI/Vulkan/VulkanDevice.cpp
        src/RHI/Vulkan/VulkanSwapchain.cpp
        src/RHI/Vulkan/VulkanBuffer.cpp
        src/RHI/Vulkan/VulkanPipeline.cpp
        src/RHI/Vulkan/VulkanImage.cpp
        src/RHI/Vulkan/VmaImplementation.cpp
        src/Platform/Win32/Win32Window.cpp
        src/Platform/Win32/Win32SharedLibrary.cpp
        src/Platform/Win32/Win32Filesystem.cpp
)
add_library(Flux::Engine ALIAS FluxEngine)

# FluxEngine.dll plutôt que libFluxEngine.dll avec MinGW.
set_target_properties(FluxEngine PROPERTIES PREFIX "")

include(GenerateExportHeader)
generate_export_header(FluxEngine
        BASE_NAME FLUX
        EXPORT_MACRO_NAME FLUX_API
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/Flux/Export.hpp)

target_include_directories(FluxEngine
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

# windows.h reste confiné aux .cpp de Platform/Win32 et RHI/Vulkan.
target_compile_definitions(FluxEngine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# EnTT est public : les modules de jeu manipulent directement la scène.
target_link_libraries(FluxEngine PUBLIC EnTT::EnTT)

# Vulkan est privé au moteur : aucun header public ne l'expose.
target_link_libraries(FluxEngine PRIVATE volk::volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator)
# FreeType et msdfgen servent à générer l'atlas de police ; eux aussi restent privés.
target_link_libraries(FluxEngine PRIVATE freetype msdfgen::msdfgen-core nlohmann_json::nlohmann_json)

target_compile_definitions(FluxEngine PRIVATE
        VK_NO_PROTOTYPES
        VK_USE_PLATFORM_WIN32_KHR
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

flux_set_warnings(FluxEngine)

flux_add_shaders(FluxShaders
        shaders/Mesh.slang
        shaders/Overlay.slang
)
add_dependencies(FluxEngine FluxShaders)

flux_add_assets(FluxAssets assets)
add_dependencies(FluxEngine FluxAssets)
