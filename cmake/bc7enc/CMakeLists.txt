# bc7enc compilé dans sa propre bibliothèque liée dans FluxEngine.dll, optimisé même en Debug (voir
# cmake/FluxDependencies.cmake). Compilé en C++ : ses fonctions « inline » d'en-tête n'ont pas de définition externe
# en C99 et manqueraient à l'édition des liens sans optimisation. Le décodeur (bc7decomp) sert aux tests seulement.
add_library(bc7enc STATIC ${bc7enc_SOURCE_DIR}/bc7enc.c ${bc7enc_SOURCE_DIR}/bc7decomp.cpp)
set_source_files_properties(${bc7enc_SOURCE_DIR}/bc7enc.c PROPERTIES LANGUAGE CXX)
target_include_directories(bc7enc SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
set_target_properties(bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(bc7enc PRIVATE /W0)
else()
    target_compile_options(bc7enc PRIVATE -w)
endif()
