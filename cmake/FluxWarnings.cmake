function(flux_set_warnings target)
    if(MSVC)
        # C4251 : membre de la bibliothèque standard dans une classe exportée (FLUX_API). Sans effet ici :
        # moteur, jeu et éditeur sont toujours compilés ensemble, avec le même compilateur.
        target_compile_options(${target} PRIVATE /W4 /permissive- /we4715 /wd4251)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror=return-type)
    endif()
    # Format objet étendu de Windows : un fichier comme ComponentRegistry.cpp (une fonction générée par champ de
    # composant) dépasse le nombre de sections d'un .obj ordinaire (« file too big », erreur C1128).
    if(MSVC)
        target_compile_options(${target} PRIVATE /bigobj)
    elseif(WIN32)
        target_compile_options(${target} PRIVATE -Wa,-mbig-obj)
    endif()
endfunction()
