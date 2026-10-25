#include "Flux/Interface/Interface.hpp"

#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <string>
#include <utility>

namespace Flux::Interface {
    using Math::LinearColor;
    using Math::Vector2f;

    namespace {
        constexpr std::array<std::string_view, static_cast<std::size_t>(ElementType::Count)> TypeNames{
            "Fenêtre", "Cadre", "Texte", "Image", "Bouton", "Case à cocher", "Curseur", "Onglets", "Barre de progression",
            "Zone de défilement", "Liste déroulante"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(Anchor::Count)> AnchorNames{
            "En haut à gauche", "En haut",  "En haut à droite", "À gauche", "Au centre",
            "À droite",         "En bas à gauche", "En bas", "En bas à droite", "Étiré (marges)"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(TextAlign::Count)> AlignNames{"À gauche", "Centré",
                                                                                                      "À droite"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(ButtonAction::Count)> ActionNames{
            "Aucune",           "Ouvrir une fenêtre", "Fermer une fenêtre", "Ouvrir ou fermer une fenêtre",
            "Fermer sa fenêtre", "Changer de scène",  "Quitter le jeu",     "Fonction C++",
            "Afficher un onglet", "Changer une touche", "Touches par défaut"};

        constexpr std::uint32_t Bit(ElementType type) { return 1u << static_cast<unsigned>(type); }
        constexpr std::uint32_t W = Bit(ElementType::Window);
        constexpr std::uint32_t F = Bit(ElementType::Frame);
        constexpr std::uint32_t T = Bit(ElementType::Text);
        constexpr std::uint32_t I = Bit(ElementType::Image);
        constexpr std::uint32_t B = Bit(ElementType::Button);
        constexpr std::uint32_t C = Bit(ElementType::Checkbox);
        constexpr std::uint32_t S = Bit(ElementType::Slider);
        constexpr std::uint32_t O = Bit(ElementType::Tabs);
        constexpr std::uint32_t P = Bit(ElementType::ProgressBar);
        constexpr std::uint32_t D = Bit(ElementType::ScrollArea);
        constexpr std::uint32_t L = Bit(ElementType::Dropdown);

        constexpr float WheelStep = 60.0f;  // pixels de référence par cran de molette
        constexpr float MinThumb = 24.0f;   // hauteur minimale de la poignée de l'ascenseur (pixels de référence)

        const std::array Fields{
            Field{"text", "Titre", &Element::text, W},
            Field{"text", "Texte", &Element::text, T},
            Field{"text", "Libellé", &Element::text, B | C},
            Field{"options", "Choix", &Element::options, L},
            Field{"value", "Choix fait", &Element::value, L, Field::Text::Plain, 0.05f, 0.0f, 255.0f, 0},
            Field{"fontSize", "Taille du texte", &Element::fontSize, W | T | B | C | O | L, Field::Text::Plain, 0.5f, 4.0f, 400.0f, 0},
            Field{"textColor", "Couleur du texte", &Element::textColor, W | T | B | C | O | L},
            Field{"textColor", "Poignée", &Element::textColor, S},
            Field{"align", "Alignement", &Element::align, T | B},
            Field{"image", "Image", &Element::image, I, Field::Text::Image},
            Field{"tint", "Teinte", &Element::tint, I},
            Field{"keepAspect", "Garder les proportions", &Element::keepAspect, I},
            Field{"titleHeight", "Hauteur du titre", &Element::titleHeight, W, Field::Text::Plain, 0.5f, 0.0f, 400.0f, 0},
            Field{"titleColor", "Barre de titre", &Element::titleColor, W},
            Field{"titleHeight", "Hauteur des onglets", &Element::titleHeight, O, Field::Text::Plain, 0.5f, 8.0f, 400.0f, 0},
            Field{"titleColor", "Onglet", &Element::titleColor, O},
            Field{"pressedColor", "Onglet affiché", &Element::pressedColor, O},
            Field{"accent", "Soulignement", &Element::accent, O},
            Field{"background", "Fond", &Element::background, W | F | T | B | C | O | P | D | L},
            Field{"background", "Piste", &Element::background, S},
            Field{"scrollbarWidth", "Largeur de l'ascenseur", &Element::scrollbarWidth, D, Field::Text::Plain, 0.25f, 2.0f, 200.0f, 0},
            Field{"titleColor", "Piste de l'ascenseur", &Element::titleColor, D},
            Field{"accent", "Ascenseur", &Element::accent, D},
            Field{"hoverColor", "Survol", &Element::hoverColor, B | C | S | O | D | L},
            Field{"accent", "Flèche et choix fait", &Element::accent, L},
            Field{"pressedColor", "Appuyé", &Element::pressedColor, B},
            Field{"accent", "Coche", &Element::accent, C},
            Field{"accent", "Remplissage", &Element::accent, S | P},
            Field{"border", "Bordure", &Element::border, W | F | B | C | O | P | D | L},
            Field{"borderWidth", "Épaisseur de bordure", &Element::borderWidth, W | F | B | C | O | P | D | L, Field::Text::Plain, 0.1f, 0.0f, 50.0f, 1},
            Field{"radius", "Arrondi des coins", &Element::radius, W | F | T | B | C | O | P | D | L, Field::Text::Plain, 0.5f, 0.0f, 500.0f, 0},
            Field{"shadow", "Ombre", &Element::shadow, W},
            Field{"movable", "Déplaçable en jeu", &Element::movable, W},
            Field{"closeButton", "Croix de fermeture", &Element::closeButton, W},
            Field{"closeOnEscape", "Échap la ferme", &Element::closeOnEscape, W},
            Field{"openOnEscape", "Échap l'ouvre (menu pause)", &Element::openOnEscape, W},
            Field{"pausesGame", "Met le jeu en pause", &Element::pausesGame, W},
            Field{"action", "Action", &Element::action, B},
            Field{"target", "Cible", &Element::target, B, Field::Text::ActionTarget},
            Field{"checked", "Cochée", &Element::checked, C},
            Field{"binding", "Liaison", &Element::binding, C | S | T | B | P | L, Field::Text::Binding},
            Field{"value", "Valeur", &Element::value, S | P, Field::Text::Plain, 0.01f, -1.0e6f, 1.0e6f, 2},
            Field{"minimum", "Minimum", &Element::minimum, S | P, Field::Text::Plain, 0.01f, -1.0e6f, 1.0e6f, 2},
            Field{"maximum", "Maximum", &Element::maximum, S | P, Field::Text::Plain, 0.01f, -1.0e6f, 1.0e6f, 2},
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
                case ElementType::Tabs:
                    element.size = {600.0f, 400.0f};
                    element.titleHeight = 44.0f;
                    element.fontSize = 22.0f;
                    element.background = Srgb(20, 28, 40, 0.9f);
                    element.titleColor = Srgb(30, 40, 54);
                    element.hoverColor = Srgb(45, 60, 78);
                    element.pressedColor = Srgb(40, 70, 100);
                    element.accent = Srgb(60, 200, 230);
                    element.border = Srgb(90, 170, 210, 0.6f);
                    element.radius = 6.0f;
                    break;
                case ElementType::ProgressBar:
                    element.size = {320.0f, 24.0f};
                    element.background = Srgb(30, 40, 52);
                    element.accent = Srgb(60, 200, 230);
                    element.border = Srgb(90, 170, 210, 0.6f);
                    element.borderWidth = 1.0f;
                    element.radius = 4.0f;
                    break;
                case ElementType::ScrollArea:
                    element.size = {400.0f, 300.0f};
                    element.background = Srgb(20, 28, 40, 0.6f);
                    element.titleColor = Srgb(30, 40, 52);
                    element.accent = Srgb(70, 110, 140);
                    element.hoverColor = Srgb(60, 200, 230);
                    element.radius = 4.0f;
                    break;
                case ElementType::Dropdown:
                    element.size = {320.0f, 44.0f};
                    element.options = {"Choix 1", "Choix 2", "Choix 3"};
                    element.value = 0.0f;
                    element.background = Srgb(30, 40, 52);
                    element.hoverColor = Srgb(45, 60, 78);
                    element.accent = Srgb(60, 200, 230);
                    element.border = Srgb(90, 170, 210, 0.8f);
                    element.borderWidth = 1.0f;
                    element.radius = 4.0f;
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

        bool Contains(const Element& element, std::uint32_t id) {
            return std::ranges::any_of(element.children,
                                       [id](const Element& child) { return child.id == id || Contains(child, id); });
        }

        void ArrangeChildren(const std::vector<Element>& elements, const Rect& parent, const Rect& clip, std::uint32_t parentId,
                             int depth, float scale, std::uint32_t reveal, std::vector<Placed>& out);

        // Onglets : seule la page affichée est placée, sous la barre ; ou celle qui contient l'élément montré par
        // l'éditeur (reveal), sans changer le document.
        void ArrangePage(const Element& tabs, const Rect& rect, const Rect& clip, int depth, float scale, std::uint32_t reveal,
                         std::vector<Placed>& out) {
            int shown = GetShownTab(tabs);
            if (reveal != 0) {
                for (std::size_t index = 0; index < tabs.children.size(); ++index) {
                    if (tabs.children[index].id == reveal || Contains(tabs.children[index], reveal)) {
                        shown = static_cast<int>(index);
                    }
                }
            }
            const Element& page = tabs.children[static_cast<std::size_t>(shown)];
            const Rect content = GetTabContent(tabs, rect, scale);
            const Rect pageRect = Arrange(page, content, scale);
            const Rect pageClip = Intersect(clip, content);
            out.push_back({&page, pageRect, content, pageClip, tabs.id, depth + 1});
            if (!page.children.empty()) {
                ArrangeChildren(page.children, pageRect, Intersect(pageClip, pageRect), page.id, depth + 2, scale, reveal, out);
            }
        }

        bool IsShown(const Element& element, std::uint32_t reveal) {
            return element.visible || (reveal != 0 && (element.id == reveal || Contains(element, reveal)));
        }

        // Zone de défilement : les enfants se placent dans la zone (moins l'ascenseur s'il en faut un), remontés du
        // défilement, et ne se voient que dedans. Leur hauteur ne dépend pas de la largeur : on mesure d'abord ce
        // qui dépasse en bas, puis on retire la place de l'ascenseur seulement s'il est utile.
        void ArrangeScrollArea(const Element& area, const Rect& rect, const Rect& clip, int depth, float scale,
                               std::uint32_t reveal, std::vector<Placed>& out) {
            float bottom = rect.max.y;
            for (const Element& child : area.children) {
                if (IsShown(child, reveal)) {
                    bottom = std::max(bottom, Arrange(child, rect, scale).max.y);
                }
            }
            Placed& placed = out.back();
            placed.scrollRange = bottom - rect.max.y;
            Rect content = rect;
            if (const std::optional<Rect> track = GetScrollbarTrack(placed, scale)) {
                content.max.x = track->min.x;
            }
            const float offset = GetScrollOffset(placed, scale);
            const Rect shifted{content.min - Vector2f{0.0f, offset}, content.max - Vector2f{0.0f, offset}};
            ArrangeChildren(area.children, shifted, Intersect(clip, content), area.id, depth + 1, scale, reveal, out);
        }

        void ArrangeChildren(const std::vector<Element>& elements, const Rect& parent, const Rect& clip, std::uint32_t parentId,
                             int depth, float scale, std::uint32_t reveal, std::vector<Placed>& out) {
            for (const Element& element : elements) {
                if (!IsShown(element, reveal)) {
                    continue;
                }
                const Rect rect = Arrange(element, parent, scale);
                out.push_back({&element, rect, parent, clip, parentId, depth});
                if (element.children.empty()) {
                    continue;
                }
                if (element.type == ElementType::Tabs) {
                    ArrangePage(element, rect, clip, depth, scale, reveal, out);
                } else if (element.type == ElementType::ScrollArea) {
                    ArrangeScrollArea(element, rect, clip, depth, scale, reveal, out);
                } else {
                    ArrangeChildren(element.children, rect, Intersect(clip, rect), element.id, depth + 1, scale, reveal, out);
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
            return type == ElementType::Button || type == ElementType::Checkbox || type == ElementType::Slider
                || type == ElementType::Dropdown;
        }

        // Fenêtres et cadres arrêtent aussi la souris : un clic sur leur fond ne traverse pas vers le jeu.
        bool BlocksMouse(ElementType type) {
            return IsInteractive(type) || type == ElementType::Window || type == ElementType::Frame || type == ElementType::Tabs
                || type == ElementType::ScrollArea;
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

        // Liste déroulante dépliée : une ligne par choix, de la hauteur du champ, sous lui ; au-dessus s'il n'y a pas
        // la place en bas de la vue (et si elle tient mieux en haut).
        Rect DropdownList(const Element& dropdown, const Rect& field, const Rect& view) {
            const float row = field.max.y - field.min.y;
            const float height = row * static_cast<float>(dropdown.options.size());
            if (field.max.y + height > view.max.y && field.min.y - view.min.y > view.max.y - field.max.y) {
                return {{field.min.x, field.min.y - height}, {field.max.x, field.min.y}};
            }
            return {{field.min.x, field.max.y}, {field.max.x, field.max.y + height}};
        }

        int DropdownOptionAt(const Element& dropdown, const Rect& list, Vector2f point) {
            if (!list.Contains(point) || dropdown.options.empty()) {
                return -1;
            }
            const float row = (list.max.y - list.min.y) / static_cast<float>(dropdown.options.size());
            return std::clamp(static_cast<int>((point.y - list.min.y) / row), 0, static_cast<int>(dropdown.options.size()) - 1);
        }

        // Croix d'une fenêtre qui en a une : un carré au bout de sa barre de titre.
        std::optional<Rect> CloseButtonRect(const Element& element, const Rect& rect, float scale) {
            if (element.type != ElementType::Window || !element.closeButton || element.titleHeight <= 0.0f) {
                return std::nullopt;
            }
            const float side = std::min({element.titleHeight * scale, rect.max.y - rect.min.y, rect.max.x - rect.min.x});
            return Rect{{rect.max.x - side, rect.min.y}, {rect.max.x, rect.min.y + side}};
        }

        // Racine qui contient l'élément (lui-même s'il est à la racine), 0 s'il est absent.
        std::uint32_t RootOf(const Document& document, std::uint32_t id) {
            for (const Element& root : document.roots) {
                if (root.id == id || Contains(root, id)) {
                    return root.id;
                }
            }
            return 0;
        }

        // Met une fenêtre racine devant les autres (à la fin de la liste) ; sans effet sur les autres éléments, que
        // le concepteur a rangés (un fond, un HUD).
        void BringToFront(Document& document, std::uint32_t id) {
            const auto found = std::ranges::find_if(document.roots, [id](const Element& root) { return root.id == id; });
            if (found != document.roots.end() && found->type == ElementType::Window) {
                std::rotate(found, found + 1, document.roots.end());
            }
        }

        void SetOpen(Element& element, bool open, std::vector<std::uint32_t>& raise) {
            element.visible = open;
            if (open) {
                raise.push_back(element.id);
            }
        }

        // Action d'un bouton cliqué : les fenêtres ici, le reste pour le moteur (Interaction::action).
        void RunAction(Document& document, const Element& button, Interaction& interaction, std::vector<std::uint32_t>& raise) {
            switch (button.action) {
                case ButtonAction::OpenWindow:
                case ButtonAction::CloseWindow:
                case ButtonAction::ToggleWindow:
                    if (Element* window = FindByName(document, button.target)) {
                        const bool open = button.action == ButtonAction::OpenWindow
                                       || (button.action == ButtonAction::ToggleWindow && !window->visible);
                        SetOpen(*window, open, raise);
                    }
                    break;
                case ButtonAction::CloseParent:
                    for (Element* parent = FindParent(document, button.id); parent; parent = FindParent(document, parent->id)) {
                        if (parent->type == ElementType::Window) {
                            parent->visible = false;
                            break;
                        }
                    }
                    break;
                case ButtonAction::ShowTab:
                    ShowTab(document, button.target);
                    break;
                case ButtonAction::ChangeScene:
                case ButtonAction::Quit:
                case ButtonAction::CallFunction:
                case ButtonAction::RebindKey:
                case ButtonAction::ResetKeys:
                    interaction.action = button.action;
                    interaction.target = button.target;
                    break;
                case ButtonAction::None:
                case ButtonAction::Count:
                    break;
            }
        }

        struct DrawContext
        {
            UI::DrawList& list;
            float scale;
            const DrawOptions& options;
        };

        // Texte aligné dans le rectangle (centré verticalement), avec une marge sur les côtés.
        void DrawLabel(const DrawContext& context, const Element& element, const Rect& rect, float padding) {
            const std::string text = GetShownText(element);
            if (text.empty() || element.textColor.a <= 0.0f) {
                return;
            }
            const float pixelSize = element.fontSize * context.scale;
            const Vector2f size = context.list.MeasureText(text, pixelSize);
            float x = rect.min.x + padding;
            if (element.align == TextAlign::Center) {
                x = 0.5f * (rect.min.x + rect.max.x - size.x);
            } else if (element.align == TextAlign::Right) {
                x = rect.max.x - padding - size.x;
            }
            context.list.AddText({x, 0.5f * (rect.min.y + rect.max.y - size.y)}, text, pixelSize, element.textColor);
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

        // Croix de fermeture : fond rouge au survol, plus vif quand elle est tenue.
        void DrawCloseButton(const DrawContext& context, const Element& element, const Rect& box) {
            const Interaction* interaction = context.options.interaction;
            const bool hovered = interaction && interaction->closeHovered == element.id;
            const bool held = interaction && interaction->closePressed && interaction->pressed == element.id;
            const float side = box.max.x - box.min.x;
            if (hovered || held) {
                const float inset = side * 0.14f;
                context.list.AddRoundedRect(box.min + Vector2f{inset, inset}, box.max - Vector2f{inset, inset},
                                            held && hovered ? Srgb(220, 60, 60) : Srgb(190, 50, 50, 0.85f), side * 0.18f);
            }
            const Vector2f center = (box.min + box.max) * 0.5f;
            const float arm = side * 0.17f;
            const float thickness = std::max(1.5f, side * 0.05f);
            context.list.AddLine(center - Vector2f{arm, arm}, center + Vector2f{arm, arm}, element.textColor, thickness);
            context.list.AddLine(center + Vector2f{-arm, arm}, center + Vector2f{arm, -arm}, element.textColor, thickness);
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
                    const std::optional<Rect> close = CloseButtonRect(element, rect, context.scale);
                    DrawLabel(context, element, close ? Rect{band.min, {close->min.x, band.max.y}} : band,
                              14.0f * context.scale);
                    if (close) {
                        DrawCloseButton(context, element, *close);
                    }
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

        void DrawProgressBar(const DrawContext& context, const Element& element, const Rect& rect) {
            DrawBox(context, rect, element.background, element);
            const float fill = (rect.max.x - rect.min.x) * SliderFraction(element);
            if (fill > 0.0f && element.accent.a > 0.0f) {
                context.list.AddRoundedRect(rect.min, {rect.min.x + fill, rect.max.y}, element.accent,
                                            std::min(element.radius * context.scale, fill * 0.5f));
            }
        }

        // Texte d'un choix, à gauche du rectangle, centré verticalement.
        void DrawOption(const DrawContext& context, const Element& element, const Rect& rect, std::string_view text) {
            if (text.empty() || element.textColor.a <= 0.0f) {
                return;
            }
            const float pixelSize = element.fontSize * context.scale;
            const Vector2f size = context.list.MeasureText(text, pixelSize);
            context.list.AddText({rect.min.x + 12.0f * context.scale, 0.5f * (rect.min.y + rect.max.y - size.y)}, text, pixelSize,
                                 element.textColor);
        }

        // Le champ : le choix fait et une flèche à droite (vers le haut quand la liste est dépliée).
        void DrawDropdown(const DrawContext& context, const Placed& placed, bool hovered) {
            const Element& element = *placed.element;
            const Rect& rect = placed.rect;
            const Interaction* interaction = context.options.interaction;
            const bool open = interaction && interaction->openList == element.id;
            DrawBox(context, rect, hovered || open ? element.hoverColor : element.background, element);
            const float height = rect.max.y - rect.min.y;
            const float arrow = height * 0.16f;
            const Vector2f tip{rect.max.x - height * 0.5f, 0.5f * (rect.min.y + rect.max.y) + (open ? -0.5f : 0.5f) * arrow};
            const float back = open ? arrow : -arrow;
            context.list.AddTriangle(tip, tip + Vector2f{-arrow, back}, tip + Vector2f{arrow, back}, element.accent);
            const Rect text{rect.min, {rect.max.x - height, rect.max.y}};
            const Rect clip = Intersect(placed.clip, text); // un texte long s'arrête avant la flèche
            if (const int choice = GetDropdownChoice(element); choice >= 0 && !IsEmpty(clip)) {
                context.list.SetClipRect(clip.min, clip.max);
                DrawOption(context, element, text, element.options[static_cast<std::size_t>(choice)]);
                context.list.SetClipRect(placed.clip.min, placed.clip.max);
            }
        }

        // La liste dépliée, par-dessus tout le reste : le choix survolé, le choix fait marqué d'un trait à gauche.
        void DrawDropdownList(const DrawContext& context, const Element& element, const Rect& list) {
            const float radius = element.radius * context.scale;
            context.list.AddShadow(list.min, list.max, LinearColor{0.0f, 0.0f, 0.0f, 0.5f}, radius, 10.0f * context.scale);
            const LinearColor fill{element.background.r, element.background.g, element.background.b, 1.0f}; // jamais transparente
            context.list.AddRoundedRect(list.min, list.max, fill, radius);
            const float row = (list.max.y - list.min.y) / static_cast<float>(element.options.size());
            const int chosen = GetDropdownChoice(element);
            const int hovered = context.options.interaction ? context.options.interaction->hoveredOption : -1;
            for (int index = 0; index < static_cast<int>(element.options.size()); ++index) {
                const Rect rect{{list.min.x, list.min.y + row * static_cast<float>(index)},
                                {list.max.x, list.min.y + row * static_cast<float>(index + 1)}};
                if (index == hovered) {
                    context.list.AddRoundedRect(rect.min, rect.max, element.hoverColor, radius);
                }
                if (index == chosen) {
                    const float line = std::max(2.0f, 3.0f * context.scale);
                    context.list.AddRect({rect.min.x, rect.min.y + row * 0.2f}, {rect.min.x + line, rect.max.y - row * 0.2f}, element.accent);
                }
                DrawOption(context, element, rect, element.options[static_cast<std::size_t>(index)]);
            }
            if (element.borderWidth > 0.0f && element.border.a > 0.0f) {
                context.list.AddRoundedRectOutline(list.min, list.max, element.border, radius,
                                                   std::max(1.0f, element.borderWidth * context.scale));
            }
        }

        // Fond, puis un onglet par page (son nom), l'onglet affiché souligné ; la page se dessine ensuite par-dessus.
        void DrawTabs(const DrawContext& context, const Placed& placed) {
            const Element& element = *placed.element;
            const Rect& rect = placed.rect;
            DrawBox(context, rect, element.background, element);
            const int shown = GetShownTab(element);
            const Interaction* interaction = context.options.interaction;
            const bool tabsHovered = interaction && interaction->hovered == element.id;
            const float radius = element.radius * context.scale;
            for (int index = 0; index < static_cast<int>(element.children.size()); ++index) {
                const Rect tab = GetTabRect(element, rect, context.scale, index);
                const bool hovered = tabsHovered && interaction->hoveredTab == index;
                const LinearColor fill = index == shown ? element.pressedColor : hovered ? element.hoverColor : element.titleColor;
                if (fill.a > 0.0f) {
                    context.list.AddRoundedRect(tab.min, tab.max, fill, radius);
                }
                if (index == shown && element.accent.a > 0.0f) {
                    const float line = std::max(2.0f, 3.0f * context.scale);
                    context.list.AddRect({tab.min.x + radius, tab.max.y - line}, {tab.max.x - radius, tab.max.y}, element.accent);
                }
                const Rect clip = Intersect(placed.clip, tab);
                if (IsEmpty(clip)) {
                    continue;
                }
                const std::string& name = element.children[static_cast<std::size_t>(index)].name;
                const float pixelSize = element.fontSize * context.scale;
                const Vector2f size = context.list.MeasureText(name, pixelSize);
                context.list.SetClipRect(clip.min, clip.max);
                context.list.AddText({0.5f * (tab.min.x + tab.max.x - size.x), 0.5f * (tab.min.y + tab.max.y - size.y)}, name,
                                     pixelSize, element.textColor);
                context.list.SetClipRect(placed.clip.min, placed.clip.max);
            }
        }

        // Fond, puis l'ascenseur s'il y a du contenu caché (les enfants, découpés à gauche de lui, ne le couvrent pas).
        void DrawScrollArea(const DrawContext& context, const Placed& placed) {
            const Element& element = *placed.element;
            DrawBox(context, placed.rect, element.background, element);
            const std::optional<Rect> track = GetScrollbarTrack(placed, context.scale);
            const std::optional<Rect> thumb = GetScrollbarThumb(placed, context.scale);
            if (!track || !thumb) {
                return;
            }
            const float width = track->max.x - track->min.x;
            const float inset = width * 0.2f;
            if (element.titleColor.a > 0.0f) {
                context.list.AddRoundedRect(track->min, track->max, element.titleColor, width * 0.5f);
            }
            const Interaction* interaction = context.options.interaction;
            const bool active = interaction
                             && (interaction->thumbHovered == element.id
                                 || (interaction->thumbPressed && interaction->pressed == element.id));
            context.list.AddRoundedRect(thumb->min + Vector2f{inset, inset}, thumb->max - Vector2f{inset, inset},
                                        active ? element.hoverColor : element.accent, (width - 2.0f * inset) * 0.5f);
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
                case ElementType::Tabs:
                    DrawTabs(context, placed);
                    break;
                case ElementType::ProgressBar:
                    DrawProgressBar(context, element, rect);
                    break;
                case ElementType::ScrollArea:
                    DrawScrollArea(context, placed);
                    break;
                case ElementType::Dropdown:
                    DrawDropdown(context, placed, hovered);
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

    std::span<const std::string_view> GetActionNames() {
        return ActionNames;
    }

    bool TargetsWindow(ButtonAction action) {
        return action == ButtonAction::OpenWindow || action == ButtonAction::CloseWindow || action == ButtonAction::ToggleWindow;
    }

    std::span<const Field> GetFields() {
        return Fields;
    }

    bool IsContainer(ElementType type) {
        return type == ElementType::Window || type == ElementType::Frame || type == ElementType::Tabs
            || type == ElementType::ScrollArea;
    }

    bool TargetsTab(ButtonAction action) {
        return action == ButtonAction::ShowTab;
    }

    int GetDropdownChoice(const Element& dropdown) {
        if (dropdown.options.empty() || !std::isfinite(dropdown.value)) {
            return -1;
        }
        return std::clamp(static_cast<int>(std::lround(dropdown.value)), 0, static_cast<int>(dropdown.options.size()) - 1);
    }

    std::string GetShownText(const Element& element) {
        if (!element.boundText) {
            return element.text;
        }
        const std::size_t slot = element.text.find("{}");
        if (slot == std::string::npos) {
            return *element.boundText;
        }
        std::string shown = element.text;
        shown.replace(slot, 2, *element.boundText);
        return shown;
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

    Element* FindByName(Document& document, std::string_view name) {
        return const_cast<Element*>(FindByName(std::as_const(document), name));
    }

    const Element* FindByName(const Document& document, std::string_view name) {
        const auto search = [name](auto&& self, const std::vector<Element>& elements) -> const Element* {
            for (const Element& element : elements) {
                if (element.name == name) {
                    return &element;
                }
                if (const Element* found = self(self, element.children)) {
                    return found;
                }
            }
            return nullptr;
        };
        return name.empty() ? nullptr : search(search, document.roots);
    }

    bool SetWindowOpen(Document& document, std::string_view name, bool open) {
        Element* element = FindByName(document, name);
        if (!element) {
            return false;
        }
        element->visible = open;
        if (open) {
            BringToFront(document, element->id);
        }
        return true;
    }

    bool ShowTab(Document& document, std::string_view pageName) {
        Element* page = FindByName(document, pageName);
        Element* tabs = page ? FindParent(document, page->id) : nullptr;
        if (!tabs || tabs->type != ElementType::Tabs) {
            return false;
        }
        for (Element& other : tabs->children) {
            other.visible = &other == page;
        }
        return true;
    }

    int GetShownTab(const Element& tabs) {
        if (tabs.children.empty()) {
            return -1;
        }
        const auto visible = std::ranges::find_if(tabs.children, [](const Element& page) { return page.visible; });
        return visible == tabs.children.end() ? 0 : static_cast<int>(visible - tabs.children.begin());
    }

    bool IsPausingGame(const Document& document) {
        const auto search = [](auto&& self, const std::vector<Element>& elements) -> bool {
            return std::ranges::any_of(elements, [&self](const Element& element) {
                return element.visible
                    && ((element.type == ElementType::Window && element.pausesGame) || self(self, element.children));
            });
        };
        return search(search, document.roots);
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

    Rect GetTabBar(const Element& tabs, const Rect& rect, float scale) {
        return {rect.min, {rect.max.x, std::min(rect.max.y, rect.min.y + tabs.titleHeight * scale)}};
    }

    Rect GetTabRect(const Element& tabs, const Rect& rect, float scale, int index) {
        const Rect bar = GetTabBar(tabs, rect, scale);
        const int count = std::max(1, static_cast<int>(tabs.children.size()));
        const float gap = 4.0f * scale; // entre deux onglets
        const float width = std::max(0.0f, (bar.max.x - bar.min.x - gap * static_cast<float>(count - 1)) / static_cast<float>(count));
        const float x = bar.min.x + (width + gap) * static_cast<float>(index);
        return {{x, bar.min.y}, {x + width, bar.max.y}};
    }

    Rect GetTabContent(const Element& tabs, const Rect& rect, float scale) {
        return {{rect.min.x, GetTabBar(tabs, rect, scale).max.y}, rect.max};
    }

    int FindTabAt(const Element& tabs, const Rect& rect, float scale, Vector2f point) {
        if (tabs.type != ElementType::Tabs || !GetTabBar(tabs, rect, scale).Contains(point)) {
            return -1;
        }
        for (int index = 0; index < static_cast<int>(tabs.children.size()); ++index) {
            if (GetTabRect(tabs, rect, scale, index).Contains(point)) {
                return index;
            }
        }
        return -1;
    }

    Vector2f AnchorPoint(Anchor anchor, const Rect& parent) {
        if (anchor == Anchor::Stretch) {
            return parent.min;
        }
        const Vector2f fraction = Fraction(anchor);
        const Vector2f size = parent.max - parent.min;
        return parent.min + Vector2f{size.x * fraction.x, size.y * fraction.y};
    }

    std::vector<Placed> Arrange(const Document& document, Vector2f viewMin, Vector2f viewMax, const std::optional<Rect>& clip,
                                std::uint32_t reveal) {
        std::vector<Placed> placed;
        const float scale = GetScale(viewMin, viewMax);
        if (scale <= 0.0f || viewMax.x <= viewMin.x) {
            return placed;
        }
        const Rect view{viewMin, viewMax};
        ArrangeChildren(document.roots, view, clip ? Intersect(view, *clip) : view, 0, 0, scale, reveal, placed);
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

    float GetScrollOffset(const Placed& area, float scale) {
        return std::clamp(area.element->scroll * scale, 0.0f, std::max(area.scrollRange, 0.0f));
    }

    std::optional<Rect> GetScrollbarTrack(const Placed& area, float scale) {
        if (area.element->type != ElementType::ScrollArea || area.scrollRange <= 0.5f) {
            return std::nullopt;
        }
        const Rect& rect = area.rect;
        const float width = std::min(area.element->scrollbarWidth * scale, rect.max.x - rect.min.x);
        return Rect{{rect.max.x - width, rect.min.y}, rect.max};
    }

    std::optional<Rect> GetScrollbarThumb(const Placed& area, float scale) {
        const std::optional<Rect> track = GetScrollbarTrack(area, scale);
        if (!track) {
            return std::nullopt;
        }
        const float height = track->max.y - track->min.y;
        const float thumb = std::clamp(height * height / (height + area.scrollRange), std::min(height, MinThumb * scale), height);
        const float top = track->min.y + (height - thumb) * GetScrollOffset(area, scale) / area.scrollRange;
        return Rect{{track->min.x, top}, {track->max.x, top + thumb}};
    }

    std::uint32_t ScrollAt(Document& document, std::span<const Placed> placed, Vector2f point, float notches, float scale) {
        if (notches == 0.0f || scale <= 0.0f) {
            return 0;
        }
        for (auto item = placed.rbegin(); item != placed.rend(); ++item) {
            if (item->element->type != ElementType::ScrollArea || item->scrollRange <= 0.5f
                || !Intersect(item->rect, item->clip).Contains(point)) {
                continue;
            }
            Element* area = Find(document, item->element->id);
            if (!area) {
                return 0;
            }
            const float range = item->scrollRange / scale;
            area->scroll = std::clamp(std::clamp(area->scroll, 0.0f, range) - notches * WheelStep, 0.0f, range);
            return area->id;
        }
        return 0;
    }

    void Update(Document& document, Interaction& interaction, Vector2f viewMin, Vector2f viewMax, const Pointer& pointer) {
        interaction.clicked = 0;
        interaction.changed = 0;
        interaction.action = ButtonAction::None;
        interaction.target.clear();
        interaction.escapeUsed = false;
        const std::vector<Placed> placed = Arrange(document, viewMin, viewMax);
        const float scale = GetScale(viewMin, viewMax);
        // Liste déroulante dépliée : elle retient la souris et Échap tant qu'elle l'est. Un clic sur un choix le
        // prend, un clic ailleurs la replie (sans toucher ce qui est dessous), Échap aussi.
        interaction.hoveredOption = -1;
        if (interaction.openList != 0) {
            const Placed* field = FindPlaced(placed, interaction.openList);
            Element* dropdown = field ? Find(document, interaction.openList) : nullptr;
            if (!dropdown || dropdown->options.empty()) {
                interaction.openList = 0; // sa fenêtre a été fermée, ou ses choix vidés
            } else {
                const Rect list = DropdownList(*dropdown, field->rect, {viewMin, viewMax});
                if (pointer.enabled) {
                    interaction.hoveredOption = DropdownOptionAt(*dropdown, list, pointer.position);
                }
                if (pointer.escape) {
                    interaction.openList = 0;
                    interaction.escapeUsed = true;
                } else if (pointer.pressed) {
                    if (interaction.hoveredOption >= 0 && static_cast<float>(interaction.hoveredOption) != dropdown->value) {
                        dropdown->value = static_cast<float>(interaction.hoveredOption);
                        interaction.changed = dropdown->id;
                    }
                    interaction.openList = 0;
                }
                interaction.hovered = 0;
                interaction.pressed = 0;
                interaction.closeHovered = 0;
                interaction.closePressed = false;
                interaction.hoveredTab = -1;
                interaction.pressedTab = -1;
                interaction.thumbHovered = 0;
                interaction.thumbPressed = false;
                interaction.wantsMouse = true;
                interaction.pausesGame = IsPausingGame(document);
                return;
            }
        }

        // Fenêtres à mettre devant les autres, à la fin (réordonner les racines déplace les éléments) ; la dernière
        // finit devant.
        std::vector<std::uint32_t> raise;

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
        const std::optional<Rect> closeBox = hit ? CloseButtonRect(*hit->element, hit->rect, scale) : std::nullopt;
        interaction.closeHovered = closeBox && closeBox->Contains(pointer.position) ? hitId : 0;
        interaction.hoveredTab = hit ? FindTabAt(*hit->element, hit->rect, scale, pointer.position) : -1;
        if (interaction.hoveredTab >= 0) {
            interaction.hovered = hitId;
        }
        const std::optional<Rect> track = hit ? GetScrollbarTrack(*hit, scale) : std::nullopt;
        const std::optional<Rect> thumb = hit ? GetScrollbarThumb(*hit, scale) : std::nullopt;
        const bool onTrack = track && track->Contains(pointer.position);
        interaction.thumbHovered = thumb && thumb->Contains(pointer.position) ? hitId : 0;

        if (pointer.pressed && hit) {
            const Element& element = *hit->element;
            const bool onClose = interaction.closeHovered == hitId;
            const bool titleBar = !onClose && element.type == ElementType::Window && element.movable
                               && element.anchor != Anchor::Stretch
                               && pointer.position.y < hit->rect.min.y + element.titleHeight * scale;
            const bool onThumb = interaction.thumbHovered == hitId;
            interaction.pressed = IsInteractive(element.type) || titleBar || onClose || interaction.hoveredTab >= 0 || onThumb
                                    ? hitId
                                    : 0;
            interaction.closePressed = onClose;
            interaction.thumbPressed = onThumb;
            interaction.pressedTab = interaction.hoveredTab;
            interaction.dragStart = pointer.position;
            interaction.dragOrigin = onThumb ? Vector2f{0.0f, element.scroll} : element.position;
            if (onTrack && !onThumb && thumb && scale > 0.0f) {
                // Clic sur la piste : une page (presque une hauteur de zone) vers le clic.
                Element& area = *Find(document, hitId);
                const float page = 0.9f * (hit->rect.max.y - hit->rect.min.y) / scale;
                const float range = hit->scrollRange / scale;
                area.scroll = std::clamp(std::clamp(area.scroll, 0.0f, range) + (pointer.position.y < thumb->min.y ? -page : page),
                                         0.0f, range);
            }
            raise.push_back(RootOf(document, hitId)); // un clic dans une fenêtre la met devant
        }

        Element* pressed = interaction.pressed != 0 ? Find(document, interaction.pressed) : nullptr;
        if (pressed && pressed->visible) {
            if (pressed->type == ElementType::Window && !interaction.closePressed && scale > 0.0f) {
                // Souris bornée à la vue : la barre de titre ne peut pas sortir de l'écran.
                const Vector2f mouse{std::clamp(pointer.position.x, viewMin.x, viewMax.x),
                                     std::clamp(pointer.position.y, viewMin.y, viewMax.y)};
                pressed->position = interaction.dragOrigin + (mouse - interaction.dragStart) * (1.0f / scale);
            } else if (pressed->type == ElementType::ScrollArea && interaction.thumbPressed) {
                // Poignée tenue : le contenu suit la souris, à l'échelle de la piste.
                const Placed* area = FindPlaced(placed, pressed->id);
                const std::optional<Rect> areaTrack = area ? GetScrollbarTrack(*area, scale) : std::nullopt;
                const std::optional<Rect> areaThumb = area ? GetScrollbarThumb(*area, scale) : std::nullopt;
                if (areaTrack && areaThumb && scale > 0.0f) {
                    const float free = (areaTrack->max.y - areaTrack->min.y) - (areaThumb->max.y - areaThumb->min.y);
                    const float range = area->scrollRange / scale;
                    const float moved = free > 0.0f ? (pointer.position.y - interaction.dragStart.y) / free * range : 0.0f;
                    pressed->scroll = std::clamp(interaction.dragOrigin.y + moved, 0.0f, range);
                }
            } else if (pressed->type == ElementType::Slider) {
                if (const Placed* slider = FindPlaced(placed, pressed->id)) {
                    const auto [start, end] = SliderTrack(slider->rect);
                    const float t = end > start ? std::clamp((pointer.position.x - start) / (end - start), 0.0f, 1.0f) : 0.0f;
                    const float value = pressed->minimum + (pressed->maximum - pressed->minimum) * t;
                    if (value != pressed->value) {
                        pressed->value = value;
                        interaction.changed = pressed->id;
                    }
                }
            }
            if (pointer.released && hitId == pressed->id) {
                if (pressed->type == ElementType::Window && interaction.closePressed) {
                    if (interaction.closeHovered == pressed->id) {
                        pressed->visible = false;
                    }
                } else if (pressed->type == ElementType::Button) {
                    interaction.clicked = pressed->id;
                    RunAction(document, *pressed, interaction, raise);
                } else if (pressed->type == ElementType::Checkbox) {
                    pressed->checked = !pressed->checked;
                    interaction.changed = pressed->id;
                } else if (pressed->type == ElementType::Dropdown && !pressed->options.empty()) {
                    interaction.openList = pressed->id;
                } else if (pressed->type == ElementType::Tabs && interaction.pressedTab >= 0
                           && interaction.pressedTab == interaction.hoveredTab
                           && interaction.pressedTab < static_cast<int>(pressed->children.size())) {
                    const std::size_t chosen = static_cast<std::size_t>(interaction.pressedTab);
                    for (std::size_t index = 0; index < pressed->children.size(); ++index) {
                        pressed->children[index].visible = index == chosen;
                    }
                }
            }
        }
        if (!pressed || !pressed->visible || pointer.released || !pointer.down) {
            interaction.pressed = 0;
            interaction.closePressed = false;
            interaction.thumbPressed = false;
            interaction.pressedTab = -1;
        }
        if (pointer.enabled && pointer.wheel != 0.0f) {
            ScrollAt(document, placed, pointer.position, pointer.wheel, scale);
        }

        if (pointer.escape) {
            // Pile des fenêtres : Échap ferme la plus en avant qui l'accepte ; s'il n'y en a pas, ouvre le menu
            // prévu pour (pause).
            const auto open = std::ranges::find_if(document.roots | std::views::reverse, [](const Element& element) {
                return element.type == ElementType::Window && element.visible && element.closeOnEscape;
            });
            if (open != (document.roots | std::views::reverse).end()) {
                open->visible = false;
                interaction.escapeUsed = true;
            } else {
                const auto menu = std::ranges::find_if(document.roots, [](const Element& element) {
                    return element.type == ElementType::Window && !element.visible && element.openOnEscape;
                });
                if (menu != document.roots.end()) {
                    menu->visible = true;
                    raise.push_back(menu->id);
                    interaction.escapeUsed = true;
                }
            }
        }

        for (const std::uint32_t id : raise) {
            BringToFront(document, id);
        }
        interaction.wantsMouse = hit != nullptr || interaction.pressed != 0;
        interaction.pausesGame = IsPausingGame(document);
    }

    void Draw(const Document& document, UI::DrawList& list, Vector2f viewMin, Vector2f viewMax, const DrawOptions& options) {
        const std::vector<Placed> placed = Arrange(document, viewMin, viewMax, options.clip, options.reveal);
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
        // Liste déroulante dépliée : par-dessus tout, découpée à la vue seulement (elle déborde de sa fenêtre).
        if (options.interaction && options.interaction->openList != 0) {
            const Placed* field = FindPlaced(placed, options.interaction->openList);
            if (field && !field->element->options.empty()) {
                const Rect view = options.clip ? Intersect({viewMin, viewMax}, *options.clip) : Rect{viewMin, viewMax};
                list.SetClipRect(view.min, view.max);
                DrawDropdownList(context, *field->element, DropdownList(*field->element, field->rect, {viewMin, viewMax}));
            }
        }
        list.ResetClipRect();
    }
}
