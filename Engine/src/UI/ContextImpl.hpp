// Détails internes de UI::Context, partagés par ses fichiers sources (Context.cpp, Docking.cpp) : pas d'en-tête public.
#pragma once

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
    namespace Detail {
        using Math::LinearColor;
        using Math::Vector2f;

        constexpr WidgetId RootId = 14695981039346656037ull; // base FNV-1a

        // FNV-1a 64 bits : rapide, et suffisant pour quelques centaines de widgets par panneau.
        inline WidgetId Hash(WidgetId seed, const void* data, std::size_t size) {
            WidgetId hash = seed;
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < size; ++i) {
                hash = (hash ^ bytes[i]) * 1099511628211ull;
            }
            return hash == 0 ? 1 : hash; // 0 signifie « aucun widget »
        }

        inline WidgetId Hash(WidgetId seed, std::string_view text) {
            return Hash(seed, text.data(), text.size());
        }

        // "Position##soleil" s'affiche "Position" mais s'identifie par le libellé complet.
        inline std::string_view VisibleLabel(std::string_view label) {
            const std::size_t hidden = label.find("##");
            return hidden == std::string_view::npos ? label : label.substr(0, hidden);
        }

        inline bool Contains(Vector2f min, Vector2f max, Vector2f point) {
            return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
        }

        inline void AddLine(DrawList& list, Vector2f a, Vector2f b, float thickness, LinearColor color) {
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

        inline bool IsNumberCharacter(char c) {
            return (c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-' || c == '+' || c == 'e' || c == 'E';
        }

        inline std::string FormatNumber(double value, int decimals) {
            std::string text = std::format("{:.{}f}", value, std::clamp(decimals, 0, 12));
            // "-0.000" : un zéro arrondi n'a pas de signe.
            if (text.starts_with('-') && text.find_first_not_of("-0.") == std::string::npos) {
                text.erase(0, 1);
            }
            return text;
        }

        // Pour l'édition : "9.000" devient "9", "0.250" devient "0.25".
        inline std::string FormatEditable(double value, int decimals) {
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
        inline std::size_t PreviousBoundary(std::string_view text, std::size_t index) {
            while (index > 0) {
                --index;
                if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
                    break;
                }
            }
            return index;
        }

        inline std::size_t NextBoundary(std::string_view text, std::size_t index) {
            if (index < text.size()) {
                ++index;
            }
            while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
                ++index;
            }
            return index;
        }

        inline std::optional<double> ParseNumber(std::string text) {
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

        // Zone d'ancrage (Docking.cpp).
        struct DockNode {
            DockId children[2]{-1, -1};      // zone découpée : ses deux parties ; -1 : zone feuille
            bool horizontal = false;         // découpée en gauche | droite (sinon haut / bas)
            float ratio = 0.5f;              // part de la première partie
            std::vector<std::string> panels; // feuille : titres des onglets, dans l'ordre
            WidgetId selected = 0;           // onglet affiché
            bool central = false;            // la zone libre (vue 3D) : ni panneaux, ni fond
            Vector2f min;                    // rectangle de la frame en cours
            Vector2f max;

            bool IsLeaf() const { return children[0] < 0; }
        };
    }
    using namespace Detail; // en-tête interne : seulement inclus par les sources de FluxUI

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
            bool docked = false;        // ancré à la dernière frame (sa zone décide de sa place)
            bool hiddenTab = false;     // ancré derrière un autre onglet : ni dessiné, ni survolé
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

        Impl(const Font& uiFont, const Style& uiStyle) : font(uiFont), style(uiStyle), measure(uiFont), dockChrome(uiFont) {}

        const Font& font;
        Style style;
        DrawList measure; // mesure du texte hors de tout panneau

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
        WidgetId openMenu = 0;                  // titre du menu ouvert (ou menu contextuel)
        Vector2f contextPosition;               // où le menu contextuel ouvert est apparu
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

        // Ancrage (Docking.cpp) : arbre de zones stocké à plat, la racine en 0.
        std::vector<DockNode> dockNodes;
        std::unordered_map<WidgetId, DockId> dockedPanels; // panneau -> zone, reconstruit à chaque changement
        std::uint64_t dockFrame = 0;                       // frame du dernier DockSpace
        Rect dockArea;
        Rect dockCentral;
        bool dockHovered = false; // souris sur un séparateur ou une zone vide : ni l'UI ni la vue 3D ne la prennent
        DrawList dockChrome;      // séparateurs, barres d'onglets et zones vides, sous tous les panneaux

        void RebuildDockIndex();
        void LayoutDock(DockId id, Vector2f min, Vector2f max);
        void DockSplitters();
        void DockTabs(DockNode& node);
        bool DockMouseFree() const;
        // DockSpace décrit dans cette frame ou la précédente : les panneaux du jeu, décrits avant ceux de l'éditeur,
        // se placent d'après la disposition de la frame précédente.
        bool DockActive() const { return dockFrame != 0 && dockFrame + 1 >= frame; }

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
        // Menu déroulant ou contextuel en position : le panneau en cours est mis de côté jusqu'à EndMenu.
        void BeginPopup(WidgetId id, Vector2f position) {
            openMenuSeen = true;
            menuParent = SaveLayout();
            Panel& popup = BeginOverlay(id);
            // Largeur des éléments mesurée à la frame précédente (la première frame, une largeur minimale).
            popup.width = std::max(180.0f, popup.measuredWidth);
            popup.position = {std::clamp(position.x, 0.0f, std::max(0.0f, screenSize.x - popup.width)),
                              std::clamp(position.y, 0.0f, std::max(0.0f, screenSize.y - popup.height))};
            const float padding = 4.0f;
            contentMin = popup.position + Vector2f{padding, padding};
            contentMaxX = popup.position.x + popup.width - padding;
            cursorY = contentMin.y;
            lastSpacing = 0.0f;
            clipMin = popup.position;
            clipMax = {popup.position.x + popup.width, screenSize.y};
            RestorePanelClip();
            menuItemWidth = 0.0f;
        }

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
}
