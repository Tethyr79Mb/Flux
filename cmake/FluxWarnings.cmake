function(flux_set_warnings target)
    if(MSVC)
        # C4251 : membre de la bibliothèque standard dans une classe exportée (FLUX_API). Sans effet ici :
        # moteur, jeu et éditeur sont toujours compilés ensemble, avec le même compilateur.
        target_compile_options(${target} PRIVATE /W4 /permissive- /we4715 /wd4251)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror=return-type)
    endif()
endfunction()
