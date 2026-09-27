#pragma once

#include <Flux/Scene/ComponentRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Choix d'un fichier du projet dans l'explorateur Windows, pour un champ « asset » de l'inspecteur.
    // Un fichier pris hors du dossier des assets est d'abord copié dans son sous-dossier (textures/ pour une image,
    // scenes/ pour une scène), après confirmation. Retourne le chemin relatif au dossier des assets, avec des « / » ; nullopt si annulé.
    std::optional<std::string> ChooseAsset(Flux::Engine& engine, Flux::AssetType type, std::string_view current);
}
