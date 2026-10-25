add_library(FluxEngine SHARED)
# Tous les fichiers de src/ (et les en-têtes publics, pour les voir dans l'IDE) : voir cmake/FluxSources.cmake.
flux_add_sources(FluxEngine src include)
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

# windows.h reste confiné aux .cpp de Platform/Win32 et Renderer/Vulkan.
target_compile_definitions(FluxEngine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# EnTT est public : les modules de jeu manipulent directement la scène.
target_link_libraries(FluxEngine PUBLIC EnTT::EnTT)

# Vulkan est privé au moteur : aucun header public ne l'expose.
target_link_libraries(FluxEngine PRIVATE volk::volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator)
# FreeType et msdfgen servent à générer l'atlas de police ; eux aussi restent privés.
target_link_libraries(FluxEngine PRIVATE freetype msdfgen::msdfgen-core nlohmann_json::nlohmann_json)
# Lecture des images PNG et JPEG (textures de l'interface).
target_link_libraries(FluxEngine PRIVATE stb_image stb_image_impl bc7enc)
# Lecture des modèles 3D glTF.
target_link_libraries(FluxEngine PRIVATE cgltf_header)
# Paquets de triangles des modèles (meshlets).
target_link_libraries(FluxEngine PRIVATE meshoptimizer)
# Physique (Jolt), privée : ses types n'apparaissent dans aucun en-tête public.
target_link_libraries(FluxEngine PRIVATE Jolt)
# Son.
target_link_libraries(FluxEngine PRIVATE miniaudio)
# Boîtes de dialogue d'ouverture et d'enregistrement (GetOpenFileNameW).
target_link_libraries(FluxEngine PRIVATE comdlg32)
# Corbeille, explorateur et icônes des fichiers (SHFileOperationW, ShellExecuteW, SHGetImageList).
target_link_libraries(FluxEngine PRIVATE shell32 ole32)

target_compile_definitions(FluxEngine PRIVATE
        VK_NO_PROTOTYPES
        VK_USE_PLATFORM_WIN32_KHR
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

flux_set_warnings(FluxEngine)
flux_precompile_headers(FluxEngine)
flux_add_version(FluxEngine)

flux_add_shaders(FluxShaders
        shaders/Culling.slang
        shaders/Flame.slang
        shaders/Grid.slang
        shaders/Mesh.slang
        shaders/ModelMesh.slang
        shaders/Overlay.slang
        shaders/PlanetMesh.slang
        shaders/PlanetMeshLarge.slang
        shaders/PlanetMeshSmall.slang
        shaders/PlanetTile.slang
        shaders/PlanetTile64.slang
        shaders/Post.slang
        shaders/Sky.slang
        shaders/Sun.slang
        shaders/Temporal.slang
        shaders/Tracer.slang
        shaders/Vegetation.slang
)
add_dependencies(FluxEngine FluxShaders)

flux_add_assets(FluxAssets assets)
add_dependencies(FluxEngine FluxAssets)
