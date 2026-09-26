#include "Flux/UI/Context.hpp"

#include "Flux/Platform/Input.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Flux/UI/Font.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Flux::UI {
    namespace {
        using Math::LinearColor;
        using Math::Vector2f;

        constexpr WidgetId RootId = 14695981039346656037ull; // base FNV-1a

        // FNV-1a 64 bits : rapide, et suffisant pour quelques centaines de widgets par panneau.
        WidgetId Hash(WidgetId seed, const void* data, std::size_t size) {
            WidgetId hash = seed;
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < size; ++i) {
                hash = (hash ^ bytes[i]) * 1099511628211ull;
            }
            return hash == 0 ? 1 : hash; // 0 signifie « aucun widget »
        }

        WidgetId Hash(WidgetId seed, std::string_view text) {
            return Hash(seed, text.data(), text.size());
        }

        // "Position##soleil" s'affiche "Position" mais s'identifie par le libellé complet.
        std::string_view VisibleLabel(std::string_view label) {
            const std::size_t hidden = label.find("##");
            return hidden == std::string_view::npos ? label : label.substr(0, hidden);
        }

        bool Contains(Vector2f min, Vector2f max, Vector2f point) {
            return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
        }

        void AddLine(DrawList& list, Vector2f a, Vector2f b, float thickness, LinearColor color) {
            const Vector2f direction = b - a;
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
            if (length <= 0.0f) {
                return;
            }
            const float half = thickness * 0.5f / length;
            const Vector2f normal{-direction.y * half, direction.x * half};
            list.AddTriangle(a + normal, b + normal, b - normal, color);
            list.AddTriangle(a + normal, b - normal, a - normal, color);
        }

        bool IsNumberCharacter(char c) {
            return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-' || c == '+' || c == 'e' || c == 'E';
        }

        std::string FormatNumber(double value, int decimals) {
            std::string text = std::format("{:.{}f}", value, std::clamp(decimals, 0, 12));
            // "-0.000" : un zéro arrondi n'a pas de signe.
            if (text.starts_with('-') && text.find_first_not_of("-0.") == std::string::npos) {
                text.erase(0, 1);
            }
            return text;
        }

        // Pour l'édition : "9.000" devient "9", "0.250" devient "0.25".
        std::string FormatEditable(double value, int decimals) {
            std::string text = FormatNumber(value, decimals);
            if (text.find('.') != std::string::npos) {
                text.erase(text.find_last_not_of('0') + 1);
                if (text.ends_with('.')) {
                    text.pop_back();
                }
            }
            return text;
        }

        std::optional<double> ParseNumber(std::string text) {
            std::ranges::replace(text, ',', '.'); // clavier français
            if (text.starts_with('+')) {
                text.erase(0, 1);
            }
            double value = 0.0;
            const char* end = text.data() + text.size();
            const auto [last, error] = std::from_chars(text.data(), end, value);
            if (error != std::errc{} || last != end || !std::isfinite(value)) {
                return std::nullopt;
            }
            return value;
        }
    }

    Style Style::Dark() {
        using C = LinearColor;
        Style style;
        style.text = C::FromSrgb8(230, 230, 232);
        style.textDisabled = C::FromSrgb8(140, 142, 148);
        style.panelBackground = C::FromSrgb8(30, 31, 34, 0.94f);
        style.panelBorder = C::FromSrgb8(0, 0, 0, 0.6f);
        style.titleBackground = C::FromSrgb8(22, 23, 26);
        style.titleBackgroundActive = C::FromSrgb8(40, 64, 104);
        style.frame = C::FromSrgb8(50, 52, 57);
        style.frameHovered = C::FromSrgb8(64, 67, 74);
        style.frameActive = C::FromSrgb8(78, 82, 90);
        style.button = C::FromSrgb8(54, 80, 122);
        style.buttonHovered = C::FromSrgb8(70, 102, 152);
        style.buttonActive = C::FromSrgb8(44, 64, 98);
        style.checkMark = C::FromSrgb8(110, 170, 250);
        style.selection = C::FromSrgb8(60, 110, 190, 0.8f);
        style.separator = C::FromSrgb8(68, 70, 76);
        style.axisX = C::FromSrgb8(210, 70, 70);
        style.axisY = C::FromSrgb8(100, 180, 70);
        style.axisZ = C::FromSrgb8(70, 125, 220);
        return style;
    }

    struct Context::Impl {
        struct Panel {
            explicit Panel(const Font& font) : chrome(font), content(font) {}

            std::string title;
            Vector2f position;
            float width = 0.0f;
            float height = 0.0f;      // hauteur de la dernière frame : sert au survol de la frame suivante
            bool collapsed = false;
            std::uint64_t lastFrame = 0;
            DrawList chrome;          // fond, bordure et barre de titre, dessinés sous le contenu
            DrawList content;
        };

        struct Interaction {
            bool hovered = false;
            bool held = false;
            bool clicked = false;
        };

        Impl(const Font& uiFont, const Style& uiStyle) : font(uiFont), style(uiStyle) {}

        const Font& font;
        Style style;

        // Entrées de la frame.
        const Input* input = nullptr;
        Vector2f screenSize;
        Vector2f mouse;
        Vector2f previousMouse;
        Vector2f mouseDelta;
        bool mouseEnabled = false;
        bool mousePressed = false;
        bool mouseReleased = false;
        bool mouseDown = false;
        double time = 0.0;
        std::uint64_t frame = 0;

        // Panneaux, du plus lointain au plus proche dans order.
        std::unordered_map<WidgetId, std::unique_ptr<Panel>> panels;
        std::vector<WidgetId> order;
        WidgetId hoveredPanel = 0;

        // Panneau en cours de description et sa mise en page.
        Panel* panel = nullptr;
        WidgetId panelId = 0;
        Vector2f contentMin;
        float contentMaxX = 0.0f;
        float cursorY = 0.0f;
        Vector2f lastItemMin;
        Vector2f lastItemMax;
        bool sameLine = false;

        std::vector<WidgetId> idStack{RootId};

        // Widget sous le bouton gauche enfoncé (glissement, clic en cours).
        WidgetId activeId = 0;
        bool activeSeen = false;
        Vector2f pressPosition;
        Vector2f dragOrigin;
        bool dragging = false;

        // Champ numérique en cours de saisie.
        WidgetId editingId = 0;
        bool editingSeen = false;
        std::string editBuffer;
        std::size_t caret = 0;
        bool selectAll = false;
        double caretTime = 0.0;

        float LineHeight() const { return font.GetLineHeight() * style.fontSize; }
        float FrameHeight() const { return LineHeight() + 2.0f * style.framePadding.y; }
        float RowWidth() const { return contentMaxX - contentMin.x; }

        WidgetId GetId(std::string_view label) const { return Hash(idStack.back(), label); }

        Vector2f MeasureText(std::string_view text) const {
            return panel->content.MeasureText(text, style.fontSize);
        }

        // Réserve la place d'un widget et retourne son coin haut-gauche.
        Vector2f ItemRect(Vector2f size) {
            const Vector2f min = sameLine ? Vector2f{lastItemMax.x + style.itemSpacing, lastItemMin.y}
                                          : Vector2f{contentMin.x, cursorY};
            sameLine = false;
            lastItemMin = min;
            lastItemMax = min + size;
            cursorY = std::max(cursorY, lastItemMax.y + style.itemSpacing);
            return min;
        }

        void RestorePanelClip() {
            panel->content.SetClipRect({panel->position.x + 1.0f, panel->position.y + FrameHeight()},
                                       {panel->position.x + panel->width - 1.0f, screenSize.y + 1.0e4f});
        }

        // Texte découpé à [min.x, maxX] en plus du panneau.
        void ClippedText(Vector2f topLeft, float maxX, std::string_view text, LinearColor color) {
            const float panelMaxX = panel->position.x + panel->width - 1.0f;
            panel->content.SetClipRect({std::max(topLeft.x, panel->position.x + 1.0f), panel->position.y + FrameHeight()},
                                       {std::min(maxX, panelMaxX), screenSize.y + 1.0e4f});
            panel->content.AddText(topLeft, text, style.fontSize, color);
            RestorePanelClip();
        }

        Interaction Behavior(WidgetId id, Vector2f min, Vector2f max) {
            Interaction result;
            const bool inside = mouseEnabled && panel && !panel->collapsed && panelId == hoveredPanel
                             && Contains(min, max, mouse);
            result.hovered = inside && (activeId == 0 || activeId == id);
            if (result.hovered && mousePressed) {
                activeId = id;
                pressPosition = mouse;
                dragging = false;
            }
            if (activeId == id) {
                activeSeen = true;
                if (mouseReleased || !mouseDown) {
                    result.clicked = inside; // relâché sur le widget : clic ; ailleurs : abandon
                    activeId = 0;
                } else {
                    result.held = true;
                }
            }
            return result;
        }

        // Libellé à gauche d'une ligne de champ ; retourne le rectangle restant pour le champ.
        std::pair<Vector2f, Vector2f> LabeledRow(std::string_view label) {
            const Vector2f min = ItemRect({RowWidth(), FrameHeight()});
            const float labelWidth = std::floor(RowWidth() * style.labelFraction);
            ClippedText({min.x, min.y + style.framePadding.y}, min.x + labelWidth - style.itemSpacing,
                        VisibleLabel(label), style.text);
            return {{min.x + labelWidth, min.y}, {min.x + RowWidth(), min.y + FrameHeight()}};
        }

        void EditField(Vector2f min, Vector2f max, double& value, double low, double high, bool& changed) {
            editingSeen = true;
            bool commit = false;
            bool cancel = false;
            bool typed = false;

            const auto clearIfSelected = [&] {
                if (selectAll) {
                    editBuffer.clear();
                    caret = 0;
                    selectAll = false;
                }
            };
            for (const char c : input->GetTextInput()) {
                if (IsNumberCharacter(c)) {
                    clearIfSelected();
                    editBuffer.insert(caret++, 1, c);
                    typed = true;
                }
            }
            if (input->WasKeyTyped(Key::Backspace)) {
                if (selectAll) {
                    clearIfSelected();
                } else if (caret > 0) {
                    editBuffer.erase(--caret, 1);
                }
                typed = true;
            }
            if (input->WasKeyTyped(Key::Delete)) {
                if (selectAll) {
                    clearIfSelected();
                } else if (caret < editBuffer.size()) {
                    editBuffer.erase(caret, 1);
                }
                typed = true;
            }
            if (input->WasKeyTyped(Key::Left)) {
                caret = selectAll ? 0 : caret - (caret > 0 ? 1 : 0);
                selectAll = false;
                typed = true;
            }
            if (input->WasKeyTyped(Key::Right)) {
                caret = selectAll ? editBuffer.size() : std::min(caret + 1, editBuffer.size());
                selectAll = false;
                typed = true;
            }
            if (input->WasKeyPressed(Key::Home)) {
                caret = 0;
                selectAll = false;
                typed = true;
            }
            if (input->WasKeyPressed(Key::End)) {
                caret = editBuffer.size();
                selectAll = false;
                typed = true;
            }
            if (typed) {
                caretTime = time; // le curseur reste visible pendant la frappe
            }
            if (input->WasKeyPressed(Key::Enter) || input->WasKeyPressed(Key::NumpadEnter) || input->WasKeyPressed(Key::Tab)) {
                commit = true;
            }
            if (input->WasKeyPressed(Key::Escape)) {
                cancel = true;
            }
            if (mousePressed && !Contains(min, max, mouse)) {
                commit = true; // clic ailleurs : on garde ce qui a été tapé
            }

            if (commit) {
                if (const std::optional<double> parsed = ParseNumber(editBuffer)) {
                    const double clamped = std::clamp(*parsed, low, high);
                    changed = clamped != value;
                    value = clamped;
                }
                editingId = 0;
            } else if (cancel) {
                editingId = 0;
            }

            DrawList& list = panel->content;
            list.AddRect(min, max, style.frameActive);
            const Vector2f textPosition{min.x + style.framePadding.x, min.y + style.framePadding.y};
            if (editingId != 0) {
                const float textWidth = MeasureText(editBuffer).x;
                if (selectAll && !editBuffer.empty()) {
                    list.AddRect({textPosition.x - 1.0f, min.y + 2.0f}, {textPosition.x + textWidth + 1.0f, max.y - 2.0f},
                                 style.selection);
                }
                ClippedText(textPosition, max.x - 1.0f, editBuffer, style.text);
                if (std::fmod(time - caretTime, 1.0) < 0.55) {
                    const float caretX = textPosition.x + MeasureText(std::string_view(editBuffer).substr(0, caret)).x;
                    list.AddRect({caretX, min.y + 3.0f}, {caretX + 1.0f, max.y - 3.0f}, style.text);
                }
            }
        }

        bool DragScalar(WidgetId id, Vector2f min, Vector2f max, double& value, double speed, double low, double high,
                        int decimals, const LinearColor* axisColor) {
            bool changed = false;
            if (editingId == id) {
                EditField(min, max, value, low, high, changed);
                if (editingId == id) {
                    return changed;
                }
                // Saisie terminée à cette frame : on dessine le champ normalement par-dessus.
            } else {
                const Interaction interaction = Behavior(id, min, max);
                if (interaction.held) {
                    if (!dragging && std::abs(mouse.x - pressPosition.x) > 2.0f) {
                        dragging = true; // quelques pixels de tolérance pour qu'un clic reste un clic
                    }
                    if (dragging && mouseDelta.x != 0.0f) {
                        double step = speed;
                        if (input->IsShiftDown()) {
                            step *= 10.0;
                        } else if (input->IsControlDown()) {
                            step *= 0.1;
                        }
                        const double dragged = std::clamp(value + static_cast<double>(mouseDelta.x) * step, low, high);
                        changed = dragged != value;
                        value = dragged;
                    }
                }
                if (interaction.clicked && !dragging) {
                    editingId = id;
                    editingSeen = true;
                    editBuffer = FormatEditable(value, decimals);
                    caret = editBuffer.size();
                    selectAll = true;
                    caretTime = time;
                }
            }

            const bool active = activeId == id || editingId == id;
            const bool hovered = mouseEnabled && panelId == hoveredPanel && activeId == 0 && Contains(min, max, mouse);
            DrawList& list = panel->content;
            list.AddRect(min, max, active ? style.frameActive : (hovered ? style.frameHovered : style.frame));
            if (axisColor) {
                list.AddRect(min, {min.x + 3.0f, max.y}, *axisColor);
            }
            const std::string text = FormatNumber(value, decimals);
            const float textWidth = MeasureText(text).x;
            // Centré ; aligné à gauche quand il déborde, pour garder le signe et les premiers chiffres visibles.
            const float x = std::max(min.x + style.framePadding.x, (min.x + max.x - textWidth) * 0.5f);
            ClippedText({x, min.y + style.framePadding.y}, max.x - 2.0f, text, style.text);
            return changed;
        }

        template <typename T>
        bool DragVector3(std::string_view label, Math::Vector3<T>& value, double speed, double low, double high,
                         int decimals) {
            if (!panel) {
                return false;
            }
            const WidgetId id = GetId(label);
            const auto [fieldMin, fieldMax] = LabeledRow(label);
            const float width = (fieldMax.x - fieldMin.x - 2.0f * style.itemSpacing) / 3.0f;
            T* components[3] = {&value.x, &value.y, &value.z};
            const LinearColor* colors[3] = {&style.axisX, &style.axisY, &style.axisZ};
            bool changed = false;
            for (int axis = 0; axis < 3; ++axis) {
                const float x = fieldMin.x + static_cast<float>(axis) * (width + style.itemSpacing);
                double component = static_cast<double>(*components[axis]);
                if (DragScalar(Hash(id, &axis, sizeof(axis)), {x, fieldMin.y}, {x + width, fieldMax.y}, component, speed,
                               low, high, decimals, colors[axis])) {
                    *components[axis] = static_cast<T>(component);
                    changed = true;
                }
            }
            return changed;
        }
    };

    Context::Context(const Font& font, const Style& style) : m_impl(std::make_unique<Impl>(font, style)) {}

    Context::~Context() = default;

    void Context::BeginFrame(const Input& input, Math::Vector2f screenSize, double deltaSeconds, bool mouseEnabled) {
        Impl& ui = *m_impl;
        ui.input = &input;
        ui.screenSize = screenSize;
        ui.time += deltaSeconds;
        ++ui.frame;

        ui.mouse = input.GetMousePosition();
        ui.mouseDelta = ui.mouse - ui.previousMouse;
        ui.previousMouse = ui.mouse;
        ui.mouseEnabled = mouseEnabled;
        ui.mousePressed = mouseEnabled && input.WasMouseButtonPressed(MouseButton::Left);
        ui.mouseReleased = input.WasMouseButtonReleased(MouseButton::Left);
        ui.mouseDown = input.IsMouseButtonDown(MouseButton::Left);

        // Survol d'après les rectangles de la frame précédente, du panneau le plus proche au plus lointain.
        ui.hoveredPanel = 0;
        if (mouseEnabled) {
            for (auto it = ui.order.rbegin(); it != ui.order.rend(); ++it) {
                const Impl::Panel& panel = *ui.panels.at(*it);
                if (panel.lastFrame + 1 == ui.frame
                    && Contains(panel.position, panel.position + Vector2f{panel.width, panel.height}, ui.mouse)) {
                    ui.hoveredPanel = *it;
                    break;
                }
            }
        }
        // Un clic sur un panneau le passe au premier plan.
        if (ui.mousePressed && ui.hoveredPanel != 0 && ui.order.back() != ui.hoveredPanel) {
            std::erase(ui.order, ui.hoveredPanel);
            ui.order.push_back(ui.hoveredPanel);
        }

        ui.activeSeen = false;
        ui.editingSeen = false;
        ui.idStack.assign(1, RootId);
        ui.panel = nullptr;
        ui.panelId = 0;
    }

    void Context::EndFrame(DrawList& output) {
        Impl& ui = *m_impl;
        // Widget actif ou champ en saisie qui n'a pas été redessiné (panneau fermé ou replié) : abandonné.
        if (!ui.activeSeen) {
            ui.activeId = 0;
        }
        if (!ui.editingSeen) {
            ui.editingId = 0;
        }
        for (const WidgetId id : ui.order) {
            const Impl::Panel& panel = *ui.panels.at(id);
            if (panel.lastFrame == ui.frame) {
                output.Append(panel.chrome);
                output.Append(panel.content);
            }
        }
    }

    bool Context::WantsMouse() const {
        return m_impl->hoveredPanel != 0 || m_impl->activeId != 0;
    }

    bool Context::WantsKeyboard() const {
        return m_impl->editingId != 0;
    }

    bool Context::BeginPanel(std::string_view title, Math::Vector2f initialPosition, float width) {
        Impl& ui = *m_impl;
        const WidgetId id = ui.GetId(title);
        auto& slot = ui.panels[id];
        if (!slot) {
            slot = std::make_unique<Impl::Panel>(ui.font);
            slot->position = initialPosition;
            slot->width = width;
            ui.order.push_back(id);
        }
        Impl::Panel& panel = *slot;
        panel.title = VisibleLabel(title);
        panel.lastFrame = ui.frame;
        panel.chrome.Clear();
        panel.content.Clear();
        ui.panel = &panel;
        ui.panelId = id;
        ui.idStack.push_back(id);

        // Barre de titre : glisser pour déplacer, cliquer pour replier.
        const float titleHeight = ui.FrameHeight();
        const WidgetId titleId = Hash(id, "#titre");
        const bool wasCollapsed = panel.collapsed;
        panel.collapsed = false; // Behavior ignore les panneaux repliés ; la barre de titre, elle, reste active
        const Impl::Interaction titleBar = ui.Behavior(titleId, panel.position, panel.position + Vector2f{panel.width, titleHeight});
        panel.collapsed = wasCollapsed;
        if (ui.activeId == titleId && ui.mousePressed) {
            ui.dragOrigin = panel.position;
        }
        if (titleBar.held) {
            const Vector2f moved = ui.mouse - ui.pressPosition;
            if (!ui.dragging && std::abs(moved.x) + std::abs(moved.y) > 3.0f) {
                ui.dragging = true;
            }
            if (ui.dragging) {
                panel.position = ui.dragOrigin + moved;
            }
        }
        if (titleBar.clicked && !ui.dragging) {
            panel.collapsed = !panel.collapsed;
        }
        // La barre de titre reste toujours attrapable, même si la fenêtre a rétréci.
        panel.position.x = std::clamp(panel.position.x, 40.0f - panel.width, std::max(0.0f, ui.screenSize.x - 40.0f));
        panel.position.y = std::clamp(panel.position.y, 0.0f, std::max(0.0f, ui.screenSize.y - titleHeight));

        ui.contentMin = {panel.position.x + ui.style.panelPadding, panel.position.y + titleHeight + ui.style.panelPadding};
        ui.contentMaxX = panel.position.x + panel.width - ui.style.panelPadding;
        ui.cursorY = ui.contentMin.y;
        ui.sameLine = false;
        ui.RestorePanelClip();
        return !panel.collapsed;
    }

    void Context::EndPanel() {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        Impl::Panel& panel = *ui.panel;
        const Style& style = ui.style;
        const float titleHeight = ui.FrameHeight();
        const bool empty = ui.cursorY == ui.contentMin.y;
        panel.height = panel.collapsed || empty ? titleHeight
                                                : ui.cursorY - style.itemSpacing + style.panelPadding - panel.position.y;

        const Vector2f min = panel.position;
        const Vector2f max = min + Vector2f{panel.width, panel.height};
        DrawList& chrome = panel.chrome;
        chrome.AddRect(min, max, style.panelBackground);
        const bool front = ui.order.back() == ui.panelId;
        chrome.AddRect(min, {max.x, min.y + titleHeight}, front ? style.titleBackgroundActive : style.titleBackground);

        // Flèche : vers le bas si déplié, vers la droite si replié.
        const float arrow = style.fontSize * 0.5f;
        const float arrowX = min.x + style.panelPadding;
        const float centerY = min.y + titleHeight * 0.5f;
        if (panel.collapsed) {
            chrome.AddTriangle({arrowX + arrow * 0.15f, centerY - arrow * 0.5f}, {arrowX + arrow * 0.15f, centerY + arrow * 0.5f},
                               {arrowX + arrow * 0.95f, centerY}, style.text);
        } else {
            chrome.AddTriangle({arrowX, centerY - arrow * 0.35f}, {arrowX + arrow, centerY - arrow * 0.35f},
                               {arrowX + arrow * 0.5f, centerY + arrow * 0.45f}, style.text);
        }
        const float titleX = arrowX + arrow + style.framePadding.x;
        chrome.SetClipRect({titleX, min.y}, {max.x - style.framePadding.x, min.y + titleHeight});
        chrome.AddText({titleX, min.y + style.framePadding.y}, panel.title, style.fontSize, style.text);
        chrome.ResetClipRect();
        chrome.AddRectOutline(min, max, style.panelBorder);

        panel.content.ResetClipRect();
        ui.idStack.pop_back();
        ui.panel = nullptr;
        ui.panelId = 0;
    }

    void Context::Text(std::string_view text) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        const Vector2f min = ui.ItemRect(ui.MeasureText(text));
        ui.panel->content.AddText(min, text, ui.style.fontSize, ui.style.text);
    }

    void Context::TextDisabled(std::string_view text) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        const Vector2f min = ui.ItemRect(ui.MeasureText(text));
        ui.panel->content.AddText(min, text, ui.style.fontSize, ui.style.textDisabled);
    }

    void Context::LabelText(std::string_view label, std::string_view text) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        ui.ClippedText({fieldMin.x + ui.style.framePadding.x, fieldMin.y + ui.style.framePadding.y}, fieldMax.x, text,
                       ui.style.text);
    }

    bool Context::Button(std::string_view label) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const Style& style = ui.style;
        const std::string_view visible = VisibleLabel(label);
        const Vector2f size{ui.MeasureText(visible).x + 2.0f * style.framePadding.x, ui.FrameHeight()};
        const Vector2f min = ui.ItemRect(size);
        const Vector2f max = min + size;
        const Impl::Interaction interaction = ui.Behavior(ui.GetId(label), min, max);

        const LinearColor color = interaction.held && interaction.hovered ? style.buttonActive
                                : interaction.hovered                     ? style.buttonHovered
                                                                          : style.button;
        ui.panel->content.AddRect(min, max, color);
        ui.panel->content.AddText(min + style.framePadding, visible, style.fontSize, style.text);
        return interaction.clicked;
    }

    bool Context::Checkbox(std::string_view label, bool& value) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const Style& style = ui.style;
        const std::string_view visible = VisibleLabel(label);
        const float box = ui.FrameHeight();
        const float labelWidth = visible.empty() ? 0.0f : 2.0f * style.itemSpacing + ui.MeasureText(visible).x;
        const Vector2f min = ui.ItemRect({box + labelWidth, box});
        // Toute la ligne est cliquable, libellé compris.
        const Impl::Interaction interaction = ui.Behavior(ui.GetId(label), min, min + Vector2f{box + labelWidth, box});
        if (interaction.clicked) {
            value = !value;
        }

        DrawList& list = ui.panel->content;
        const LinearColor color = interaction.held && interaction.hovered ? style.frameActive
                                : interaction.hovered                     ? style.frameHovered
                                                                          : style.frame;
        list.AddRect(min, min + Vector2f{box, box}, color);
        if (value) {
            const float thickness = std::max(2.0f, box * 0.12f);
            const auto at = [&](float x, float y) { return min + Vector2f{box * x, box * y}; };
            AddLine(list, at(0.22f, 0.52f), at(0.42f, 0.72f), thickness, style.checkMark);
            AddLine(list, at(0.40f, 0.72f), at(0.78f, 0.30f), thickness, style.checkMark);
        }
        if (!visible.empty()) {
            list.AddText({min.x + box + 2.0f * style.itemSpacing, min.y + style.framePadding.y}, visible, style.fontSize,
                         style.text);
        }
        return interaction.clicked;
    }

    bool Context::DragDouble(std::string_view label, double& value, double speed, double min, double max, int decimals) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const WidgetId id = ui.GetId(label);
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        return ui.DragScalar(id, fieldMin, fieldMax, value, speed, min, max, decimals, nullptr);
    }

    bool Context::DragFloat(std::string_view label, float& value, float speed, double min, double max, int decimals) {
        double wide = value;
        if (!DragDouble(label, wide, speed, min, max, decimals)) {
            return false;
        }
        value = static_cast<float>(wide);
        return true;
    }

    bool Context::DragFloat3(std::string_view label, Math::Vector3f& value, float speed, double min, double max,
                             int decimals) {
        return m_impl->DragVector3(label, value, speed, min, max, decimals);
    }

    bool Context::DragDouble3(std::string_view label, Math::Vector3d& value, double speed, double min, double max,
                              int decimals) {
        return m_impl->DragVector3(label, value, speed, min, max, decimals);
    }

    void Context::Separator() {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        const float spacing = ui.style.itemSpacing;
        const Vector2f min = ui.ItemRect({ui.RowWidth(), spacing + 1.0f});
        ui.panel->content.AddRect({min.x, min.y + spacing * 0.5f}, {min.x + ui.RowWidth(), min.y + spacing * 0.5f + 1.0f},
                                  ui.style.separator);
    }

    void Context::SameLine() {
        m_impl->sameLine = true;
    }

    void Context::PushId(std::string_view id) {
        m_impl->idStack.push_back(Hash(m_impl->idStack.back(), id));
    }

    void Context::PushId(std::uint64_t id) {
        m_impl->idStack.push_back(Hash(m_impl->idStack.back(), &id, sizeof(id)));
    }

    void Context::PopId() {
        if (m_impl->idStack.size() > 1) {
            m_impl->idStack.pop_back();
        }
    }

    const Style& Context::GetStyle() const {
        return m_impl->style;
    }
}
