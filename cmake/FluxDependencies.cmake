# Dépendances tierces, figées sur des versions précises et compilées depuis les sources
# (les binaires précompilés du SDK Vulkan visent MSVC).
# Les préréglages (CMakePresets.json) les rangent dans out/deps/<préréglage>, hors du dossier de compilation :
# « Supprimer le cache et reconfigurer » ne les fait pas retélécharger. Un dossier par préréglage : Debug et Release
# ne peuvent pas partager les dossiers de compilation des dépendances que FetchContent y met aussi.
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
# Runtime C++ en DLL (/MD), comme le reste du projet : sinon MSVC refuse de lier (discordance RuntimeLibrary).
set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
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

# Lecture des images (PNG, JPEG...) : stb_image, un seul header. Pas de version publiée : commit figé.
# Seulement téléchargé (FetchContent_Populate implicite) : stb n'a pas de CMakeLists.
FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
        SYSTEM)

# Lecture des modèles glTF 2.0 (.gltf, .glb) : cgltf, un seul header C, sans dépendance.
FetchContent_Declare(cgltf
        GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
        GIT_TAG v1.15
        GIT_SHALLOW TRUE
        SYSTEM)

# Physique : Jolt Physics (licence MIT). Le CMakeLists est dans Build/. Bibliothèque statique, privée au moteur.
FetchContent_Declare(JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.6.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build
        SYSTEM)
# Seulement la physique : ni rendu de debug, ni profileur, ni calcul GPU (Vulkan/DX12 à lui), ni installation.
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
# Nos options de compilation (et le runtime DLL /MD de MSVC, comme msdfgen), pas les siennes ; pas de LTO
# (mélangé à MinGW, source d'ennuis) ; ses avertissements ne sont pas des erreurs chez nous.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
# Ses symboles de debug en Release gonfleraient FluxEngine.dll pour rien.
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)

# Son : miniaudio (domaine public ou MIT-0), un seul fichier C. Son CMakeLists (exemples, décodeurs Opus/Vorbis
# externes) est ignoré : SOURCE_SUBDIR désigne un dossier qui n'existe pas, le dépôt est seulement téléchargé.
FetchContent_Declare(miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG 0.11.25
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR flux-telechargement-seul
        SYSTEM)

# Vulkan-Headers d'abord : vk-bootstrap réutilise sa cible Vulkan::Headers au lieu de chercher le SDK.
FetchContent_MakeAvailable(VulkanHeaders)
FetchContent_MakeAvailable(volk vk_bootstrap VulkanMemoryAllocator freetype msdfgen EnTT nlohmann_json stb cgltf miniaudio)

# Jolt optimisé même en Debug : sans optimisation, la physique (coque du vaisseau contre le sol, personnages) prend
# plusieurs fois le temps d'une image et le Debug devient inutilisable. On ne débogue jamais l'intérieur de Jolt.
# Le runtime reste celui du Debug (/MDd, _DEBUG) : mêmes conteneurs de la bibliothèque standard que notre code.
# add_subdirectory copie les variables au moment de l'appel : les options ne valent que pour Jolt.
set(FLUX_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
if (MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "/O2 /Ob2")
else()
    set(CMAKE_CXX_FLAGS_DEBUG "-O2")
endif()
FetchContent_MakeAvailable(JoltPhysics)
set(CMAKE_CXX_FLAGS_DEBUG "${FLUX_CXX_FLAGS_DEBUG}")

add_library(stb_image INTERFACE)
target_include_directories(stb_image SYSTEM INTERFACE ${stb_SOURCE_DIR})

# Le header seul : l'implémentation (CGLTF_IMPLEMENTATION) est compilée dans Model.cpp.
add_library(cgltf_header INTERFACE)
target_include_directories(cgltf_header SYSTEM INTERFACE ${cgltf_SOURCE_DIR})

# miniaudio compilé en C dans sa propre bibliothèque (Engine/src/Audio/Miniaudio.c : implémentation, plus
# stb_vorbis pour les .ogg), liée dans FluxEngine.dll. Les mêmes options pour l'implémentation et pour AudioSystem.cpp :
# elles changent la taille de certaines structures. Sorties Windows seulement (WASAPI, DirectSound en secours), plus
# la sortie « nulle » quand aucune carte son n'est là (machine de CI, Wine) : le jeu tourne quand même, muet.
add_library(miniaudio STATIC ${CMAKE_SOURCE_DIR}/Engine/src/Audio/Miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_compile_definitions(miniaudio PUBLIC
        MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI MA_ENABLE_DSOUND MA_ENABLE_NULL MA_NO_ENCODING)
set_target_properties(miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
else()
    target_compile_options(miniaudio PRIVATE -w)
endif()

# Jolt en « unity build » : ses ~150 .cpp regroupés par paquets de 16 en une dizaine de gros fichiers ; les en-têtes
# communs ne sont lus qu'une fois par paquet. Il n'est recompilé que si le dossier de compilation est vidé.
set_target_properties(Jolt PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 16)

target_link_libraries(volk PUBLIC Vulkan::Headers)
# Bibliothèques statiques liées dans FluxEngine.dll.
set_target_properties(volk vk-bootstrap PROPERTIES POSITION_INDEPENDENT_CODE ON)
