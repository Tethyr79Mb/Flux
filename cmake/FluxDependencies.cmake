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

# Compression des textures en BC7 (sol des planètes) : bc7enc de Rich Geldreich (MIT ou domaine public), un fichier.
# Pas de version publiée : commit figé. Son CMakeLists (programme d'essai) est ignoré, comme celui de miniaudio.
FetchContent_Declare(bc7enc
        GIT_REPOSITORY https://github.com/richgel999/bc7enc.git
        GIT_TAG f66c2e489b07138f2673a2fb3d27c1aa1d565c48
        SOURCE_SUBDIR flux-telechargement-seul
        SYSTEM)

# Découpe des modèles en paquets de triangles (meshlets) pour le rendu piloté par le GPU, puis simplification :
# meshoptimizer d'Arseny Kapoulkine (MIT). Seulement la bibliothèque, ni démo ni gltfpack.
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG v1.3
        GIT_SHALLOW TRUE
        SYSTEM)

# Vulkan-Headers d'abord : vk-bootstrap réutilise sa cible Vulkan::Headers au lieu de chercher le SDK.
FetchContent_MakeAvailable(VulkanHeaders)
FetchContent_MakeAvailable(volk vk_bootstrap VulkanMemoryAllocator EnTT nlohmann_json stb cgltf miniaudio bc7enc)

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

# Ce qui tourne au démarrage (atlas de la police : FreeType et msdfgen ; décodage des images : stb_image ; compression
# des textures du sol : bc7enc ; paquets de triangles des modèles : meshoptimizer) est compilé
# optimisé même en Debug : sinon une vingtaine de secondes de plus à chaque lancement. On ne les suit pas pas à pas.
# Les options d'un dossier valent pour ses cibles : changées le temps d'ajouter ces dossiers, puis remises.
set(FLUX_SAVED_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
set(FLUX_SAVED_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG "/O2 /Ob2")
    set(CMAKE_CXX_FLAGS_DEBUG "/O2 /Ob2")
else()
    set(CMAKE_C_FLAGS_DEBUG "-O2")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2")
endif()
FetchContent_MakeAvailable(freetype msdfgen meshoptimizer)
add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/stb ${CMAKE_BINARY_DIR}/stb_image_impl)
add_subdirectory(${CMAKE_SOURCE_DIR}/cmake/bc7enc ${CMAKE_BINARY_DIR}/bc7enc_impl)
set(CMAKE_C_FLAGS_DEBUG "${FLUX_SAVED_C_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_DEBUG "${FLUX_SAVED_CXX_FLAGS_DEBUG}")

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

# Mise à l'échelle FSR 3.1 d'AMD (FidelityFX SDK 1.1.4, licence MIT) : la DLL Vulkan signée qu'AMD publie déjà compilée,
# et les quatre en-têtes C de son API. Compiler le SDK nous-mêmes demanderait son compilateur de shaders, des Go de
# sources et un long temps de compilation, pour la même DLL. Téléchargés une fois dans le dossier des dépendances
# (empreintes vérifiées : un fichier déjà là et intact n'est pas retéléchargé) ; la DLL est copiée à côté des exe et
# chargée au démarrage (Renderer/Vulkan/VulkanUpscaler.cpp). Sans elle, le jeu tourne en Natif.
set(FLUX_FIDELITYFX_DIR ${FETCHCONTENT_BASE_DIR}/fidelityfx-1.1.4)
set(FLUX_FIDELITYFX_URL https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4)
set(FLUX_FIDELITYFX_FILES
        "PrebuiltSignedDLL/amd_fidelityfx_vk.dll|amd_fidelityfx_vk.dll|a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78"
        "LICENSE.txt|LICENSE.txt|82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a"
        "ffx-api/include/ffx_api/ffx_api.h|include/ffx_api/ffx_api.h|408b4203545e8299ed188fbb1d9ddfe0932b73ea6e8b14fe10edcf1f45771561"
        "ffx-api/include/ffx_api/ffx_api_types.h|include/ffx_api/ffx_api_types.h|dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe"
        "ffx-api/include/ffx_api/ffx_upscale.h|include/ffx_api/ffx_upscale.h|1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1"
        "ffx-api/include/ffx_api/vk/ffx_api_vk.h|include/ffx_api/vk/ffx_api_vk.h|a50a8d5ff5689a10a6ce378f2df5569fd880c89ec46e8356aaf35eb40512edd9")
foreach(entry IN LISTS FLUX_FIDELITYFX_FILES)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 source)
    list(GET entry 1 destination)
    list(GET entry 2 hash)
    file(DOWNLOAD ${FLUX_FIDELITYFX_URL}/${source} ${FLUX_FIDELITYFX_DIR}/${destination}
            EXPECTED_HASH SHA256=${hash}
            TLS_VERIFY ON)
endforeach()
file(COPY ${FLUX_FIDELITYFX_DIR}/amd_fidelityfx_vk.dll DESTINATION ${FLUX_OUTPUT_DIRECTORY})
add_library(fidelityfx_api INTERFACE)
target_include_directories(fidelityfx_api SYSTEM INTERFACE ${FLUX_FIDELITYFX_DIR}/include)
