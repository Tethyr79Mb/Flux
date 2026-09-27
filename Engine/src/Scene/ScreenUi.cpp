#include "Flux/Scene/ScreenUi.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>

namespace Flux::ScreenUi {
    using Math::Vector2f;

    namespace {
        // Position de l'ancre en fractions de la vue : 0 à gauche (en haut), 0,5 au milieu, 1 à droite (en bas).
        Vector2f AnchorFraction(ScreenAnchor anchor) {
            const auto index = static_cast<int>(anchor);
            return {static_cast<float>(index % 3) * 0.5f, static_cast<float>(index / 3) * 0.5f};
        }

        void DrawEntity(const Scene& scene, Entity entity, UI::DrawList& list, Vector2f viewMin, Vector2f viewMax, float scale,
                        std::vector<ItemRect>* rects) {
            if (const auto* text = scene.TryGet<ScreenTextComponent>(entity); text && !text->text.empty()) {
                const float pixelSize = text->size * scale;
                const Vector2f size = list.MeasureText(text->text, pixelSize);
                // Le point du texte qui correspond à l'ancre (son coin, son milieu...) est posé sur l'ancre.
                const Vector2f fraction = AnchorFraction(text->anchor);
                const Vector2f topLeft = AnchorPoint(text->anchor, viewMin, viewMax)
                                       + Vector2f{text->offsetX, text->offsetY} * scale
                                       - Vector2f{size.x * fraction.x, size.y * fraction.y};
                list.AddText(topLeft, text->text, pixelSize, text->color);
                if (rects) {
                    rects->push_back({entity, topLeft, topLeft + size});
                }
            }
            scene.ForEachChild(entity, [&](Entity child) { DrawEntity(scene, child, list, viewMin, viewMax, scale, rects); });
        }
    }

    Vector2f AnchorPoint(ScreenAnchor anchor, Vector2f viewMin, Vector2f viewMax) {
        const Vector2f fraction = AnchorFraction(anchor);
        const Vector2f size = viewMax - viewMin;
        return viewMin + Vector2f{size.x * fraction.x, size.y * fraction.y};
    }

    float GetScale(Vector2f viewMin, Vector2f viewMax) {
        return std::max(viewMax.y - viewMin.y, 0.0f) / ReferenceHeight;
    }

    void Draw(const Scene& scene, UI::DrawList& list, Vector2f viewMin, Vector2f viewMax, std::vector<ItemRect>* rects) {
        const float scale = GetScale(viewMin, viewMax);
        if (scale <= 0.0f) {
            return;
        }
        // Rien ne déborde de la vue (dans l'éditeur, sur les panneaux voisins).
        list.SetClipRect(viewMin, viewMax);
        scene.ForEachRoot([&](Entity root) { DrawEntity(scene, root, list, viewMin, viewMax, scale, rects); });
        list.ResetClipRect();
    }
}
