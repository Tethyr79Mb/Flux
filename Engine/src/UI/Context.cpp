#include "ContextImpl.hpp"

namespace Flux::UI {
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
            // Panneaux flottants, puis panneaux ancrés (toujours dessous).
            for (const bool docked : {false, true}) {
                for (auto it = ui.order.rbegin(); it != ui.order.rend() && ui.hoveredPanel == 0; ++it) {
                    const Impl::Panel& panel = *ui.panels.at(*it);
                    if (panel.docked == docked && !panel.hiddenTab && panel.lastFrame + 1 == ui.frame
                        && Contains(panel.position, panel.position + Vector2f{panel.width, panel.height}, ui.mouse)) {
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

    const Style& Context::GetStyle() const {
        return m_impl->style;
    }
}
