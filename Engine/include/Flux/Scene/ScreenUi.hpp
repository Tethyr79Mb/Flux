#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Components.hpp"

#include <vector>

namespace Flux {
    class Scene;

    namespace UI {
        class DrawList;
    }

    // Éléments d'interface de la scène (ScreenTextComponent...) : dessinés à plat par-dessus la 3D, dans l'écran du jeu
    // ou dans la vue de l'éditeur.
    namespace ScreenUi {
        // Hauteur d'écran de référence : tailles et décalages sont exprimés pour 1080 lignes, puis mis à l'échelle
        // de la vue (un texte de 64 fait 64 pixels en 1080p, 85 en 1440p, et reste proportionné dans l'éditeur).
        inline constexpr float ReferenceHeight = 1080.0f;

        // Rectangle d'un élément dessiné, pour le sélectionner à la souris.
        struct ItemRect
        {
            Entity entity = NullEntity;
            Math::Vector2f min;
            Math::Vector2f max;
        };

        // Dessine les éléments de la scène dans la vue min-max, dans l'ordre de la hiérarchie (le dernier par-dessus).
        // rects (facultatif) reçoit leurs rectangles, dans le même ordre.
        FLUX_API void Draw(const Scene& scene, UI::DrawList& list, Math::Vector2f viewMin, Math::Vector2f viewMax,
                           std::vector<ItemRect>* rects = nullptr);

        // Point de la vue où se trouve l'ancre (coin, milieu d'un bord ou centre).
        FLUX_API Math::Vector2f AnchorPoint(ScreenAnchor anchor, Math::Vector2f viewMin, Math::Vector2f viewMax);
        // Échelle de la vue : pixels de la vue par pixel de référence.
        FLUX_API float GetScale(Math::Vector2f viewMin, Math::Vector2f viewMax);
    }
}
