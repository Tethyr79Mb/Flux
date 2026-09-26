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

        // Déplacements du curseur de saisie d'un caractère UTF-8 entier (é tient sur deux octets).
        std::size_t PreviousBoundary(std::string_view text, std::size_t index) {
            while (index > 0) {
                --index;
                if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
                    break;
                }
            }
            return index;
        }

        std::size_t NextBoundary(std::string_view text, std::size_t index) {
            if (index < text.size()) {
                ++index;
            }
            while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
                ++index;
            }
            return index;
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
            float fixedHeight = 0.0f; // 0 : ajustée au contenu
            float scrollY = 0.0f;
            float contentHeight = 0.0f; // hauteur totale du contenu à la dernière frame, défilement compris
            bool collapsed = false;
            bool overlay = false;       // barre de menus ou menu déroulant : au-dessus des panneaux, jamais dans order
            float measuredWidth = 0.0f; // menu déroulant : largeur de ses éléments à la dernière frame
            std::uint64_t lastFrame = 0;
            DrawList chrome;          // fond, bordure et barre de titre, dessinés sous le contenu
            DrawList content;
        };

        enum class EditResult { Editing, Commit, Cancel };

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
        float wheel = 0.0f;
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

        // Barre de menus et menu déroulant ouvert : dessinés après les panneaux, survolés avant eux.
        std::vector<WidgetId> overlays;         // décrits pendant la frame en cours
        std::vector<WidgetId> previousOverlays; // ceux de la frame précédente, pour le survol
        WidgetId openMenu = 0;                  // titre du menu ouvert
        bool openMenuSeen = false;
        bool menuBarDrawn = false;
        float menuBarHeight = 0.0f;             // à la frame précédente
        float menuBarNextX = 0.0f;              // position du prochain titre dans la barre
        float menuItemWidth = 0.0f;             // plus large élément du menu ouvert
        bool clickSwallowed = false;            // clic qui a refermé un menu : il ne va à personne d'autre

        // Mise en page de la barre, mise de côté pendant la description du menu déroulant.
        struct Layout {
            Panel* panel = nullptr;
            WidgetId panelId = 0;
            Vector2f contentMin;
            float contentMaxX = 0.0f;
            float cursorY = 0.0f;
            float indent = 0.0f;
            Vector2f clipMin;
            Vector2f clipMax;
            Vector2f lastItemMin;
            Vector2f lastItemMax;
            bool sameLine = false;
            float lastSpacing = 0.0f;
        };
        std::optional<Layout> menuParent;

        // Panneau en cours de description et sa mise en page.
        Panel* panel = nullptr;
        WidgetId panelId = 0;
        Vector2f contentMin;
        float contentMaxX = 0.0f;
        float cursorY = 0.0f;
        float indent = 0.0f;       // décalage des nœuds d'arbre dépliés
        Vector2f clipMin;          // zone visible du contenu (sous la barre de titre, au-dessus du bas défilé)
        Vector2f clipMax;
        Vector2f lastItemMin;
        Vector2f lastItemMax;
        bool sameLine = false;
        float lastSpacing = 0.0f;

        std::vector<WidgetId> idStack{RootId};

        // Widget sous le bouton gauche enfoncé (glissement, clic en cours).
        WidgetId activeId = 0;
        bool activeSeen = false;
        Vector2f pressPosition;
        Vector2f dragOrigin;
        bool dragging = false;

        // Arbres : nœuds dépliés, et dernier clic pour reconnaître un double-clic.
        std::unordered_map<WidgetId, bool> treeOpen;
        std::optional<bool> nextTreeOpen;
        // Valeurs en cours de glissement des champs entiers (DragInt3).
        std::unordered_map<WidgetId, double> intAccumulators;
        WidgetId lastClickId = 0;
        double lastClickTime = -1.0;

        // Champ (numérique ou texte) en cours de saisie.
        WidgetId editingId = 0;
        bool editingSeen = false;
        std::string editBuffer;
        std::size_t caret = 0;
        bool selectAll = false;
        double caretTime = 0.0;

        float LineHeight() const { return font.GetLineHeight() * style.fontSize; }
        float FrameHeight() const { return LineHeight() + 2.0f * style.framePadding.y; }
        float RowWidth() const { return contentMaxX - (contentMin.x + indent); }
        float TreeRowHeight() const { return LineHeight() + 4.0f; }

        WidgetId GetId(std::string_view label) const { return Hash(idStack.back(), label); }

        Vector2f MeasureText(std::string_view text) const {
            return panel->content.MeasureText(text, style.fontSize);
        }

        // Réserve la place d'un widget et retourne son coin haut-gauche. spacing < 0 : espacement du style.
        Vector2f ItemRect(Vector2f size, float spacing = -1.0f) {
            const Vector2f min = sameLine ? Vector2f{lastItemMax.x + style.itemSpacing, lastItemMin.y}
                                          : Vector2f{contentMin.x + indent, cursorY};
            sameLine = false;
            lastItemMin = min;
            lastItemMax = min + size;
            lastSpacing = spacing < 0.0f ? style.itemSpacing : spacing;
            cursorY = std::max(cursorY, lastItemMax.y + lastSpacing);
            return min;
        }

        Layout SaveLayout() const {
            return {panel, panelId, contentMin, contentMaxX, cursorY, indent, clipMin, clipMax, lastItemMin, lastItemMax,
                    sameLine, lastSpacing};
        }

        void RestoreLayout(const Layout& layout) {
            panel = layout.panel;
            panelId = layout.panelId;
            contentMin = layout.contentMin;
            contentMaxX = layout.contentMaxX;
            cursorY = layout.cursorY;
            indent = layout.indent;
            clipMin = layout.clipMin;
            clipMax = layout.clipMax;
            lastItemMin = layout.lastItemMin;
            lastItemMax = layout.lastItemMax;
            sameLine = layout.sameLine;
            lastSpacing = layout.lastSpacing;
        }

        // Barre de menus ou menu déroulant : devient le panneau en cours, hors de l'ordre des panneaux.
        Panel& BeginOverlay(WidgetId id) {
            auto& slot = panels[id];
            if (!slot) {
                slot = std::make_unique<Panel>(font);
                slot->overlay = true;
            }
            Panel& overlay = *slot;
            overlay.lastFrame = frame;
            overlay.chrome.Clear();
            overlay.content.Clear();
            overlays.push_back(id);
            panel = &overlay;
            panelId = id;
            idStack.push_back(id);
            indent = 0.0f;
            sameLine = false;
            return overlay;
        }

        void RestorePanelClip() {
            panel->content.SetClipRect(clipMin, clipMax);
        }

        // Texte découpé à [topLeft.x, maxX] en plus de la zone visible du panneau.
        void ClippedText(Vector2f topLeft, float maxX, std::string_view text, LinearColor color, float minX = -1.0e9f) {
            panel->content.SetClipRect({std::max(std::max(topLeft.x, minX), clipMin.x), clipMin.y},
                                       {std::min(maxX, clipMax.x), clipMax.y});
            panel->content.AddText(topLeft, text, style.fontSize, color);
            RestorePanelClip();
        }

        // Souris au-dessus de ce rectangle, dans la partie visible du panneau survolé.
        bool IsMouseOver(Vector2f min, Vector2f max) const {
            return mouseEnabled && panel && !panel->collapsed && panelId == hoveredPanel && Contains(min, max, mouse)
                && Contains(clipMin, clipMax, mouse);
        }

        Interaction Behavior(WidgetId id, Vector2f min, Vector2f max) {
            Interaction result;
            const bool inside = IsMouseOver(min, max);
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

        void BeginEdit(WidgetId id, std::string text) {
            editingId = id;
            editingSeen = true;
            editBuffer = std::move(text);
            caret = editBuffer.size();
            selectAll = true; // la première frappe remplace tout, comme dans la plupart des éditeurs
            caretTime = time;
        }

        // Frappe dans le champ en cours de saisie ; numeric : seuls les caractères d'un nombre sont acceptés.
        EditResult ProcessTextEdit(Vector2f min, Vector2f max, bool numeric) {
            editingSeen = true;
            bool typed = false;
            const auto clearIfSelected = [&] {
                if (selectAll) {
                    editBuffer.clear();
                    caret = 0;
                    selectAll = false;
                }
            };

            const std::string_view text = input->GetTextInput();
            for (std::size_t i = 0; i < text.size();) {
                const std::size_t next = NextBoundary(text, i);
                const std::string_view character = text.substr(i, next - i);
                i = next;
                if (numeric && (character.size() != 1 || !IsNumberCharacter(character[0]))) {
                    continue;
                }
                clearIfSelected();
                editBuffer.insert(caret, character);
                caret += character.size();
                typed = true;
            }
            if (input->WasKeyTyped(Key::Backspace)) {
                if (selectAll) {
                    clearIfSelected();
                } else if (caret > 0) {
                    const std::size_t previous = PreviousBoundary(editBuffer, caret);
                    editBuffer.erase(previous, caret - previous);
                    caret = previous;
                }
                typed = true;
            }
            if (input->WasKeyTyped(Key::Delete)) {
                if (selectAll) {
                    clearIfSelected();
                } else if (caret < editBuffer.size()) {
                    editBuffer.erase(caret, NextBoundary(editBuffer, caret) - caret);
                }
                typed = true;
            }
            if (input->WasKeyTyped(Key::Left)) {
                caret = selectAll ? 0 : PreviousBoundary(editBuffer, caret);
                selectAll = false;
                typed = true;
            }
            if (input->WasKeyTyped(Key::Right)) {
                caret = selectAll ? editBuffer.size() : NextBoundary(editBuffer, caret);
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

            if (input->WasKeyPressed(Key::Escape)) {
                editingId = 0;
                return EditResult::Cancel;
            }
            if (input->WasKeyPressed(Key::Enter) || input->WasKeyPressed(Key::NumpadEnter) || input->WasKeyPressed(Key::Tab)
                || (mousePressed && !Contains(min, max, mouse))) { // clic ailleurs : on garde ce qui a été tapé
                editingId = 0;
                return EditResult::Commit;
            }
            return EditResult::Editing;
        }

        void DrawTextEdit(Vector2f min, Vector2f max) {
            DrawList& list = panel->content;
            list.AddRect(min, max, style.frameActive);
            // Texte décalé vers la gauche quand le curseur sortirait du champ.
            const float visibleWidth = max.x - min.x - 2.0f * style.framePadding.x;
            const float caretOffset = MeasureText(std::string_view(editBuffer).substr(0, caret)).x;
            const float scroll = std::max(0.0f, caretOffset - visibleWidth + 1.0f);
            const Vector2f textPosition{min.x + style.framePadding.x - scroll, min.y + style.framePadding.y};
            const float clipLeft = min.x + 1.0f;
            if (selectAll && !editBuffer.empty()) {
                const float textWidth = MeasureText(editBuffer).x;
                list.AddRect({std::max(textPosition.x - 1.0f, clipLeft), min.y + 2.0f},
                             {std::min(textPosition.x + textWidth + 1.0f, max.x - 1.0f), max.y - 2.0f}, style.selection);
            }
            ClippedText(textPosition, max.x - 1.0f, editBuffer, style.text, clipLeft);
            if (std::fmod(time - caretTime, 1.0) < 0.55) {
                const float caretX = textPosition.x + caretOffset;
                list.AddRect({caretX, min.y + 3.0f}, {caretX + 1.0f, max.y - 3.0f}, style.text);
            }
        }

        bool DragScalar(WidgetId id, Vector2f min, Vector2f max, double& value, double speed, double low, double high,
                        int decimals, const LinearColor* axisColor) {
            bool changed = false;
            if (editingId == id) {
                const EditResult result = ProcessTextEdit(min, max, true);
                if (result == EditResult::Editing) {
                    DrawTextEdit(min, max);
                    return false;
                }
                if (result == EditResult::Commit) {
                    if (const std::optional<double> parsed = ParseNumber(editBuffer)) {
                        const double clamped = std::clamp(*parsed, low, high);
                        changed = clamped != value;
                        value = clamped;
                    }
                }
                // Saisie terminée à cette frame : le champ est dessiné normalement.
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
                    BeginEdit(id, FormatEditable(value, decimals));
                }
            }

            const bool active = activeId == id || editingId == id;
            const bool hovered = activeId == 0 && IsMouseOver(min, max);
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
        ui.wheel = mouseEnabled ? input.GetWheelDelta() : 0.0f;

        // Survol d'après les rectangles de la frame précédente, du panneau le plus proche au plus lointain.
        ui.hoveredPanel = 0;
        if (mouseEnabled) {
            for (auto it = ui.previousOverlays.rbegin(); it != ui.previousOverlays.rend() && ui.hoveredPanel == 0; ++it) {
                const Impl::Panel& panel = *ui.panels.at(*it);
                if (Contains(panel.position, panel.position + Vector2f{panel.width, panel.height}, ui.mouse)) {
                    ui.hoveredPanel = *it;
                }
            }
            for (auto it = ui.order.rbegin(); it != ui.order.rend() && ui.hoveredPanel == 0; ++it) {
                const Impl::Panel& panel = *ui.panels.at(*it);
                if (panel.lastFrame + 1 == ui.frame
                    && Contains(panel.position, panel.position + Vector2f{panel.width, panel.height}, ui.mouse)) {
                    ui.hoveredPanel = *it;
                    break;
                }
            }
        }
        // Menu ouvert : un clic ailleurs (ou Échap) le referme, sans rien faire d'autre.
        ui.clickSwallowed = false;
        if (ui.openMenu != 0) {
            const bool overMenu = ui.hoveredPanel != 0 && ui.panels.at(ui.hoveredPanel)->overlay;
            if (ui.mousePressed && !overMenu) {
                ui.openMenu = 0;
                ui.mousePressed = false;
                ui.clickSwallowed = true;
            } else if (input.WasKeyPressed(Key::Escape)) {
                ui.openMenu = 0;
            }
        }
        // Un clic sur un panneau le passe au premier plan.
        if (ui.mousePressed && ui.hoveredPanel != 0 && !ui.panels.at(ui.hoveredPanel)->overlay
            && ui.order.back() != ui.hoveredPanel) {
            std::erase(ui.order, ui.hoveredPanel);
            ui.order.push_back(ui.hoveredPanel);
        }

        ui.activeSeen = false;
        ui.editingSeen = false;
        ui.idStack.assign(1, RootId);
        ui.panel = nullptr;
        ui.panelId = 0;
        ui.menuParent.reset();
        ui.overlays.clear();
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
        // Menu ouvert dont le titre n'a pas été décrit (barre masquée) : refermé.
        if (!ui.openMenuSeen) {
            ui.openMenu = 0;
        }
        ui.openMenuSeen = false;
        ui.menuBarHeight = ui.menuBarDrawn ? ui.FrameHeight() : 0.0f;
        ui.menuBarDrawn = false;

        for (const WidgetId id : ui.order) {
            const Impl::Panel& panel = *ui.panels.at(id);
            if (panel.lastFrame == ui.frame) {
                output.Append(panel.chrome);
                output.Append(panel.content);
            }
        }
        for (const WidgetId id : ui.overlays) {
            const Impl::Panel& panel = *ui.panels.at(id);
            output.Append(panel.chrome);
            output.Append(panel.content);
        }
        ui.previousOverlays = std::move(ui.overlays);
        ui.overlays.clear();
    }

    bool Context::WantsMouse() const {
        const Impl& ui = *m_impl;
        return ui.hoveredPanel != 0 || ui.activeId != 0 || ui.openMenu != 0 || ui.clickSwallowed;
    }

    bool Context::WantsKeyboard() const {
        return m_impl->editingId != 0;
    }

    bool Context::BeginPanel(std::string_view title, Math::Vector2f initialPosition, float width, float height) {
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
        panel.fixedHeight = height;
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
        ui.clipMin = panel.position;
        ui.clipMax = panel.position + Vector2f{panel.width, titleHeight};
        const Impl::Interaction titleBar = ui.Behavior(titleId, ui.clipMin, ui.clipMax);
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
        // Jamais sous la barre de menus.
        panel.position.y = std::clamp(panel.position.y, ui.menuBarHeight,
                                      std::max(ui.menuBarHeight, ui.screenSize.y - titleHeight));

        // Défilement (hauteur fixe seulement) : molette au-dessus du panneau, bornée par le contenu de la frame précédente.
        const float viewTop = panel.position.y + titleHeight;
        const float viewHeight = std::max(0.0f, panel.fixedHeight - titleHeight);
        const bool scrollable = panel.fixedHeight > 0.0f;
        const float maxScroll = scrollable ? std::max(0.0f, panel.contentHeight - viewHeight) : 0.0f;
        if (scrollable && ui.panelId == ui.hoveredPanel && ui.wheel != 0.0f) {
            panel.scrollY -= ui.wheel * 3.0f * (ui.TreeRowHeight() + 1.0f); // trois lignes d'arbre par cran
        }
        panel.scrollY = std::clamp(panel.scrollY, 0.0f, maxScroll);

        const float scrollbarWidth = maxScroll > 0.0f ? 10.0f : 0.0f;
        ui.contentMin = {panel.position.x + ui.style.panelPadding, viewTop + ui.style.panelPadding - panel.scrollY};
        ui.contentMaxX = panel.position.x + panel.width - ui.style.panelPadding - scrollbarWidth;
        ui.cursorY = ui.contentMin.y;
        ui.indent = 0.0f;
        ui.sameLine = false;
        ui.clipMin = {panel.position.x + 1.0f, viewTop};
        ui.clipMax = {panel.position.x + panel.width - 1.0f - scrollbarWidth,
                      scrollable ? panel.position.y + panel.fixedHeight - 1.0f : ui.screenSize.y + 1.0e4f};
        if (panel.collapsed) {
            ui.clipMax = ui.clipMin; // rien du contenu ne doit apparaître, même si l'appelant le décrit quand même
        }
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
        const float viewTop = panel.position.y + titleHeight;
        const bool empty = ui.cursorY == ui.contentMin.y;
        if (!panel.collapsed) {
            panel.contentHeight = empty ? 0.0f : ui.cursorY - ui.lastSpacing + style.panelPadding - (viewTop - panel.scrollY);
        }
        if (panel.collapsed) {
            panel.height = titleHeight;
        } else if (panel.fixedHeight > 0.0f) {
            panel.height = std::max(panel.fixedHeight, titleHeight);
        } else {
            panel.height = titleHeight + panel.contentHeight;
        }

        const Vector2f min = panel.position;
        const Vector2f max = min + Vector2f{panel.width, panel.height};
        DrawList& chrome = panel.chrome;
        chrome.AddRect(min, max, style.panelBackground);
        const bool front = ui.order.back() == ui.panelId;
        chrome.AddRect(min, {max.x, viewTop}, front ? style.titleBackgroundActive : style.titleBackground);

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
        chrome.SetClipRect({titleX, min.y}, {max.x - style.framePadding.x, viewTop});
        chrome.AddText({titleX, min.y + style.framePadding.y}, panel.title, style.fontSize, style.text);
        chrome.ResetClipRect();

        // Barre de défilement : glisser le curseur, la molette fait le reste.
        const float viewHeight = panel.height - titleHeight;
        const float maxScroll = std::max(0.0f, panel.contentHeight - viewHeight);
        if (!panel.collapsed && panel.fixedHeight > 0.0f && maxScroll > 0.0f) {
            const float trackTop = viewTop + 3.0f;
            const float trackHeight = viewHeight - 6.0f;
            const float thumbHeight = std::max(20.0f, trackHeight * viewHeight / panel.contentHeight);
            const float travel = std::max(1.0f, trackHeight - thumbHeight);
            const float thumbY = trackTop + travel * panel.scrollY / maxScroll;
            const Vector2f thumbMin{max.x - 9.0f, thumbY};
            const Vector2f thumbMax{max.x - 3.0f, thumbY + thumbHeight};
            const WidgetId thumbId = Hash(ui.panelId, "#défilement");
            const Impl::Interaction thumb = ui.Behavior(thumbId, {thumbMin.x - 2.0f, thumbMin.y}, {thumbMax.x + 2.0f, thumbMax.y});
            if (ui.activeId == thumbId && ui.mousePressed) {
                ui.dragOrigin = {0.0f, panel.scrollY};
            }
            if (thumb.held) {
                panel.scrollY = std::clamp(ui.dragOrigin.y + (ui.mouse.y - ui.pressPosition.y) * maxScroll / travel, 0.0f, maxScroll);
            }
            chrome.AddRect({thumbMin.x, trackTop}, {thumbMax.x, trackTop + trackHeight}, style.frame);
            chrome.AddRect(thumbMin, thumbMax, thumb.held || thumb.hovered ? style.textDisabled : style.frameActive);
        }
        // La barre est dessinée sous le contenu (chrome) mais à droite de celui-ci : pas de chevauchement.
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

    bool Context::InputText(std::string_view label, std::string& value) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const WidgetId id = ui.GetId(label);
        const auto [min, max] = ui.LabeledRow(label);
        if (ui.editingId == id) {
            const Impl::EditResult result = ui.ProcessTextEdit(min, max, false);
            if (result == Impl::EditResult::Editing) {
                ui.DrawTextEdit(min, max);
                return false;
            }
            const bool changed = result == Impl::EditResult::Commit && ui.editBuffer != value;
            if (changed) {
                value = ui.editBuffer;
            }
            ui.panel->content.AddRect(min, max, ui.style.frame);
            ui.ClippedText(min + ui.style.framePadding, max.x - 2.0f, value, ui.style.text);
            return changed;
        }

        const Impl::Interaction interaction = ui.Behavior(id, min, max);
        if (interaction.clicked) {
            ui.BeginEdit(id, value);
        }
        ui.panel->content.AddRect(min, max, interaction.hovered ? ui.style.frameHovered : ui.style.frame);
        ui.ClippedText(min + ui.style.framePadding, max.x - 2.0f, value, ui.style.text);
        return false;
    }

    bool Context::ColorEdit3(std::string_view label, Math::LinearColor& color) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const WidgetId id = ui.GetId(label);
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        const Style& style = ui.style;

        // Aperçu (couleur opaque), puis rouge, vert, bleu.
        const float swatch = fieldMax.y - fieldMin.y;
        ui.panel->content.AddRect(fieldMin, {fieldMin.x + swatch, fieldMax.y}, {color.r, color.g, color.b, 1.0f});
        ui.panel->content.AddRectOutline(fieldMin, {fieldMin.x + swatch, fieldMax.y}, style.panelBorder);
        const float start = fieldMin.x + swatch + style.itemSpacing;
        const float width = (fieldMax.x - start - 2.0f * style.itemSpacing) / 3.0f;
        float* channels[3] = {&color.r, &color.g, &color.b};
        const LinearColor* colors[3] = {&style.axisX, &style.axisY, &style.axisZ};
        bool changed = false;
        for (int channel = 0; channel < 3; ++channel) {
            const float x = start + static_cast<float>(channel) * (width + style.itemSpacing);
            // Sans arrondi : un glissement lent s'accumule au lieu d'être perdu à chaque frame.
            double value = static_cast<double>(Math::LinearColor::LinearToSrgb(std::clamp(*channels[channel], 0.0f, 1.0f))) * 255.0;
            if (ui.DragScalar(Hash(id, &channel, sizeof(channel)), {x, fieldMin.y}, {x + width, fieldMax.y}, value, 1.0,
                              0.0, 255.0, 0, colors[channel])) {
                *channels[channel] = Math::LinearColor::SrgbToLinear(static_cast<float>(value / 255.0));
                changed = true;
            }
        }
        return changed;
    }

    bool Context::DragInt3(std::string_view label, Math::Vector3i64& value, float speed) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const WidgetId id = ui.GetId(label);
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        const float width = (fieldMax.x - fieldMin.x - 2.0f * ui.style.itemSpacing) / 3.0f;
        std::int64_t* components[3] = {&value.x, &value.y, &value.z};
        const LinearColor* colors[3] = {&ui.style.axisX, &ui.style.axisY, &ui.style.axisZ};
        bool changed = false;
        for (int axis = 0; axis < 3; ++axis) {
            const float x = fieldMin.x + static_cast<float>(axis) * (width + ui.style.itemSpacing);
            const WidgetId fieldId = Hash(id, &axis, sizeof(axis));
            // Le glissement s'accumule en double tant que le champ est actif : un pas de 0,2 finit par compter.
            double& accumulated = ui.intAccumulators[fieldId];
            if (ui.activeId != fieldId) {
                accumulated = static_cast<double>(*components[axis]);
            }
            // Exact jusqu'à 2^53 secteurs, bien au-delà d'une galaxie (1e12 secteurs).
            constexpr double Limit = 9.0e15;
            if (ui.DragScalar(fieldId, {x, fieldMin.y}, {x + width, fieldMax.y}, accumulated, speed, -Limit, Limit, 0,
                              colors[axis])) {
                const auto rounded = static_cast<std::int64_t>(std::llround(accumulated));
                if (rounded != *components[axis]) {
                    *components[axis] = rounded;
                    changed = true;
                }
            }
        }
        return changed;
    }

    bool Context::BeginMainMenuBar() {
        Impl& ui = *m_impl;
        if (ui.panel) {
            return false; // hors de tout panneau
        }
        const float height = ui.FrameHeight();
        const float width = ui.screenSize.x;
        Impl::Panel& bar = ui.BeginOverlay(Hash(RootId, "#barre de menus"));
        bar.position = {0.0f, 0.0f};
        bar.width = width;
        bar.height = height;
        ui.contentMin = {ui.style.panelPadding * 0.5f, 0.0f};
        ui.contentMaxX = width;
        ui.cursorY = 0.0f;
        ui.clipMin = {0.0f, 0.0f};
        ui.clipMax = {width, height};
        ui.RestorePanelClip();
        ui.menuBarNextX = ui.contentMin.x;
        ui.menuBarDrawn = true;
        ui.menuBarHeight = height; // dès cette frame pour les panneaux décrits après la barre
        bar.chrome.AddRect({0.0f, 0.0f}, {width, height}, ui.style.titleBackground);
        bar.chrome.AddRect({0.0f, height - 1.0f}, {width, height}, ui.style.panelBorder);
        return true;
    }

    void Context::EndMainMenuBar() {
        Impl& ui = *m_impl;
        if (!ui.panel || !ui.panel->overlay || ui.menuParent) {
            return;
        }
        ui.panel->content.ResetClipRect();
        ui.idStack.pop_back();
        ui.panel = nullptr;
        ui.panelId = 0;
    }

    bool Context::BeginMenu(std::string_view label) {
        Impl& ui = *m_impl;
        if (!ui.panel || !ui.panel->overlay || ui.menuParent) {
            return false; // dans la barre seulement : pas de sous-menus pour l'instant
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        const std::string_view visible = VisibleLabel(label);
        const Vector2f size{ui.MeasureText(visible).x + 2.0f * style.framePadding.x, ui.FrameHeight()};
        const Vector2f min{ui.menuBarNextX, 0.0f};
        const Vector2f max = min + size;
        ui.menuBarNextX = max.x;

        const bool hovered = ui.IsMouseOver(min, max) && ui.activeId == 0;
        if (hovered && ui.mousePressed) {
            ui.openMenu = ui.openMenu == id ? 0 : id;
        } else if (hovered && ui.openMenu != 0) {
            ui.openMenu = id; // un menu est ouvert : survoler un autre titre l'ouvre à sa place
        }
        const bool open = ui.openMenu == id;
        if (open || hovered) {
            ui.panel->content.AddRect(min, max, open ? style.selection : style.frameHovered);
        }
        ui.panel->content.AddText(min + style.framePadding, visible, style.fontSize, style.text);
        if (!open) {
            return false;
        }

        ui.openMenuSeen = true;
        ui.menuParent = ui.SaveLayout();
        Impl::Panel& popup = ui.BeginOverlay(Hash(id, "#menu"));
        // Largeur des éléments mesurée à la frame précédente (la première frame, une largeur minimale).
        popup.width = std::max(180.0f, popup.measuredWidth);
        popup.position = {std::clamp(min.x, 0.0f, std::max(0.0f, ui.screenSize.x - popup.width)), max.y};
        const float padding = 4.0f;
        ui.contentMin = popup.position + Vector2f{padding, padding};
        ui.contentMaxX = popup.position.x + popup.width - padding;
        ui.cursorY = ui.contentMin.y;
        ui.lastSpacing = 0.0f;
        ui.clipMin = popup.position;
        ui.clipMax = {popup.position.x + popup.width, ui.screenSize.y};
        ui.RestorePanelClip();
        ui.menuItemWidth = 0.0f;
        return true;
    }

    void Context::EndMenu() {
        Impl& ui = *m_impl;
        if (!ui.menuParent) {
            return;
        }
        Impl::Panel& popup = *ui.panel;
        const float padding = 4.0f;
        const bool empty = ui.cursorY == ui.contentMin.y;
        popup.height = (empty ? ui.cursorY : ui.cursorY - ui.lastSpacing) + padding - popup.position.y;
        popup.measuredWidth = ui.menuItemWidth + 2.0f * padding;
        const Vector2f max = popup.position + Vector2f{popup.width, popup.height};
        popup.chrome.AddRect(popup.position, max, {ui.style.panelBackground.r, ui.style.panelBackground.g,
                                                   ui.style.panelBackground.b, 1.0f});
        popup.chrome.AddRectOutline(popup.position, max, ui.style.panelBorder);
        popup.content.ResetClipRect();

        ui.idStack.pop_back();
        ui.RestoreLayout(*ui.menuParent);
        ui.menuParent.reset();
    }

    bool Context::MenuItem(std::string_view label, std::string_view shortcut, bool enabled) {
        Impl& ui = *m_impl;
        if (!ui.panel || !ui.menuParent) {
            return false;
        }
        const Style& style = ui.style;
        const std::string_view visible = VisibleLabel(label);
        const float labelWidth = ui.MeasureText(visible).x;
        const float shortcutWidth = shortcut.empty() ? 0.0f : ui.MeasureText(shortcut).x;
        const float gap = shortcut.empty() ? 0.0f : 3.0f * style.fontSize;
        ui.menuItemWidth = std::max(ui.menuItemWidth, 2.0f * style.framePadding.x + labelWidth + gap + shortcutWidth);

        const Vector2f size{ui.RowWidth(), ui.FrameHeight()};
        const Vector2f min = ui.ItemRect(size, 0.0f);
        const Vector2f max = min + size;
        const Impl::Interaction interaction = enabled ? ui.Behavior(ui.GetId(label), min, max) : Impl::Interaction{};
        DrawList& list = ui.panel->content;
        if (interaction.hovered) {
            list.AddRect(min, max, style.selection);
        }
        list.AddText(min + style.framePadding, visible, style.fontSize, enabled ? style.text : style.textDisabled);
        if (!shortcut.empty()) {
            list.AddText({max.x - style.framePadding.x - shortcutWidth, min.y + style.framePadding.y}, shortcut, style.fontSize,
                         style.textDisabled);
        }
        if (interaction.clicked) {
            ui.openMenu = 0;
            return true;
        }
        return false;
    }

    float Context::GetMenuBarHeight() const {
        return m_impl->menuBarHeight;
    }

    bool Context::DragInt(std::string_view label, std::int32_t& value, float speed, double min, double max) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const WidgetId id = ui.GetId(label);
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        double& accumulated = ui.intAccumulators[id];
        if (ui.activeId != id) {
            accumulated = static_cast<double>(value);
        }
        constexpr double Lowest = std::numeric_limits<std::int32_t>::min();
        constexpr double Highest = std::numeric_limits<std::int32_t>::max();
        if (!ui.DragScalar(id, fieldMin, fieldMax, accumulated, speed, std::max(min, Lowest), std::min(max, Highest), 0,
                           nullptr)) {
            return false;
        }
        const auto rounded = static_cast<std::int32_t>(std::lround(accumulated));
        if (rounded == value) {
            return false;
        }
        value = rounded;
        return true;
    }

    bool Context::Choice(std::string_view label, int& index, std::span<const std::string_view> options) {
        Impl& ui = *m_impl;
        if (!ui.panel || options.empty()) {
            return false;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        const auto [min, max] = ui.LabeledRow(label);
        const Impl::Interaction interaction = ui.Behavior(id, min, max);
        const int count = static_cast<int>(options.size());
        const int previous = index;
        if (interaction.clicked) {
            index = index < 0 || index >= count - 1 ? 0 : index + 1;
        } else if (interaction.hovered && ui.input->WasMouseButtonPressed(MouseButton::Right)) {
            index = index <= 0 || index >= count ? count - 1 : index - 1;
        }

        DrawList& list = ui.panel->content;
        list.AddRect(min, max, interaction.held && interaction.hovered ? style.frameActive
                               : interaction.hovered                   ? style.frameHovered
                                                                       : style.frame);
        const std::string_view text = index >= 0 && index < count ? options[static_cast<std::size_t>(index)] : "?";
        const float arrows = count > 1 ? style.fontSize : 0.0f;
        ui.ClippedText(min + style.framePadding, max.x - arrows - 2.0f, text, style.text);
        if (count > 1) {
            // Deux petites flèches : la valeur se fait défiler.
            const float size = style.fontSize * 0.3f;
            const float x = max.x - style.framePadding.x - size;
            const float centerY = (min.y + max.y) * 0.5f;
            list.AddTriangle({x, centerY - 1.0f}, {x + size * 0.5f, centerY - 1.0f - size * 0.6f}, {x + size, centerY - 1.0f},
                             style.text);
            list.AddTriangle({x, centerY + 1.0f}, {x + size, centerY + 1.0f}, {x + size * 0.5f, centerY + 1.0f + size * 0.6f},
                             style.text);
        }
        return index != previous;
    }

    bool Context::CollapsingHeader(std::string_view label, bool defaultOpen) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        bool& open = ui.treeOpen.try_emplace(id, defaultOpen).first->second;
        if (ui.nextTreeOpen) {
            open = *ui.nextTreeOpen;
            ui.nextTreeOpen.reset();
        }

        const float height = ui.FrameHeight();
        const Vector2f min = ui.ItemRect({ui.RowWidth(), height});
        const Vector2f max = min + Vector2f{ui.RowWidth(), height};
        const Impl::Interaction interaction = ui.Behavior(id, min, max);
        if (interaction.clicked) {
            open = !open;
        }

        DrawList& list = ui.panel->content;
        list.AddRect(min, max, interaction.hovered ? style.buttonHovered : style.button);
        const float arrowWidth = style.fontSize;
        const float size = style.fontSize * 0.45f;
        const float x = min.x + style.framePadding.x * 0.5f + (arrowWidth - size) * 0.5f;
        const float centerY = min.y + height * 0.5f;
        if (open) {
            list.AddTriangle({x, centerY - size * 0.35f}, {x + size, centerY - size * 0.35f},
                             {x + size * 0.5f, centerY + size * 0.45f}, style.text);
        } else {
            list.AddTriangle({x + size * 0.15f, centerY - size * 0.5f}, {x + size * 0.15f, centerY + size * 0.5f},
                             {x + size * 0.95f, centerY}, style.text);
        }
        ui.ClippedText({min.x + style.framePadding.x * 0.5f + arrowWidth + 2.0f, min.y + style.framePadding.y},
                       max.x - 2.0f, VisibleLabel(label), style.text);
        return open;
    }

    Context::TreeNodeResult Context::TreeNode(std::string_view label, bool selected, bool leaf, bool defaultOpen) {
        Impl& ui = *m_impl;
        TreeNodeResult result;
        if (!ui.panel) {
            return result;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        bool& open = ui.treeOpen.try_emplace(id, defaultOpen).first->second;
        if (ui.nextTreeOpen) {
            open = *ui.nextTreeOpen;
            ui.nextTreeOpen.reset();
        }

        const float rowHeight = ui.TreeRowHeight();
        const float rowWidth = ui.RowWidth();
        const Vector2f min = ui.ItemRect({rowWidth, rowHeight}, 1.0f);
        const Vector2f max = min + Vector2f{rowWidth, rowHeight};
        const float arrowWidth = style.fontSize;
        const Impl::Interaction interaction = ui.Behavior(id, min, max);
        if (interaction.clicked) {
            if (!leaf && ui.mouse.x < min.x + arrowWidth) {
                open = !open; // clic sur la flèche : déplier sans sélectionner
            } else {
                result.clicked = true;
                const bool doubleClick = ui.lastClickId == id && ui.time - ui.lastClickTime < 0.35;
                result.doubleClicked = doubleClick;
                if (doubleClick && !leaf) {
                    open = !open;
                }
                ui.lastClickId = doubleClick ? 0 : id;
                ui.lastClickTime = ui.time;
            }
        }

        DrawList& list = ui.panel->content;
        if (selected) {
            list.AddRect(min, max, style.selection);
        } else if (interaction.hovered) {
            list.AddRect(min, max, style.frame);
        }
        if (!leaf) {
            const float size = style.fontSize * 0.45f;
            const float x = min.x + (arrowWidth - size) * 0.5f;
            const float centerY = min.y + rowHeight * 0.5f;
            if (open) {
                list.AddTriangle({x, centerY - size * 0.35f}, {x + size, centerY - size * 0.35f},
                                 {x + size * 0.5f, centerY + size * 0.45f}, style.text);
            } else {
                list.AddTriangle({x + size * 0.15f, centerY - size * 0.5f}, {x + size * 0.15f, centerY + size * 0.5f},
                                 {x + size * 0.95f, centerY}, style.text);
            }
        }
        ui.ClippedText({min.x + arrowWidth, min.y + 2.0f}, max.x - 2.0f, VisibleLabel(label), style.text);

        result.open = open && !leaf;
        if (result.open) {
            ui.indent += arrowWidth;
        }
        return result;
    }

    void Context::TreePop() {
        Impl& ui = *m_impl;
        ui.indent = std::max(0.0f, ui.indent - ui.style.fontSize);
    }

    void Context::SetNextTreeNodeOpen(bool open) {
        m_impl->nextTreeOpen = open;
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
