#pragma once

#include "Flux/Export.hpp"

#include <optional>
#include <string>

namespace Flux {
    class Scene;

    // Passage automatique d'une scène à une autre (écran de démarrage, cinématique...) : SceneChangeComponent.
    namespace SceneChange {
        // Fait avancer les minuteries des SceneChangeComponent de la scène. Retourne la scène à charger (chemin
        // relatif au dossier des assets) si l'un d'eux se déclenche : délai écoulé, ou clic / Entrée / Espace
        // (confirmed) s'il l'accepte. Le premier déclenché, dans l'ordre de la hiérarchie, l'emporte.
        FLUX_API std::optional<std::string> Update(Scene& scene, double deltaSeconds, bool confirmed);
    }
}
