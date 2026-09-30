# flux_precompile_headers(<cible>) : en-têtes précompilés (compilés une fois par cible, puis réutilisés par chaque
# .cpp) : la bibliothèque standard la plus incluse et EnTT, qui pèsent dans presque tous nos fichiers.
# Ils sont inclus d'office en tête de chaque .cpp de la cible : un fichier qui oublie un #include compilerait
# quand même ici. On garde donc les #include explicites dans le code ; seul le temps de compilation change.
function(flux_precompile_headers target)
    target_precompile_headers(${target} PRIVATE
            <algorithm>
            <array>
            <cmath>
            <cstdint>
            <expected>
            <filesystem>
            <format>
            <functional>
            <memory>
            <optional>
            <span>
            <string>
            <string_view>
            <unordered_map>
            <utility>
            <vector>
            <entt/entity/registry.hpp>)
endfunction()
