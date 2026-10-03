// Éditeur d'interface : l'écran du jeu au centre, où l'on sélectionne, glisse et redimensionne les éléments.
#include "Interface/InterfaceEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/DrawList.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace FluxEditor {
    using namespace Flux;
    using Math::LinearColor;
    using Math::Vector2f;

    namespace {
        struct Resolution
        {
            std::string_view name;
            Vector2f size;
        };
        constexpr std::array Resolutions{
            Resolution{"1920 × 1080 (16:9)", {1920.0f, 1080.0f}}, Resolution{"1280 × 720 (16:9)", {1280.0f, 720.0f}},
            Resolution{"2560 × 1440 (16:9)", {2560.0f, 1440.0f}}, Resolution{"3840 × 2160 (4K)", {3840.0f, 2160.0f}},
            Resolution{"2560 × 1080 (21:9)", {2560.0f, 1080.0f}}, Resolution{"1920 × 1200 (16:10)", {1920.0f, 1200.0f}},
            Resolution{"1440 × 1080 (4:3)", {1440.0f, 1080.0f}},
        };
        constexpr auto ResolutionNames = [] {
            std::array<std::string_view, Resolutions.size()> names{};
            for (std::size_t i = 0; i < Resolutions.size(); ++i) {
                names[i] = Resolutions[i].name;
            }
            return names;
        }();

        constexpr float Margin = 28.0f;      // autour de l'écran ajusté à la vue
        constexpr float HandleSize = 8.0f;   // poignées dessinées
        constexpr float HandleGrab = 12.0f;  // zone où on les saisit
        constexpr float DragThreshold = 3.0f;

        const LinearColor SelectionColor = LinearColor::FromSrgb8(255, 165, 40);

        // Poignées dans le sens des aiguilles d'une montre depuis le coin haut-gauche.
        Vector2f HandlePoint(const Interface::Rect& rect, int handle) {
            const Vector2f center = (rect.min + rect.max) * 0.5f;
            switch (handle) {
                case 0: return rect.min;
                case 1: return {center.x, rect.min.y};
                case 2: return {rect.max.x, rect.min.y};
                case 3: return {rect.max.x, center.y};
                case 4: return rect.max;
                case 5: return {center.x, rect.max.y};
                case 6: return {rect.min.x, rect.max.y};
                default: return {rect.min.x, center.y};
            }
        }

        int HandleAt(const Interface::Rect& rect, Vector2f point) {
            for (int handle = 0; handle < 8; ++handle) {
                const Vector2f center = HandlePoint(rect, handle);
                if (std::abs(point.x - center.x) <= HandleGrab * 0.5f && std::abs(point.y - center.y) <= HandleGrab * 0.5f) {
                    return handle;
                }
            }
            return -1;
        }

        MouseCursor HandleCursor(int handle) {
            switch (handle) {
                case 0:
                case 4: return MouseCursor::ResizeNwse;
                case 2:
                case 6: return MouseCursor::ResizeNesw;
                case 1:
                case 5: return MouseCursor::ResizeVertical;
                default: return MouseCursor::ResizeHorizontal;
            }
        }

        const Interface::Placed* FindPlaced(const std::vector<Interface::Placed>& placed, std::uint32_t id) {
            if (id == 0) {
                return nullptr;
            }
            const auto found = std::ranges::find_if(placed, [id](const Interface::Placed& item) { return item.element->id == id; });
            return found == placed.end() ? nullptr : &*found;
        }

        LinearColor Faded(LinearColor color, float alpha) {
            color.a *= alpha;
            return color;
        }
    }

    std::span<const std::string_view> InterfaceEditor::GetResolutionNames() {
        return ResolutionNames;
    }

    Vector2f InterfaceEditor::GetResolution(int index) {
        return Resolutions[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(Resolutions.size()) - 1))].size;
    }

    Interface::Rect InterfaceEditor::GetScreenRect(const OpenInterface& open, const UI::Rect& view) const {
        const Vector2f resolution = GetResolution(open.resolution);
        const Vector2f room = view.max - view.min - Vector2f{2.0f * Margin, 2.0f * Margin};
        const float fit = std::max(0.01f, std::min(room.x / resolution.x, room.y / resolution.y));
        const Vector2f size = resolution * (fit * open.zoom);
        const Vector2f center = (view.min + view.max) * 0.5f + open.pan;
        return {center - size * 0.5f, center + size * 0.5f};
    }

    void InterfaceEditor::UpdateCanvas(Engine& engine, const UI::Rect& view) {
        OpenInterface& open = *GetActive();
        UI::Context& ui = engine.GetUi();
        const UI::Style& style = ui.GetStyle();
        const Input& input = engine.GetWindow().GetInput();
        const Interface::Rect area{view.min, view.max};
        if (area.max.x - area.min.x < 1.0f || area.max.y - area.min.y < 1.0f) {
            m_drag = {};
            return; // vue écrasée par les panneaux
        }
        const Vector2f mouse = input.GetMousePosition();
        const bool mouseFree = !ui.WantsMouse() && area.Contains(mouse);

        // Molette : zoom autour de la souris. Clic droit ou du milieu maintenu : déplacer la vue.
        if (const float wheel = input.GetWheelDelta(); mouseFree && wheel != 0.0f) {
            const float zoom = std::clamp(open.zoom * std::pow(1.15f, wheel), 0.1f, 10.0f);
            const Vector2f viewCenter = (view.min + view.max) * 0.5f;
            const Vector2f center = viewCenter + open.pan;
            const Vector2f moved = mouse + (center - mouse) * (zoom / open.zoom);
            open.pan = moved - viewCenter;
            open.zoom = zoom;
        }
        if (mouseFree && m_drag.kind == Drag::Kind::None
            && (input.WasMouseButtonPressed(MouseButton::Right) || input.WasMouseButtonPressed(MouseButton::Middle))) {
            m_drag = {.kind = Drag::Kind::Pan, .startMouse = mouse, .startPan = open.pan};
        }
        if (m_drag.kind == Drag::Kind::Pan) {
            if (input.IsMouseButtonDown(MouseButton::Right) || input.IsMouseButtonDown(MouseButton::Middle)) {
                open.pan = m_drag.startPan + (mouse - m_drag.startMouse);
            } else {
                m_drag = {};
            }
        }

        const Interface::Rect screen = GetScreenRect(open, view);
        const float scale = Interface::GetScale(screen.min, screen.max);
        m_screen = screen;
        m_scale = scale;
        std::vector<Interface::Placed> placed = Interface::Arrange(open.document, screen.min, screen.max, area, open.selection);
        const Interface::Placed* selected = FindPlaced(placed, open.selection);

        // Clic : une poignée de l'élément sélectionné, sinon l'élément le plus en avant sous la souris.
        const int handle = selected && mouseFree && m_drag.kind == Drag::Kind::None ? HandleAt(selected->rect, mouse) : -1;
        if (mouseFree && m_drag.kind == Drag::Kind::None && input.WasMouseButtonPressed(MouseButton::Left)) {
            if (handle >= 0) {
                m_drag = {.kind = Drag::Kind::Resize, .handle = handle, .startMouse = mouse, .startRect = selected->rect,
                          .parentRect = selected->parent};
            } else {
                open.selection = Interface::FindAt(placed, mouse);
                if (const Interface::Placed* hit = FindPlaced(placed, open.selection)) {
                    m_drag = {.kind = Drag::Kind::Pending, .startMouse = mouse, .startRect = hit->rect, .parentRect = hit->parent};
                }
            }
        }

        if (m_drag.kind == Drag::Kind::Pending || m_drag.kind == Drag::Kind::Move || m_drag.kind == Drag::Kind::Resize) {
            Interface::Element* element = GetSelected();
            if (!element || !input.IsMouseButtonDown(MouseButton::Left)) {
                m_drag = {};
            } else {
                Vector2f delta = mouse - m_drag.startMouse;
                if (m_drag.kind == Drag::Kind::Pending && std::abs(delta.x) + std::abs(delta.y) > DragThreshold) {
                    m_drag.kind = Drag::Kind::Move;
                }
                if (m_drag.kind != Drag::Kind::Pending && scale > 0.0f) {
                    // Bords calés sur des pixels de référence entiers (Ctrl : sur des multiples de 10), comptés depuis
                    // l'ancre : un élément centré reste centré, un élément collé à droite y reste collé.
                    const float step = (input.IsControlDown() ? 10.0f : 1.0f) * scale;
                    const bool stretch = element->anchor == Interface::Anchor::Stretch;
                    const Vector2f anchor = Interface::AnchorPoint(element->anchor, m_drag.parentRect);
                    const Vector2f minOrigin = stretch ? m_drag.parentRect.min : anchor;
                    const Vector2f maxOrigin = stretch ? m_drag.parentRect.max : anchor;
                    const auto snap = [step](float value, float origin) {
                        return origin + std::round((value - origin) / step) * step;
                    };
                    Interface::Rect rect = m_drag.startRect;
                    const Vector2f oldSize = element->size;
                    if (m_drag.kind == Drag::Kind::Move) {
                        if (input.IsShiftDown()) {
                            // Maj : seulement le long de l'axe où la souris a le plus bougé.
                            (std::abs(delta.x) > std::abs(delta.y) ? delta.y : delta.x) = 0.0f;
                        }
                        const Vector2f size = rect.max - rect.min;
                        rect.min = {snap(rect.min.x + delta.x, minOrigin.x), snap(rect.min.y + delta.y, minOrigin.y)};
                        rect.max = rect.min + size;
                    } else {
                        const int h = m_drag.handle;
                        const float minimum = std::max(1.0f, 4.0f * scale);
                        if (h == 0 || h == 6 || h == 7) {
                            rect.min.x = std::min(snap(rect.min.x + delta.x, minOrigin.x), rect.max.x - minimum);
                        }
                        if (h == 2 || h == 3 || h == 4) {
                            rect.max.x = std::max(snap(rect.max.x + delta.x, maxOrigin.x), rect.min.x + minimum);
                        }
                        if (h == 0 || h == 1 || h == 2) {
                            rect.min.y = std::min(snap(rect.min.y + delta.y, minOrigin.y), rect.max.y - minimum);
                        }
                        if (h == 4 || h == 5 || h == 6) {
                            rect.max.y = std::max(snap(rect.max.y + delta.y, maxOrigin.y), rect.min.y + minimum);
                        }
                    }
                    const Vector2f oldPosition = element->position;
                    Interface::SetRect(*element, m_drag.parentRect, scale, rect);
                    // Valeurs rondes dans le panneau Propriétés (un demi-pixel d'écart au plus).
                    element->position = {std::round(element->position.x), std::round(element->position.y)};
                    element->size = m_drag.kind == Drag::Kind::Move
                                      ? oldSize
                                      : Vector2f{std::round(element->size.x), std::round(element->size.y)};
                    if (element->position != oldPosition || element->size != oldSize) {
                        MarkChanged();
                        placed = Interface::Arrange(open.document, screen.min, screen.max, area, open.selection);
                    }
                }
            }
        }

        const std::uint32_t hovered = m_drag.kind == Drag::Kind::None && mouseFree ? Interface::FindAt(placed, mouse) : 0;
        if (m_drag.kind == Drag::Kind::Resize) {
            ui.SetMouseCursor(HandleCursor(m_drag.handle));
        } else if (handle >= 0) {
            ui.SetMouseCursor(HandleCursor(handle));
        } else if (m_drag.kind == Drag::Kind::Move || (hovered != 0 && hovered == open.selection)) {
            ui.SetMouseCursor(MouseCursor::Move);
        }

        // Dessin : le fond de la vue, l'écran du jeu (une ombre, un fond qui tient lieu de scène 3D), l'interface
        // comme dans le jeu, puis le cadre de l'écran et la sélection.
        UI::DrawList& list = engine.GetRenderer().GetOverlay();
        list.SetClipRect(area.min, area.max);
        list.AddRect(area.min, area.max, LinearColor::FromSrgb8(20, 22, 27));
        list.AddShadow(screen.min, screen.max, LinearColor{0.0f, 0.0f, 0.0f, 0.6f}, 0.0f, 16.0f);
        list.AddRect(screen.min, screen.max, LinearColor::FromSrgb8(38, 46, 60));
        Interface::Draw(open.document, list, screen.min, screen.max,
                        {.textures = [&engine](std::string_view path) { return engine.LoadAssetTexture(path); },
                         .placeholders = true,
                         .clip = area,
                         .reveal = open.selection});
        list.SetClipRect(area.min, area.max);
        list.AddRectOutline(screen.min - Vector2f{1.0f, 1.0f}, screen.max + Vector2f{1.0f, 1.0f}, style.separator, 1.0f);
        const float labelSize = std::round(style.fontSize * 0.85f);
        if (screen.min.y - labelSize - 6.0f >= area.min.y) {
            list.AddText({screen.min.x, screen.min.y - labelSize - 6.0f},
                         std::format("Écran {} — {:.0f} %", GetResolutionNames()[static_cast<std::size_t>(open.resolution)],
                                     open.zoom * 100.0f),
                         labelSize, style.textDisabled);
        }
        if (open.document.roots.empty()) {
            constexpr std::string_view Hint = "Interface vide : ajoutez une fenêtre depuis le panneau « Interface ».";
            const Vector2f size = list.MeasureText(Hint, style.fontSize);
            list.AddText((screen.min + screen.max) * 0.5f - size * 0.5f, Hint, style.fontSize, style.textDisabled);
        }
        DrawSelection(engine, placed, view, hovered);
        list.ResetClipRect();
    }

    void InterfaceEditor::DrawSelection(Engine& engine, const std::vector<Interface::Placed>& placed, const UI::Rect& view,
                                        std::uint32_t hovered) {
        const OpenInterface& open = *GetActive();
        UI::DrawList& list = engine.GetRenderer().GetOverlay();
        const UI::Style& style = engine.GetUi().GetStyle();
        if (hovered != open.selection) {
            if (const Interface::Placed* item = FindPlaced(placed, hovered)) {
                list.AddRectOutline(item->rect.min, item->rect.max, Faded(SelectionColor, 0.55f), 1.0f);
            }
        }
        const Interface::Placed* selected = FindPlaced(placed, open.selection);
        if (!selected) {
            return;
        }
        const Interface::Element& element = *selected->element;
        const Interface::Rect& rect = selected->rect;

        // Le parent (s'il n'est pas l'écran) et l'ancre : d'où l'élément est placé.
        if (selected->parentId != 0) {
            list.AddRectOutline(selected->parent.min, selected->parent.max, LinearColor{1.0f, 1.0f, 1.0f, 0.25f}, 1.0f);
        }
        if (element.anchor == Interface::Anchor::Stretch) {
            // Les quatre marges : un trait du bord du parent au bord de l'élément.
            const Interface::Rect& parent = selected->parent;
            const Vector2f center = (rect.min + rect.max) * 0.5f;
            const LinearColor color = Faded(SelectionColor, 0.7f);
            list.AddLine({parent.min.x, center.y}, {rect.min.x, center.y}, color, 1.0f);
            list.AddLine({rect.max.x, center.y}, {parent.max.x, center.y}, color, 1.0f);
            list.AddLine({center.x, parent.min.y}, {center.x, rect.min.y}, color, 1.0f);
            list.AddLine({center.x, rect.max.y}, {center.x, parent.max.y}, color, 1.0f);
        } else {
            const Vector2f anchor = Interface::AnchorPoint(element.anchor, selected->parent);
            const Vector2f pivot = Interface::AnchorPoint(element.anchor, rect);
            list.AddLine(anchor, pivot, Faded(SelectionColor, 0.7f), 1.0f);
            list.AddCircle(anchor, 5.0f, LinearColor::FromSrgb8(30, 30, 34));
            list.AddCircleOutline(anchor, 5.0f, SelectionColor, 1.5f);
        }

        list.AddRectOutline(rect.min - Vector2f{1.0f, 1.0f}, rect.max + Vector2f{1.0f, 1.0f}, SelectionColor, 1.5f);
        for (int handle = 0; handle < 8; ++handle) {
            const Vector2f center = HandlePoint(rect, handle);
            const Vector2f half{HandleSize * 0.5f, HandleSize * 0.5f};
            list.AddRoundedRect(center - half, center + half, LinearColor::FromSrgb8(245, 245, 245), 2.0f);
            list.AddRoundedRectOutline(center - half, center + half, SelectionColor, 2.0f, 1.5f);
        }

        // Taille en pixels de référence, sous l'élément (au-dessus s'il touche le bas de la vue).
        const std::string label = std::format("{:.0f} × {:.0f}", (rect.max.x - rect.min.x) / m_scale,
                                              (rect.max.y - rect.min.y) / m_scale);
        const float labelSize = std::round(style.fontSize * 0.8f);
        const Vector2f size = list.MeasureText(label, labelSize) + Vector2f{10.0f, 4.0f};
        Vector2f at{(rect.min.x + rect.max.x - size.x) * 0.5f, rect.max.y + HandleSize};
        if (at.y + size.y > view.max.y) {
            at.y = rect.min.y - HandleSize - size.y;
        }
        list.AddRoundedRect(at, at + size, SelectionColor, 3.0f);
        list.AddText(at + Vector2f{5.0f, 2.0f}, label, labelSize, LinearColor::FromSrgb8(20, 20, 20));
    }
}
