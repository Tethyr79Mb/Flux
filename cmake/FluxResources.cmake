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
function(flux_add_assets target sourceDirectory)
    get_filename_component(sourcePath ${sourceDirectory} ABSOLUTE)
    file(GLOB_RECURSE files CONFIGURE_DEPENDS ${sourcePath}/*)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/${target}.stamp)
    add_custom_command(
            OUTPUT ${stamp}
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${sourcePath} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets
            COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
            DEPENDS ${files}
            COMMENT "Copie des assets de ${target}"
            VERBATIM)
    add_custom_target(${target} DEPENDS ${stamp})
endfunction()
