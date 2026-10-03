#include "Flux/Interface/Interface.hpp"

#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace Flux::Interface {
    using Math::LinearColor;
    using Math::Vector2f;

    namespace {
        constexpr std::array<std::string_view, static_cast<std::size_t>(ElementType::Count)> TypeNames{
            "Fenêtre", "Cadre", "Texte", "Image", "Bouton", "Case à cocher", "Curseur"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(Anchor::Count)> AnchorNames{
            "En haut à gauche", "En haut",  "En haut à droite", "À gauche", "Au centre",
            "À droite",         "En bas à gauche", "En bas", "En bas à droite", "Étiré (marges)"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(TextAlign::Count)> AlignNames{"À gauche", "Centré",
                                                                                                      "À droite"};

        constexpr std::uint32_t Bit(ElementType type) { return 1u << static_cast<unsigned>(type); }
        constexpr std::uint32_t W = Bit(ElementType::Window);
        constexpr std::uint32_t F = Bit(ElementType::Frame);
        constexpr std::uint32_t T = Bit(ElementType::Text);
        constexpr std::uint32_t I = Bit(ElementType::Image);
        constexpr std::uint32_t B = Bit(ElementType::Button);
        constexpr std::uint32_t C = Bit(ElementType::Checkbox);
        constexpr std::uint32_t S = Bit(ElementType::Slider);

        const std::array Fields{
            Field{"text", "Titre", &Element::text, W},
            Field{"text", "Texte", &Element::text, T},
            Field{"text", "Libellé", &Element::text, B | C},
            Field{"fontSize", "Taille du texte", &Element::fontSize, W | T | B | C, false, 0.5f, 4.0f, 400.0f, 0},
            Field{"textColor", "Couleur du texte", &Element::textColor, W | T | B | C},
            Field{"textColor", "Poignée", &Element::textColor, S},
            Field{"align", "Alignement", &Element::align, T | B},
            Field{"image", "Image", &Element::image, I, true},
            Field{"tint", "Teinte", &Element::tint, I},
            Field{"keepAspect", "Garder les proportions", &Element::keepAspect, I},
            Field{"titleHeight", "Hauteur du titre", &Element::titleHeight, W, false, 0.5f, 0.0f, 400.0f, 0},
            Field{"titleColor", "Barre de titre", &Element::titleColor, W},
            Field{"background", "Fond", &Element::background, W | F | T | B | C},
            Field{"background", "Piste", &Element::background, S},
            Field{"hoverColor", "Survol", &Element::hoverColor, B | C | S},
            Field{"pressedColor", "Appuyé", &Element::pressedColor, B},
            Field{"accent", "Coche", &Element::accent, C},
            Field{"accent", "Remplissage", &Element::accent, S},
            Field{"border", "Bordure", &Element::border, W | F | B | C},
            Field{"borderWidth", "Épaisseur de bordure", &Element::borderWidth, W | F | B | C, false, 0.1f, 0.0f, 50.0f, 1},
            Field{"radius", "Arrondi des coins", &Element::radius, W | F | T | B | C, false, 0.5f, 0.0f, 500.0f, 0},
            Field{"shadow", "Ombre", &Element::shadow, W},
            Field{"movable", "Déplaçable en jeu", &Element::movable, W},
            Field{"checked", "Cochée", &Element::checked, C},
            Field{"value", "Valeur", &Element::value, S, false, 0.01f, -1.0e6f, 1.0e6f, 2},
            Field{"minimum", "Minimum", &Element::minimum, S, false, 0.01f, -1.0e6f, 1.0e6f, 2},
            Field{"maximum", "Maximum", &Element::maximum, S, false, 0.01f, -1.0e6f, 1.0e6f, 2},
        };

        LinearColor Srgb(int r, int g, int b, float a = 1.0f) { return LinearColor::FromSrgb8(r, g, b, a); }

        Element Defaults(ElementType type) {
            Element element;
            element.type = type;
            element.textColor = Srgb(232, 238, 244);
            switch (type) {
                case ElementType::Window:
                    element.size = {640.0f, 420.0f};
                    element.background = Srgb(16, 22, 32, 0.94f);
                    element.border = Srgb(70, 190, 220, 0.6f);
                    element.borderWidth = 1.0f;
                    element.radius = 10.0f;
                    element.text = "Fenêtre";
                    element.titleColor = Srgb(26, 36, 50);
                    break;
                case ElementType::Frame:
                    element.size = {320.0f, 200.0f};
                    element.background = Srgb(30, 38, 50, 0.85f);
                    element.radius = 6.0f;
                    break;
                case ElementType::Text:
                    element.size = {320.0f, 48.0f};
                    element.text = "Texte";
                    element.fontSize = 32.0f;
                    break;
                case ElementType::Image:
                    element.size = {256.0f, 256.0f};
                    break;
                case ElementType::Button:
                    element.size = {260.0f, 56.0f};
                    element.text = "Bouton";
                    element.fontSize = 26.0f;
                    element.align = TextAlign::Center;
                    element.background = Srgb(40, 70, 100);
                    element.hoverColor = Srgb(55, 100, 140);
                    element.pressedColor = Srgb(30, 150, 190);
                    element.border = Srgb(90, 170, 210, 0.8f);
                    element.radius = 8.0f;
                    break;
                case ElementType::Checkbox:
                    element.size = {320.0f, 40.0f};
                    element.text = "Option";
                    element.background = Srgb(30, 40, 52);
                    element.hoverColor = Srgb(45, 60, 78);
                    element.accent = Srgb(60, 200, 230);
                    element.border = Srgb(90, 170, 210, 0.8f);
                    element.borderWidth = 1.0f;
                    element.radius = 4.0f;
                    break;
                case ElementType::Slider:
                    element.size = {320.0f, 28.0f};
                    element.background = Srgb(30, 40, 52);
                    element.hoverColor = Srgb(255, 255, 255);
                    element.accent = Srgb(60, 200, 230);
                    break;
                case ElementType::Count:
                    break;
            }
            return element;
        }

        Vector2f Fraction(Anchor anchor) {
            const auto index = static_cast<int>(anchor);
            return {static_cast<float>(index % 3) * 0.5f, static_cast<float>(index / 3) * 0.5f};
        }

        Rect Intersect(const Rect& a, const Rect& b) {
            const Vector2f min{std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y)};
            return {min, {std::max(min.x, std::min(a.max.x, b.max.x)), std::max(min.y, std::min(a.max.y, b.max.y))}};
        }

        bool IsEmpty(const Rect& rect) { return rect.max.x <= rect.min.x || rect.max.y <= rect.min.y; }

        void ArrangeChildren(const std::vector<Element>& elements, const Rect& parent, const Rect& clip, std::uint32_t parentId,
                             int depth, float scale, std::vector<Placed>& out) {
            for (const Element& element : elements) {
                if (!element.visible) {
                    continue;
                }
                const Rect rect = Arrange(element, parent, scale);
                out.push_back({&element, rect, parent, clip, parentId, depth});
                if (!element.children.empty()) {
                    ArrangeChildren(element.children, rect, Intersect(clip, rect), element.id, depth + 1, scale, out);
                }
            }
        }

        template <typename Elements, typename Visit>
        auto* FindIn(Elements& elements, std::uint32_t id, Visit&& visit) {
            for (auto& element : elements) {
                if (element.id == id) {
                    return visit(elements, element);
                }
                if (auto* found = FindIn(element.children, id, visit)) {
                    return found;
                }
            }
            return static_cast<decltype(visit(elements, elements.front()))>(nullptr);
        }

        void Renumber(Document& document, Element& element) {
            element.id = document.nextId++;
            for (Element& child : element.children) {
                Renumber(document, child);
            }
        }

        // Interface en jeu : les éléments qui réagissent à la souris (et la retiennent).
        bool IsInteractive(ElementType type) {
            return type == ElementType::Button || type == ElementType::Checkbox || type == ElementType::Slider;
        }

        // Fenêtres et cadres arrêtent aussi la souris : un clic sur leur fond ne traverse pas vers le jeu.
        bool BlocksMouse(ElementType type) {
            return IsInteractive(type) || type == ElementType::Window || type == ElementType::Frame;
        }

        const Placed* FindPlaced(const std::vector<Placed>& placed, std::uint32_t id) {
            const auto found = std::ranges::find_if(placed, [id](const Placed& item) { return item.element->id == id; });
            return found == placed.end() ? nullptr : &*found;
        }

        float SliderFraction(const Element& slider) {
            const float range = slider.maximum - slider.minimum;
            return range != 0.0f ? std::clamp((slider.value - slider.minimum) / range, 0.0f, 1.0f) : 0.0f;
        }

        // Partie où glisse le centre de la poignée : la poignée reste dans le rectangle aux deux bouts.
        std::pair<float, float> SliderTrack(const Rect& rect) {
            const float knob = 0.5f * (rect.max.y - rect.min.y);
            return {rect.min.x + knob, std::max(rect.min.x + knob, rect.max.x - knob)};
        }

        struct DrawContext
        {
            UI::DrawList& list;
            float scale;
            const DrawOptions& options;
        };

        // Texte aligné dans le rectangle (centré verticalement), avec une marge sur les côtés.
        void DrawLabel(const DrawContext& context, const Element& element, const Rect& rect, float padding) {
            if (element.text.empty() || element.textColor.a <= 0.0f) {
                return;
            }
            const float pixelSize = element.fontSize * context.scale;
            const Vector2f size = context.list.MeasureText(element.text, pixelSize);
            float x = rect.min.x + padding;
            if (element.align == TextAlign::Center) {
                x = 0.5f * (rect.min.x + rect.max.x - size.x);
            } else if (element.align == TextAlign::Right) {
                x = rect.max.x - padding - size.x;
            }
            context.list.AddText({x, 0.5f * (rect.min.y + rect.max.y - size.y)}, element.text, pixelSize, element.textColor);
        }

        void DrawBox(const DrawContext& context, const Rect& rect, LinearColor fill, const Element& element) {
            const float radius = element.radius * context.scale;
            if (fill.a > 0.0f) {
                context.list.AddRoundedRect(rect.min, rect.max, fill, radius);
            }
            if (element.borderWidth > 0.0f && element.border.a > 0.0f) {
                context.list.AddRoundedRectOutline(rect.min, rect.max, element.border, radius,
                                                   std::max(1.0f, element.borderWidth * context.scale));
            }
        }

        void DrawWindow(const DrawContext& context, const Placed& placed) {
            const Element& element = *placed.element;
            const Rect& rect = placed.rect;
            const float radius = element.radius * context.scale;
            if (element.shadow) {
                context.list.AddShadow(rect.min, rect.max, LinearColor{0.0f, 0.0f, 0.0f, 0.55f}, radius, 18.0f * context.scale);
            }
            if (element.background.a > 0.0f) {
                context.list.AddRoundedRect(rect.min, rect.max, element.background, radius);
            }
            const float titleHeight = std::min(element.titleHeight * context.scale, rect.max.y - rect.min.y);
            if (titleHeight > 0.0f) {
                // Le fond arrondi entier, découpé à la bande du titre : seuls les coins du haut sont arrondis.
                const Rect band{rect.min, {rect.max.x, rect.min.y + titleHeight}};
                const Rect clip = Intersect(placed.clip, band);
                if (!IsEmpty(clip)) {
                    context.list.SetClipRect(clip.min, clip.max);
                    if (element.titleColor.a > 0.0f) {
                        context.list.AddRoundedRect(rect.min, rect.max, element.titleColor, radius);
                    }
                    DrawLabel(context, element, band, 14.0f * context.scale);
                    context.list.SetClipRect(placed.clip.min, placed.clip.max);
                }
            }
            if (element.borderWidth > 0.0f && element.border.a > 0.0f) {
                context.list.AddRoundedRectOutline(rect.min, rect.max, element.border, radius,
                                                   std::max(1.0f, element.borderWidth * context.scale));
            }
        }

        void DrawImage(const DrawContext& context, const Element& element, const Rect& rect) {
            std::optional<UI::TextureInfo> texture;
            if (!element.image.empty() && context.options.textures) {
                texture = context.options.textures(element.image);
            }
            if (texture && (texture->size.x <= 0 || texture->size.y <= 0)) {
                texture.reset();
            }
            if (!texture) {
                if (context.options.placeholders) {
                    context.list.AddRect(rect.min, rect.max, Srgb(90, 90, 100, 0.45f));
                    context.list.AddRectOutline(rect.min, rect.max, Srgb(170, 170, 180), 1.0f);
                    const std::string_view label = element.image.empty() ? "Image : aucun fichier" : "Image introuvable";
                    const float pixelSize = std::max(11.0f, 18.0f * context.scale);
                    const Vector2f size = context.list.MeasureText(label, pixelSize);
                    context.list.AddText((rect.min + rect.max) * 0.5f - size * 0.5f, label, pixelSize, Srgb(220, 220, 225));
                }
                return;
            }
            Rect shown = rect;
            if (element.keepAspect) {
                const Vector2f box = rect.max - rect.min;
                const float fit = std::min(box.x / static_cast<float>(texture->size.x), box.y / static_cast<float>(texture->size.y));
                const Vector2f size{static_cast<float>(texture->size.x) * fit, static_cast<float>(texture->size.y) * fit};
                shown.min = rect.min + (box - size) * 0.5f;
                shown.max = shown.min + size;
            }
            context.list.AddImage(shown.min, shown.max, texture->id, element.tint);
        }

        void DrawCheckbox(const DrawContext& context, const Element& element, const Rect& rect, bool hovered) {
            const float side = std::min(rect.max.y - rect.min.y, rect.max.x - rect.min.x);
            const float top = 0.5f * (rect.min.y + rect.max.y - side);
            const Rect box{{rect.min.x, top}, {rect.min.x + side, top + side}};
            DrawBox(context, box, hovered ? element.hoverColor : element.background, element);
            if (element.checked) {
                const float inset = side * 0.24f;
                context.list.AddRoundedRect(box.min + Vector2f{inset, inset}, box.max - Vector2f{inset, inset}, element.accent,
                                            std::max(0.0f, element.radius * context.scale - inset * 0.5f));
            }
            DrawLabel(context, element, {{box.max.x, rect.min.y}, rect.max}, 10.0f * context.scale);
        }

        void DrawSlider(const DrawContext& context, const Element& element, const Rect& rect, bool hovered) {
            const float height = rect.max.y - rect.min.y;
            const auto [start, end] = SliderTrack(rect);
            const float centerY = 0.5f * (rect.min.y + rect.max.y);
            const float track = std::max(2.0f, height * 0.22f);
            const float knobX = start + (end - start) * SliderFraction(element);
            context.list.AddRoundedRect({start, centerY - track * 0.5f}, {end, centerY + track * 0.5f}, element.background,
                                        track * 0.5f);
            if (knobX > start) {
                context.list.AddRoundedRect({start, centerY - track * 0.5f}, {knobX, centerY + track * 0.5f}, element.accent,
                                            track * 0.5f);
            }
            context.list.AddCircle({knobX, centerY}, height * 0.42f, hovered ? element.hoverColor : element.textColor);
        }

        void DrawElement(const DrawContext& context, const Placed& placed) {
            const Element& element = *placed.element;
            const Rect& rect = placed.rect;
            const std::uint32_t id = element.id;
            const Interaction* interaction = context.options.interaction;
            const bool hovered = interaction && (interaction->hovered == id || interaction->pressed == id);
            const bool pressed = interaction && interaction->pressed == id && interaction->hovered == id;
            switch (element.type) {
                case ElementType::Window:
                    DrawWindow(context, placed);
                    break;
                case ElementType::Frame:
                    DrawBox(context, rect, element.background, element);
                    break;
                case ElementType::Text:
                    DrawBox(context, rect, element.background, element);
                    DrawLabel(context, element, rect, 0.0f);
                    break;
                case ElementType::Image:
                    DrawImage(context, element, rect);
                    break;
                case ElementType::Button:
                    DrawBox(context, rect, pressed ? element.pressedColor : hovered ? element.hoverColor : element.background,
                            element);
                    DrawLabel(context, element, rect, 12.0f * context.scale);
                    break;
                case ElementType::Checkbox:
                    DrawCheckbox(context, element, rect, hovered);
                    break;
                case ElementType::Slider:
                    DrawSlider(context, element, rect, hovered);
                    break;
                case ElementType::Count:
                    break;
            }
        }
    }

    std::span<const std::string_view> GetTypeNames() {
        return TypeNames;
    }

    std::span<const std::string_view> GetAnchorNames() {
        return AnchorNames;
    }

    std::span<const std::string_view> GetAlignNames() {
        return AlignNames;
    }

    std::span<const Field> GetFields() {
        return Fields;
    }

    bool IsContainer(ElementType type) {
        return type == ElementType::Window || type == ElementType::Frame;
    }

    Element MakeDefault(ElementType type) {
        return Defaults(type);
    }

    Element MakeElement(Document& document, ElementType type) {
        Element element = Defaults(type);
        element.id = document.nextId++;
        element.name = std::string(TypeNames[static_cast<std::size_t>(type) % TypeNames.size()]) + " "
                     + std::to_string(element.id);
        return element;
    }

    Element* Find(Document& document, std::uint32_t id) {
        return FindIn(document.roots, id, [](auto&, Element& element) { return &element; });
    }

    const Element* Find(const Document& document, std::uint32_t id) {
        return FindIn(document.roots, id, [](const auto&, const Element& element) { return &element; });
    }

    std::vector<Element>* FindSiblings(Document& document, std::uint32_t id, std::size_t& index) {
        return FindIn(document.roots, id, [&index](std::vector<Element>& siblings, Element& element) {
            index = static_cast<std::size_t>(&element - siblings.data());
            return &siblings;
        });
    }

    Element* FindParent(Document& document, std::uint32_t id) {
        std::size_t index = 0;
        std::vector<Element>* siblings = FindSiblings(document, id, index);
        if (!siblings || siblings == &document.roots) {
            return nullptr;
        }
        // Le parent est celui dont la liste des enfants est siblings.
        Element* parent = nullptr;
        const auto search = [&](auto&& self, std::vector<Element>& elements) -> void {
            for (Element& element : elements) {
                if (&element.children == siblings) {
                    parent = &element;
                    return;
                }
                self(self, element.children);
                if (parent) {
                    return;
                }
            }
        };
        search(search, document.roots);
        return parent;
    }

    std::optional<Element> Remove(Document& document, std::uint32_t id) {
        std::size_t index = 0;
        std::vector<Element>* siblings = FindSiblings(document, id, index);
        if (!siblings) {
            return std::nullopt;
        }
        Element removed = std::move((*siblings)[index]);
        siblings->erase(siblings->begin() + static_cast<std::ptrdiff_t>(index));
        return removed;
    }

    Element CloneWithNewIds(Document& document, const Element& element) {
        Element copy = element;
        Renumber(document, copy);
        return copy;
    }

    float GetScale(Vector2f viewMin, Vector2f viewMax) {
        return std::max(viewMax.y - viewMin.y, 0.0f) / ReferenceHeight;
    }

    Rect Arrange(const Element& element, const Rect& parent, float scale) {
        if (element.anchor == Anchor::Stretch) {
            const Vector2f min = parent.min + element.position * scale;
            const Vector2f max = parent.max - element.size * scale;
            return {min, {std::max(min.x, max.x), std::max(min.y, max.y)}};
        }
        const Vector2f fraction = Fraction(element.anchor);
        const Vector2f parentSize = parent.max - parent.min;
        const Vector2f size{std::max(element.size.x, 0.0f) * scale, std::max(element.size.y, 0.0f) * scale};
        const Vector2f point = parent.min + Vector2f{parentSize.x * fraction.x, parentSize.y * fraction.y};
        const Vector2f min = point + element.position * scale - Vector2f{size.x * fraction.x, size.y * fraction.y};
        return {min, min + size};
    }

    void SetRect(Element& element, const Rect& parent, float scale, const Rect& rect) {
        if (scale <= 0.0f) {
            return;
        }
        const float inverse = 1.0f / scale;
        if (element.anchor == Anchor::Stretch) {
            element.position = (rect.min - parent.min) * inverse;
            element.size = (parent.max - rect.max) * inverse;
            return;
        }
        const Vector2f fraction = Fraction(element.anchor);
        const Vector2f parentSize = parent.max - parent.min;
        const Vector2f point = parent.min + Vector2f{parentSize.x * fraction.x, parentSize.y * fraction.y};
        element.size = (rect.max - rect.min) * inverse;
        element.position = (rect.min - point) * inverse + Vector2f{element.size.x * fraction.x, element.size.y * fraction.y};
    }

    Vector2f AnchorPoint(Anchor anchor, const Rect& parent) {
        if (anchor == Anchor::Stretch) {
            return parent.min;
        }
        const Vector2f fraction = Fraction(anchor);
        const Vector2f size = parent.max - parent.min;
        return parent.min + Vector2f{size.x * fraction.x, size.y * fraction.y};
    }

    std::vector<Placed> Arrange(const Document& document, Vector2f viewMin, Vector2f viewMax, const std::optional<Rect>& clip) {
        std::vector<Placed> placed;
        const float scale = GetScale(viewMin, viewMax);
        if (scale <= 0.0f || viewMax.x <= viewMin.x) {
            return placed;
        }
        const Rect view{viewMin, viewMax};
        ArrangeChildren(document.roots, view, clip ? Intersect(view, *clip) : view, 0, 0, scale, placed);
        return placed;
    }

    std::uint32_t FindAt(std::span<const Placed> placed, Vector2f point) {
        for (auto item = placed.rbegin(); item != placed.rend(); ++item) {
            if (Intersect(item->rect, item->clip).Contains(point)) {
                return item->element->id;
            }
        }
        return 0;
    }

    void Update(Document& document, Interaction& interaction, Vector2f viewMin, Vector2f viewMax, const Pointer& pointer) {
        interaction.clicked = 0;
        const std::vector<Placed> placed = Arrange(document, viewMin, viewMax);
        const float scale = GetScale(viewMin, viewMax);

        // Élément le plus en avant sous la souris qui l'arrête (le texte et les images la laissent passer).
        const Placed* hit = nullptr;
        if (pointer.enabled) {
            for (auto item = placed.rbegin(); item != placed.rend(); ++item) {
                if (BlocksMouse(item->element->type) && Intersect(item->rect, item->clip).Contains(pointer.position)) {
                    hit = &*item;
                    break;
                }
            }
        }
        const std::uint32_t hitId = hit ? hit->element->id : 0;
        interaction.hovered = hit && IsInteractive(hit->element->type) ? hitId : 0;

        if (pointer.pressed && hit) {
            const Element& element = *hit->element;
            const bool titleBar = element.type == ElementType::Window && element.movable && element.anchor != Anchor::Stretch
                               && pointer.position.y < hit->rect.min.y + element.titleHeight * scale;
            interaction.pressed = IsInteractive(element.type) || titleBar ? hitId : 0;
            interaction.dragStart = pointer.position;
            interaction.dragOrigin = element.position;
        }

        Element* pressed = interaction.pressed != 0 ? Find(document, interaction.pressed) : nullptr;
        if (pressed && pressed->visible) {
            if (pressed->type == ElementType::Window && scale > 0.0f) {
                // Souris bornée à la vue : la barre de titre ne peut pas sortir de l'écran.
                const Vector2f mouse{std::clamp(pointer.position.x, viewMin.x, viewMax.x),
                                     std::clamp(pointer.position.y, viewMin.y, viewMax.y)};
                pressed->position = interaction.dragOrigin + (mouse - interaction.dragStart) * (1.0f / scale);
            } else if (pressed->type == ElementType::Slider) {
                if (const Placed* slider = FindPlaced(placed, pressed->id)) {
                    const auto [start, end] = SliderTrack(slider->rect);
                    const float t = end > start ? std::clamp((pointer.position.x - start) / (end - start), 0.0f, 1.0f) : 0.0f;
                    pressed->value = pressed->minimum + (pressed->maximum - pressed->minimum) * t;
                }
            }
            if (pointer.released && hitId == pressed->id) {
                if (pressed->type == ElementType::Button) {
                    interaction.clicked = pressed->id;
                } else if (pressed->type == ElementType::Checkbox) {
                    pressed->checked = !pressed->checked;
                }
            }
        }
        if (!pressed || !pressed->visible || pointer.released || !pointer.down) {
            interaction.pressed = 0;
        }
        interaction.wantsMouse = hit != nullptr || interaction.pressed != 0;
    }

    void Draw(const Document& document, UI::DrawList& list, Vector2f viewMin, Vector2f viewMax, const DrawOptions& options) {
        const std::vector<Placed> placed = Arrange(document, viewMin, viewMax, options.clip);
        if (placed.empty()) {
            return;
        }
        const DrawContext context{list, GetScale(viewMin, viewMax), options};
        for (const Placed& item : placed) {
            if (IsEmpty(Intersect(item.clip, {item.rect.min - Vector2f{40.0f, 40.0f}, item.rect.max + Vector2f{40.0f, 40.0f}}))) {
                continue; // hors de la partie visible de son parent (ombre comprise)
            }
            list.SetClipRect(item.clip.min, item.clip.max);
            DrawElement(context, item);
        }
        list.ResetClipRect();
    }
}
