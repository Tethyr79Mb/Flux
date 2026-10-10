# Implémentation de stb_image (Engine/src/Assets/StbImage.c), compilée en C dans sa propre bibliothèque liée dans
# FluxEngine.dll. Dossier à part : les options de Debug choisies par cmake/FluxDependencies.cmake (optimisées) ne
# valent que pour lui.
add_library(stb_image_impl STATIC ${CMAKE_SOURCE_DIR}/Engine/src/Assets/StbImage.c)
target_link_libraries(stb_image_impl PUBLIC stb_image)
set_target_properties(stb_image_impl PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(stb_image_impl PRIVATE /W0)
else()
    target_compile_options(stb_image_impl PRIVATE -w)
endif()
