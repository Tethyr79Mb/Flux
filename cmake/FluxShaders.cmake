# Compilation des shaders Slang en SPIR-V, à côté des exe dans bin/shaders/.
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
