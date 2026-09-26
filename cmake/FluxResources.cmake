# Ressources copiées à côté des exe : shaders Slang compilés en SPIR-V (bin/shaders/) et assets (bin/assets/).
# slangc est fourni par le SDK Vulkan (dossier Bin).
find_program(FLUX_SLANGC
        NAMES slangc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
        REQUIRED)

# flux_add_shaders(<cible> <fichiers .slang>...) : crée la cible qui compile chaque shader en <nom>.spv.
function(flux_add_shaders target)
    set(outputDirectory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
    set(outputs)
    foreach(source IN LISTS ARGN)
        get_filename_component(name ${source} NAME_WE)
        get_filename_component(sourcePath ${source} ABSOLUTE)
        set(output ${outputDirectory}/${name}.spv)
        add_custom_command(
                OUTPUT ${output}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${outputDirectory}
                COMMAND ${FLUX_SLANGC} ${sourcePath}
                        -target spirv
                        -fvk-use-entrypoint-name
                        "$<$<CONFIG:Debug>:-g>"
                        -o ${output}
                DEPENDS ${sourcePath}
                COMMENT "Slang : ${name}.slang"
                VERBATIM
                COMMAND_EXPAND_LISTS) # retire l'argument vide hors Debug
        list(APPEND outputs ${output})
    endforeach()
    add_custom_target(${target} DEPENDS ${outputs} SOURCES ${ARGN})
endfunction()

# flux_add_assets(<cible> <dossier source>) : copie le dossier dans bin/assets/ à chaque build si un fichier a changé.
# Chaque sous-dossier (scenes/, fonts/...) est vidé avant d'être recopié : un fichier supprimé des sources disparaît
# aussi de bin/. Le moteur et le jeu partagent bin/assets/ : leurs sous-dossiers ne doivent pas porter le même nom.
function(flux_add_assets target sourceDirectory)
    get_filename_component(sourcePath ${sourceDirectory} ABSOLUTE)
    set(outputPath ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets)
    file(GLOB_RECURSE files CONFIGURE_DEPENDS ${sourcePath}/*)

    # Liste des fichiers écrite seulement si elle change : un fichier supprimé relance la copie
    # (Ninja ne rejoue pas une commande dont seules les dépendances ont diminué).
    set(fileList ${CMAKE_CURRENT_BINARY_DIR}/${target}.files)
    string(REPLACE ";" "\n" fileListContent "${files}")
    set(previousContent "")
    if(EXISTS ${fileList})
        file(READ ${fileList} previousContent)
    endif()
    if(NOT previousContent STREQUAL fileListContent)
        file(WRITE ${fileList} "${fileListContent}")
    endif()

    set(clearCommands)
    file(GLOB children LIST_DIRECTORIES true ${sourcePath}/*)
    foreach(child IN LISTS children)
        if(IS_DIRECTORY ${child})
            get_filename_component(name ${child} NAME)
            list(APPEND clearCommands COMMAND ${CMAKE_COMMAND} -E rm -rf ${outputPath}/${name})
        endif()
    endforeach()

    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/${target}.stamp)
    add_custom_command(
            OUTPUT ${stamp}
            ${clearCommands}
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${sourcePath} ${outputPath}
            COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
            DEPENDS ${files} ${fileList}
            COMMENT "Copie des assets de ${target}"
            VERBATIM)
    add_custom_target(${target} DEPENDS ${stamp})
endfunction()
