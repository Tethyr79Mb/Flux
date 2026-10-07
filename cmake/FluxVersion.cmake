# Version affichée (Flux/Core/Version.hpp) : le numéro de project() dans CMakeLists.txt, plus le commit Git compilé.
set(FLUX_VERSION_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/WriteVersion.cmake)

# flux_add_version(<cible>) : ajoute à la cible un Version.cpp réécrit à chaque compilation, seulement s'il change
# (nouveau commit, fichiers modifiés) : le reste ne se recompile pas.
function(flux_add_version target)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.cpp)
    add_custom_target(${target}Version
            COMMAND ${CMAKE_COMMAND} -DFLUX_VERSION=${PROJECT_VERSION} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${output}
                    -P ${FLUX_VERSION_SCRIPT}
            BYPRODUCTS ${output}
            COMMENT "Version : commit Git"
            VERBATIM)
    target_sources(${target} PRIVATE ${output})
    add_dependencies(${target} ${target}Version)
endfunction()
