#include "Flux/Scene/ScreenUi.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace Flux::ScreenUi {
    using Math::Vector2f;

    namespace {
        // Taille (pixels de référence) du cadre qui remplace une image absente, dans l'éditeur.
        constexpr Vector2f PlaceholderSize{256.0f, 144.0f};

        struct DrawContext
        {
            const Scene& scene;
            UI::DrawList& list;
            Vector2f viewMin;
            Vector2f viewMax;
            float scale;
            const DrawOptions& options;
        };

        // Position de l'ancre en fractions de la vue : 0 à gauche (en haut), 0,5 au milieu, 1 à droite (en bas).
        Vector2f AnchorFraction(ScreenAnchor anchor) {
            const auto index = static_cast<int>(anchor);
            return {static_cast<float>(index % 3) * 0.5f, static_cast<float>(index / 3) * 0.5f};
        }

        // Coin haut-gauche d'un élément de cette taille (pixels de la vue) : le point de l'élément qui correspond
        // à l'ancre (son coin, son milieu...) est posé sur l'ancre, puis décalé.
        Vector2f PlaceByAnchor(const DrawContext& context, ScreenAnchor anchor, float offsetX, float offsetY, Vector2f size) {
            const Vector2f fraction = AnchorFraction(anchor);
            return AnchorPoint(anchor, context.viewMin, context.viewMax) + Vector2f{offsetX, offsetY} * context.scale
                 - Vector2f{size.x * fraction.x, size.y * fraction.y};
        }

        void AddRect(const DrawContext& context, Entity entity, Vector2f min, Vector2f max) {
            if (!context.options.rects) {
                return;
            }
            // Découpé à la vue, comme le dessin : un fond qui déborde ne se sélectionne pas sur les panneaux voisins.
            min = {std::max(min.x, context.viewMin.x), std::max(min.y, context.viewMin.y)};
            max = {std::min(max.x, context.viewMax.x), std::min(max.y, context.viewMax.y)};
            if (min.x < max.x && min.y < max.y) {
                context.options.rects->push_back({entity, min, max});
            }
        }

        // Rectangle de l'image dans la vue ; native : taille de l'image en pixels de référence.
        std::pair<Vector2f, Vector2f> ImageRect(const DrawContext& context, const ScreenImageComponent& image, Vector2f native) {
            const Vector2f viewSize = context.viewMax - context.viewMin;
            switch (image.fit) {
                case ScreenImageFit::Stretch:
                    return {context.viewMin, context.viewMax};
                case ScreenImageFit::Cover: {
                    const float cover = std::max(viewSize.x / native.x, viewSize.y / native.y);
                    const Vector2f size = native * cover;
                    const Vector2f min = context.viewMin + (viewSize - size) * 0.5f;
                    return {min, min + size};
                }
                case ScreenImageFit::Size:
                    break;
            }
            Vector2f size{image.width, image.height};
            if (size.x <= 0.0f && size.y <= 0.0f) {
                size = native;
            } else if (size.x <= 0.0f) {
                size.x = size.y * native.x / native.y;
            } else if (size.y <= 0.0f) {
                size.y = size.x * native.y / native.x;
            }
            size = size * context.scale;
            const Vector2f min = PlaceByAnchor(context, image.anchor, image.offsetX, image.offsetY, size);
            return {min, min + size};
        }

        void DrawImage(const DrawContext& context, Entity entity, const ScreenImageComponent& image) {
            std::optional<UI::TextureInfo> texture;
            if (!image.image.empty() && context.options.textures) {
                texture = context.options.textures(image.image);
            }
            if (texture && (texture->size.x <= 0 || texture->size.y <= 0)) {
                texture.reset();
            }
            if (!texture && !context.options.placeholders) {
                return;
            }
            const Vector2f native = texture ? Vector2f{static_cast<float>(texture->size.x), static_cast<float>(texture->size.y)}
                                            : PlaceholderSize;
            const auto [min, max] = ImageRect(context, image, native);
            if (texture) {
                const Math::LinearColor tint{image.tint.r, image.tint.g, image.tint.b,
                                             image.tint.a * std::clamp(image.opacity, 0.0f, 1.0f)};
                context.list.AddImage(min, max, texture->id, tint);
            } else {
                context.list.AddRect(min, max, Math::LinearColor::FromSrgb8(90, 90, 100, 0.45f));
                context.list.AddRectOutline(min, max, Math::LinearColor::FromSrgb8(170, 170, 180), 1.0f);
                const std::string label = image.image.empty() ? "Image : aucun fichier" : "Image introuvable : " + image.image;
                const float pixelSize = std::max(12.0f, 20.0f * context.scale);
                const Vector2f size = context.list.MeasureText(label, pixelSize);
                context.list.AddText((min + max) * 0.5f - size * 0.5f, label, pixelSize,
                                     Math::LinearColor::FromSrgb8(220, 220, 225));
            }
            AddRect(context, entity, min, max);
        }

        void DrawText(const DrawContext& context, Entity entity, const ScreenTextComponent& text) {
            if (text.text.empty()) {
                return;
            }
            std::optional<UI::FontRef> font;
            if (!text.font.empty() && context.options.fonts) {
                font = context.options.fonts(text.font);
            }
            // Police absente ou illisible : celle de la liste, pour que le texte reste visible et sélectionnable.
            const UI::FontRef used = font ? *font : UI::FontRef{&context.list.GetFont(), UI::FontTexture};
            const float pixelSize = text.size * context.scale;
            const Vector2f size = UI::DrawList::MeasureText(*used.font, text.text, pixelSize);
            const Vector2f topLeft = PlaceByAnchor(context, text.anchor, text.offsetX, text.offsetY, size);
            context.list.AddText(used, topLeft, text.text, pixelSize, text.color);
            AddRect(context, entity, topLeft, topLeft + size);
        }

        void DrawInterface(const DrawContext& context, Entity entity, const InterfaceComponent& component) {
            if (!context.options.interfaces) {
                return;
            }
            const auto rect = context.options.interfaces(
                {entity, component, context.list, context.viewMin, context.viewMax, context.options.placeholders,
                 context.options.rects != nullptr});
            // L'interface a pu changer le rectangle de découpe (fenêtres) : celui de la vue revient pour la suite.
            context.list.SetClipRect(context.viewMin, context.viewMax);
            if (rect) {
                AddRect(context, entity, rect->first, rect->second);
            }
        }

        void DrawEntity(const DrawContext& context, Entity entity) {
            if (const auto* component = context.scene.TryGet<InterfaceComponent>(entity)) {
                DrawInterface(context, entity, *component);
            }
            if (const auto* image = context.scene.TryGet<ScreenImageComponent>(entity)) {
                DrawImage(context, entity, *image);
            }
            if (const auto* text = context.scene.TryGet<ScreenTextComponent>(entity)) {
                DrawText(context, entity, *text);
            }
            context.scene.ForEachChild(entity, [&](Entity child) { DrawEntity(context, child); });
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

    void Draw(const Scene& scene, UI::DrawList& list, Vector2f viewMin, Vector2f viewMax, const DrawOptions& options) {
        const float scale = GetScale(viewMin, viewMax);
        if (scale <= 0.0f || viewMax.x <= viewMin.x) {
            return;
        }
        const DrawContext context{scene, list, viewMin, viewMax, scale, options};
        // Rien ne déborde de la vue (dans l'éditeur, sur les panneaux voisins).
        list.SetClipRect(viewMin, viewMax);
        scene.ForEachRoot([&](Entity root) { DrawEntity(context, root); });
        list.ResetClipRect();
    }

    bool IsScreenElement(const Scene& scene, Entity entity) {
        return scene.TryGet<ScreenTextComponent>(entity) || scene.TryGet<ScreenImageComponent>(entity)
            || scene.TryGet<InterfaceComponent>(entity);
    }

    std::optional<Offset> FindOffset(Scene& scene, Entity entity) {
        // Le texte d'abord : il est dessiné par-dessus l'image, c'est lui qu'on voit et qu'on saisit.
        if (auto* text = scene.TryGet<ScreenTextComponent>(entity)) {
            return Offset{&text->offsetX, &text->offsetY};
        }
        if (auto* image = scene.TryGet<ScreenImageComponent>(entity); image && image->fit == ScreenImageFit::Size) {
            return Offset{&image->offsetX, &image->offsetY};
        }
        return std::nullopt;
    }
}
