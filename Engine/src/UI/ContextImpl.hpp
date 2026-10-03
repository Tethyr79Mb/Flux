// Détails internes de UI::Context, partagés par ses fichiers sources (Context.cpp, Panel.cpp, Widgets.cpp,
// Fields.cpp, Menus.cpp, Tree.cpp, Docking.cpp) : pas d'en-tête public.
#pragma once

#include "Flux/UI/Context.hpp"

#include "Flux/Graphics/Font.hpp"
#include "Flux/Platform/Input.hpp"
#include "Flux/UI/DrawList.hpp"

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
        constexpr float ResizeGrip = 4.0f; // bord d'un panneau flottant saisi pour le redimensionner, de part et d'autre

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
            std::string fullTitle;      // titre donné à BeginPanel, ##suffixe compris (pour l'ancrer)
            float floatingWidth = 0.0f; // largeur demandée par BeginPanel : celle reprise en sortant d'une zone
            float userWidth = 0.0f;     // largeur choisie à la souris : reprise en sortant d'une zone, avant floatingWidth
            float userHeight = 0.0f;    // hauteur choisie à la souris : remplace celle de BeginPanel (0 : aucune)
            bool closable = false;      // croix dans la barre de titre (BeginPanel avec open)
            bool closeHovered = false;
            bool closeHeld = false;
            bool docked = false;        // ancré à la dernière frame (sa zone décide de sa place)
            bool hiddenTab = false;     // ancré derrière un autre onglet : ni dessiné, ni survolé
            Icon icon = Icon::None;     // devant le titre et dans l'onglet (SetNextPanelIcon)
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

        Impl(const Font& uiFont, const Style& uiStyle)
            : font(uiFont), style(uiStyle), measure(uiFont), dockChrome(uiFont), dockTargets(uiFont) {}

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
        MouseCursor cursor = MouseCursor::Arrow; // voulu pour cette frame (GetMouseCursor)

        // Redimensionnement d'un panneau flottant à la souris : bords saisis et taille au moment de la saisie.
        int resizeEdges = 0;
        Vector2f resizeStartSize;

        // Panneaux flottants lus avec la disposition (LoadDockLayout) : place et taille reprises à leur apparition.
        struct FloatingState {
            Vector2f position;
            float width = 0.0f;
            float height = 0.0f; // userHeight
            bool collapsed = false;
        };
        std::unordered_map<WidgetId, FloatingState> savedFloating;    // pas encore réapparus
        std::unordered_map<WidgetId, std::string> savedFloatingTitles; // leur titre, pour les réenregistrer

        // Barre de menus et menu déroulant ouvert : dessinés après les panneaux, survolés avant eux.
        std::vector<WidgetId> overlays;         // décrits pendant la frame en cours
        std::vector<WidgetId> previousOverlays; // ceux de la frame précédente, pour le survol
        WidgetId openMenu = 0;                  // titre du menu ouvert (ou menu contextuel)
        Vector2f contextPosition;               // où le menu contextuel ouvert est apparu
        float contextWidth = 0.0f;              // menu ouvert par OpenPopupMenu : largeur du widget qui l'ouvre
        std::uint64_t contextOpenedFrame = 0;   // frame où un menu contextuel s'est ouvert (un seul par clic)
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

        // Zones à défilement propre (BeginChild) : défilement gardé d'une frame à l'autre, zones en cours de
        // description (imbricables) avec la mise en page du parent à reprendre.
        struct ChildScroll {
            float scrollY = 0.0f;
            float contentHeight = 0.0f; // à la dernière frame
        };
        std::unordered_map<WidgetId, ChildScroll> childScrolls;
        struct ChildFrame {
            Layout parent;
            WidgetId id = 0;
            Vector2f min;
            Vector2f max;
            float contentTop = 0.0f; // haut du contenu sans défilement
        };
        std::vector<ChildFrame> childStack;

        // Ancrage (Docking.cpp) : arbre de zones stocké à plat, la racine en 0.
        std::vector<DockNode> dockNodes;
        std::unordered_map<WidgetId, DockId> dockedPanels; // panneau -> zone, reconstruit à chaque changement
        // Onglets à afficher dès que leur panneau apparaît (FocusPanel), avec la frame de la demande.
        std::vector<std::pair<WidgetId, std::uint64_t>> focusRequests;
        std::uint64_t dockFrame = 0;                       // frame du dernier DockSpace
        Rect dockArea;
        Rect dockCentral;
        bool dockHovered = false; // souris sur un séparateur ou une zone vide : ni l'UI ni la vue 3D ne la prennent
        DrawList dockChrome;      // séparateurs, barres d'onglets et zones vides, sous tous les panneaux
        DrawList dockTargets;     // cibles de dépôt d'un panneau glissé, au-dessus des panneaux
        WidgetId draggedPanel = 0;        // panneau flottant glissé par sa barre de titre
        std::uint64_t draggedFrame = 0;   // dernière frame où il l'était

        // Où déposer un panneau glissé : au centre d'une zone (onglet) ou sur un côté (nouvelle zone).
        struct DropTarget {
            DockId zone = -1;
            bool center = false;
            DockSide side = DockSide::Left;
            Vector2f min; // carré à survoler
            Vector2f max;
            Rect preview; // place que prendrait le panneau
        };

        void ResizePanel(WidgetId id, Panel& resized, float titleHeight); // Panel.cpp

        void RebuildDockIndex();
        void LayoutDock(DockId id, Vector2f min, Vector2f max);
        Vector2f SplitterMin(const DockNode& node) const;
        Vector2f SplitterMax(const DockNode& node) const;
        WidgetId SplitterId(std::size_t index) const;
        void DockSplitterInput();
        void DrawDockSplitters();
        std::vector<WidgetId> VisibleTabs(const DockNode& node) const;
        struct TabRect {
            WidgetId id = 0;
            Vector2f min;
            Vector2f max;
        };
        std::vector<TabRect> TabRects(const DockNode& node) const;
        WidgetId DockTabInput(DockNode& node);
        void DrawDockTabs(const DockNode& node);
        void UndockPanel(WidgetId id);
        DockId SplitZone(DockId zone, DockSide side, float fraction);
        void RemoveEmptyZones();
        void CompactDock();
        DockId DockLeafAt(Vector2f point) const;
        std::vector<DropTarget> DropTargets(DockId zone) const;
        void DockDropInput();
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
        WidgetId lastBehaviorId = 0; // dernier widget interactif décrit (IsItemActive)
        std::vector<bool> disabledStack; // un élément par BeginDisabled en cours
        int disabledDepth = 0;           // BeginDisabled(true) en cours : les widgets sont affichés mais inertes
        LinearColor enabledText;         // couleur du texte avant le premier BeginDisabled(true)
        Vector2f lastBehaviorMin;    // et son rectangle (GetItemRect)
        Vector2f lastBehaviorMax;
        Context::ColorPicker colorPicker; // ouvert par l'aperçu de ColorEdit3
        bool activeSeen = false;
        Vector2f pressPosition;
        Vector2f dragOrigin;
        bool dragging = false;

        // Arbres : nœuds dépliés, et dernier clic pour reconnaître un double-clic.
        std::unordered_map<WidgetId, bool> treeOpen;
        std::optional<bool> nextTreeOpen;
        Icon nextPanelIcon = Icon::None;
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
        float TreeRowHeight() const { return LineHeight() + 6.0f; }

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

        // Menus (Menus.cpp). Barre de menus ou menu déroulant : devient le panneau en cours, hors de l'ordre des
        // panneaux. Menu déroulant ou contextuel en position : le panneau en cours est mis de côté jusqu'à EndMenu.
        // minWidth : liste déroulante au moins aussi large que son champ.
        void BeginPopup(WidgetId id, Vector2f position, float minWidth = 180.0f);
        Panel& BeginOverlay(WidgetId id);

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
            lastBehaviorId = id;
            lastBehaviorMin = min;
            lastBehaviorMax = max;
            if (disabledDepth > 0) {
                return result; // inerte : ni survol, ni clic, ni glissement
            }
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

        // Fond d'un bouton, d'un champ ou d'une case : coins arrondis du thème.
        void FrameRect(Vector2f min, Vector2f max, LinearColor color) {
            panel->content.AddRoundedRect(min, max, color, style.frameRounding);
        }

        // Libellé à gauche d'une ligne de champ ; retourne le rectangle restant pour le champ.
        std::pair<Vector2f, Vector2f> LabeledRow(std::string_view label) {
            const Vector2f min = ItemRect({RowWidth(), FrameHeight()});
            const float labelWidth = std::floor(RowWidth() * style.labelFraction);
            ClippedText({min.x, min.y + style.framePadding.y}, min.x + labelWidth - style.itemSpacing,
                        VisibleLabel(label), style.text);
            return {{min.x + labelWidth, min.y}, {min.x + RowWidth(), min.y + FrameHeight()}};
        }

        // Saisie au clavier et champs numériques (Fields.cpp).
        void BeginEdit(WidgetId id, std::string text);
        // Frappe dans le champ en cours de saisie ; numeric : seuls les caractères d'un nombre sont acceptés.
        EditResult ProcessTextEdit(Vector2f min, Vector2f max, bool numeric);
        void DrawTextEdit(Vector2f min, Vector2f max);
        // Champ texte dans min-max (InputText, InputTextBrowse) : true quand une nouvelle valeur est validée.
        bool TextField(WidgetId id, Vector2f min, Vector2f max, std::string& value);
        bool DragScalar(WidgetId id, Vector2f min, Vector2f max, double& value, double speed, double low, double high,
                        int decimals, const LinearColor* axisColor);

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
