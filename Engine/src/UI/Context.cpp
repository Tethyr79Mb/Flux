#include "ContextImpl.hpp"

#include <iterator>

namespace Flux::UI {
    namespace {
        using C = LinearColor;

        constexpr std::string_view ThemeNames[] = {"Sombre", "Clair", "Graphite", "Ars Solaris"};
        static_assert(std::size(ThemeNames) == static_cast<std::size_t>(Theme::Count));
    }

    Style Style::Dark() {
        Style style;
        style.text = C::FromSrgb8(226, 229, 235);
        style.textDisabled = C::FromSrgb8(134, 140, 152);
        style.windowBackground = C::FromSrgb8(17, 18, 22);
        style.panelBackground = C::FromSrgb8(33, 35, 41, 0.96f);
        style.panelBorder = C::FromSrgb8(0, 0, 0, 0.55f);
        style.titleBackground = C::FromSrgb8(24, 26, 31);
        style.titleBackgroundActive = C::FromSrgb8(38, 58, 92);
        style.frame = C::FromSrgb8(22, 24, 29);
        style.frameHovered = C::FromSrgb8(52, 56, 66);
        style.frameActive = C::FromSrgb8(64, 69, 80);
        style.button = C::FromSrgb8(52, 56, 66);
        style.buttonHovered = C::FromSrgb8(64, 70, 82);
        style.buttonActive = C::FromSrgb8(44, 48, 57);
        style.header = C::FromSrgb8(44, 47, 55);
        style.headerHovered = C::FromSrgb8(54, 58, 68);
        style.checkMark = C::FromSrgb8(88, 156, 255);
        style.selection = C::FromSrgb8(52, 108, 210, 0.85f);
        style.separator = C::FromSrgb8(52, 55, 63);
        style.axisX = C::FromSrgb8(226, 76, 76);
        style.axisY = C::FromSrgb8(110, 196, 76);
        style.axisZ = C::FromSrgb8(76, 136, 236);
        style.shadow = C::FromSrgb8(0, 0, 0, 0.5f);
        return style;
    }

    Style Style::Light() {
        Style style;
        style.text = C::FromSrgb8(28, 30, 34);
        style.textDisabled = C::FromSrgb8(122, 126, 134);
        style.windowBackground = C::FromSrgb8(200, 203, 210);
        style.panelBackground = C::FromSrgb8(238, 239, 242, 0.97f);
        style.panelBorder = C::FromSrgb8(0, 0, 0, 0.28f);
        style.titleBackground = C::FromSrgb8(214, 217, 223);
        style.titleBackgroundActive = C::FromSrgb8(164, 196, 240);
        style.frame = C::FromSrgb8(255, 255, 255);
        style.frameHovered = C::FromSrgb8(230, 237, 248);
        style.frameActive = C::FromSrgb8(208, 220, 240);
        style.button = C::FromSrgb8(200, 215, 238);
        style.buttonHovered = C::FromSrgb8(178, 200, 234);
        style.buttonActive = C::FromSrgb8(152, 180, 224);
        style.header = C::FromSrgb8(222, 225, 232);
        style.headerHovered = C::FromSrgb8(210, 216, 228);
        style.checkMark = C::FromSrgb8(36, 110, 214);
        style.selection = C::FromSrgb8(120, 170, 240, 0.6f);
        style.separator = C::FromSrgb8(196, 199, 206);
        style.axisX = C::FromSrgb8(200, 50, 50);
        style.axisY = C::FromSrgb8(60, 150, 50);
        style.axisZ = C::FromSrgb8(50, 100, 210);
        style.shadow = C::FromSrgb8(0, 0, 0, 0.2f);
        return style;
    }

    Style Style::Graphite() {
        Style style;
        style.text = C::FromSrgb8(222, 222, 222);
        style.textDisabled = C::FromSrgb8(130, 130, 130);
        style.windowBackground = C::FromSrgb8(20, 20, 20);
        style.panelBackground = C::FromSrgb8(40, 40, 40, 0.96f);
        style.panelBorder = C::FromSrgb8(0, 0, 0, 0.7f);
        style.titleBackground = C::FromSrgb8(28, 28, 28);
        style.titleBackgroundActive = C::FromSrgb8(104, 62, 26);
        style.frame = C::FromSrgb8(24, 24, 24);
        style.frameHovered = C::FromSrgb8(34, 34, 34);
        style.frameActive = C::FromSrgb8(46, 46, 46);
        style.button = C::FromSrgb8(64, 64, 64);
        style.buttonHovered = C::FromSrgb8(80, 80, 80);
        style.buttonActive = C::FromSrgb8(52, 52, 52);
        style.header = C::FromSrgb8(52, 52, 52);
        style.headerHovered = C::FromSrgb8(62, 62, 62);
        style.checkMark = C::FromSrgb8(242, 150, 48);
        style.selection = C::FromSrgb8(196, 108, 30, 0.75f);
        style.separator = C::FromSrgb8(60, 60, 60);
        style.axisX = C::FromSrgb8(210, 70, 70);
        style.axisY = C::FromSrgb8(100, 180, 70);
        style.axisZ = C::FromSrgb8(70, 125, 220);
        style.panelRounding = 3.0f;
        style.frameRounding = 2.0f;
        style.shadow = C::FromSrgb8(0, 0, 0, 0.5f);
        return style;
    }

    Style Style::ArsSolaris() {
        Style style;
        style.text = C::FromSrgb8(212, 236, 246);
        style.textDisabled = C::FromSrgb8(108, 138, 158);
        style.windowBackground = C::FromSrgb8(4, 9, 18);
        style.panelBackground = C::FromSrgb8(8, 16, 30, 0.93f);
        style.panelBorder = C::FromSrgb8(40, 170, 220, 0.55f);
        style.titleBackground = C::FromSrgb8(10, 22, 40);
        style.titleBackgroundActive = C::FromSrgb8(14, 72, 104);
        style.frame = C::FromSrgb8(14, 30, 50);
        style.frameHovered = C::FromSrgb8(20, 44, 70);
        style.frameActive = C::FromSrgb8(28, 60, 92);
        style.button = C::FromSrgb8(16, 72, 100);
        style.buttonHovered = C::FromSrgb8(22, 100, 136);
        style.buttonActive = C::FromSrgb8(12, 56, 80);
        style.header = C::FromSrgb8(12, 34, 56);
        style.headerHovered = C::FromSrgb8(18, 48, 76);
        style.checkMark = C::FromSrgb8(70, 220, 250);
        style.selection = C::FromSrgb8(30, 140, 190, 0.7f);
        style.separator = C::FromSrgb8(30, 70, 100);
        style.axisX = C::FromSrgb8(230, 80, 80);
        style.axisY = C::FromSrgb8(110, 200, 80);
        style.axisZ = C::FromSrgb8(80, 140, 240);
        // Coins presque droits et halo cyan au lieu d'une ombre : une interface de vaisseau.
        style.panelRounding = 2.0f;
        style.frameRounding = 2.0f;
        style.shadowSize = 18.0f;
        style.shadow = C::FromSrgb8(40, 190, 240, 0.22f);
        return style;
    }

    Style Style::FromTheme(Theme theme) {
        switch (theme) {
            case Theme::Light:
                return Light();
            case Theme::Graphite:
                return Graphite();
            case Theme::ArsSolaris:
                return ArsSolaris();
            case Theme::Dark:
            case Theme::Count:
                break;
        }
        return Dark();
    }

    std::span<const std::string_view> GetThemeNames() {
        return ThemeNames;
    }

    Theme ThemeFromName(std::string_view name) {
        const auto found = std::ranges::find(ThemeNames, name);
        return static_cast<Theme>(found - std::begin(ThemeNames));
    }

    Context::Context(const Font& font, const Style& style) : m_impl(std::make_unique<Impl>(font, style)) {}

    Context::~Context() = default;

    void Context::BeginFrame(const Input& input, Math::Vector2f screenSize, double deltaSeconds, bool mouseEnabled) {
        Impl& ui = *m_impl;
        if (ui.disabledDepth > 0) {
            ui.style.text = ui.enabledText; // EndDisabled oublié : pas de grisé qui déborde sur la frame suivante
        }
        ui.disabledStack.clear();
        ui.disabledDepth = 0;
        ui.input = &input;
        ui.cursor = MouseCursor::Arrow;
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
            // Panneaux flottants (et leurs bords, à saisir pour les redimensionner), puis panneaux ancrés (dessous).
            for (const bool docked : {false, true}) {
                const Vector2f margin = docked ? Vector2f{} : Vector2f{ResizeGrip, ResizeGrip};
                for (auto it = ui.order.rbegin(); it != ui.order.rend() && ui.hoveredPanel == 0; ++it) {
                    const Impl::Panel& panel = *ui.panels.at(*it);
                    if (panel.docked == docked && !panel.hiddenTab && panel.lastFrame + 1 == ui.frame
                        && Contains(panel.position - margin, panel.position + Vector2f{panel.width, panel.height} + margin,
                                    ui.mouse)) {
                        ui.hoveredPanel = *it;
                    }
                }
            }
        }
        // Séparateur ou zone ancrée sans panneau : la souris n'est pas à la vue 3D (zone centrale) pour autant.
        ui.dockHovered = mouseEnabled && ui.hoveredPanel == 0 && ui.dockFrame != 0 && ui.dockFrame + 1 == ui.frame
                      && Contains(ui.dockArea.min, ui.dockArea.max, ui.mouse)
                      && !Contains(ui.dockCentral.min, ui.dockCentral.max, ui.mouse);
        // Menu ouvert : un clic ailleurs (ou Échap) le referme, sans rien faire d'autre.
        ui.clickSwallowed = false;
        if (ui.openMenu != 0) {
            const bool overMenu = ui.hoveredPanel != 0 && ui.panels.at(ui.hoveredPanel)->overlay;
            if (ui.mousePressed && !overMenu) {
                ui.openMenu = 0;
                ui.mousePressed = false;
                ui.clickSwallowed = true;
            } else if (mouseEnabled && input.WasMouseButtonPressed(MouseButton::Right) && !overMenu) {
                ui.openMenu = 0; // un autre clic droit peut rouvrir un menu contextuel ailleurs dans la même frame
            } else if (input.WasKeyPressed(Key::Escape)) {
                ui.openMenu = 0;
            }
        }
        // Un clic sur un panneau flottant le passe au premier plan.
        if (ui.mousePressed && ui.hoveredPanel != 0 && !ui.panels.at(ui.hoveredPanel)->overlay
            && !ui.panels.at(ui.hoveredPanel)->docked && ui.order.back() != ui.hoveredPanel) {
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

        // Ancrage (onglets, séparateurs), panneaux ancrés, panneaux flottants, cibles de dépôt, puis menus.
        if (ui.dockFrame == ui.frame) {
            output.Append(ui.dockChrome);
        }
        for (const bool docked : {true, false}) {
            for (const WidgetId id : ui.order) {
                const Impl::Panel& panel = *ui.panels.at(id);
                if (panel.lastFrame == ui.frame && panel.docked == docked && !panel.hiddenTab) {
                    output.Append(panel.chrome);
                    output.Append(panel.content);
                }
            }
        }
        if (ui.dockFrame == ui.frame) {
            output.Append(ui.dockTargets); // cibles de dépôt, par-dessus le panneau glissé
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
        return ui.hoveredPanel != 0 || ui.activeId != 0 || ui.openMenu != 0 || ui.clickSwallowed || ui.dockHovered;
    }

    bool Context::WantsKeyboard() const {
        return m_impl->editingId != 0;
    }

    bool Context::IsInteracting() const {
        return m_impl->activeId != 0 || m_impl->editingId != 0;
    }

    bool Context::IsItemActive() const {
        return m_impl->activeId != 0 && m_impl->activeId == m_impl->lastBehaviorId;
    }

    Rect Context::GetItemRect() const {
        return {m_impl->lastBehaviorMin, m_impl->lastBehaviorMax};
    }

    bool Context::IsMouseOverItem() const {
        return m_impl->IsMouseOver(m_impl->lastBehaviorMin, m_impl->lastBehaviorMax);
    }

    DrawList* Context::GetPanelDrawList() {
        return m_impl->panel ? &m_impl->panel->content : nullptr;
    }

    Math::Vector2f Context::MeasureText(std::string_view text) const {
        const Impl& ui = *m_impl;
        return ui.measure.MeasureText(text, ui.style.fontSize);
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

    void Context::SetColorPicker(ColorPicker picker) {
        m_impl->colorPicker = std::move(picker);
    }

    const Style& Context::GetStyle() const {
        return m_impl->style;
    }

    void Context::SetStyle(const Style& style) {
        Impl& ui = *m_impl;
        ui.style = style;
        if (ui.disabledDepth > 0) {
            ui.enabledText = style.text; // EndDisabled remettra la couleur du nouveau thème
            ui.style.text = style.textDisabled;
        }
    }

    MouseCursor Context::GetMouseCursor() const {
        return m_impl->cursor;
    }

    void Context::SetMouseCursor(MouseCursor cursor) {
        if (m_impl->cursor == MouseCursor::Arrow) {
            m_impl->cursor = cursor;
        }
    }
}
