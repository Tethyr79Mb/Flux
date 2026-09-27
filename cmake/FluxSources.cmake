# flux_add_sources(<cible> <dossier>...) : ajoute à la cible tous les .cpp et .hpp de ces dossiers (relatifs au
# CMakeLists appelant) et de leurs sous-dossiers : un nouveau fichier n'a pas à être ajouté à la main.
#
# CONFIGURE_DEPENDS : à chaque compilation, le générateur (Ninja, dans Visual Studio comme en ligne de commande)
# revérifie ces dossiers et relance la configuration si un fichier est apparu ou a disparu. Coût : un parcours
# des dossiers par compilation, négligeable à notre taille. Revers : tout .cpp posé dans ces dossiers est compilé,
# même un brouillon oublié.
function(flux_add_sources target)
    foreach(directory IN LISTS ARGN)
        file(GLOB_RECURSE sources CONFIGURE_DEPENDS
                ${CMAKE_CURRENT_SOURCE_DIR}/${directory}/*.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/${directory}/*.hpp)
        target_sources(${target} PRIVATE ${sources})
    endforeach()
endfunction()
