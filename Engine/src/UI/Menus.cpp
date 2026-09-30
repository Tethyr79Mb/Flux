// Barre de menus, menus déroulants et contextuels : des panneaux « overlay » au-dessus des autres.
#include "ContextImpl.hpp"

namespace Flux::UI {
    // Barre de menus ou menu déroulant : devient le panneau en cours, hors de l'ordre des panneaux.
    // Menu déroulant ou contextuel en position : le panneau en cours est mis de côté jusqu'à EndMenu.
    void Context::Impl::BeginPopup(WidgetId id, Vector2f position, float minWidth) {
        openMenuSeen = true;
        menuParent = SaveLayout();
        Panel& popup = BeginOverlay(id);
        // Largeur des éléments mesurée à la frame précédente (la première frame, une largeur minimale).
        popup.width = std::max(minWidth, popup.measuredWidth);
        popup.position = {std::clamp(position.x, 0.0f, std::max(0.0f, screenSize.x - popup.width)),
                          std::clamp(position.y, 0.0f, std::max(0.0f, screenSize.y - popup.height))};
        const float padding = 4.0f;
        contentMin = popup.position + Vector2f{padding, padding};
        contentMaxX = popup.position.x + popup.width - padding;
        cursorY = contentMin.y;
        indent = 0.0f; // ouvert depuis un widget en retrait (arbre, section) : la liste n'en hérite pas
        sameLine = false;
        lastSpacing = 0.0f;
        clipMin = popup.position;
        clipMax = {popup.position.x + popup.width, screenSize.y};
        RestorePanelClip();
        menuItemWidth = 0.0f;
    }

    Context::Impl::Panel& Context::Impl::BeginOverlay(WidgetId id) {
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

        ui.BeginPopup(Hash(id, "#menu"), {min.x, max.y});
        return true;
    }

    bool Context::BeginContextMenu(std::string_view id) {
        Impl& ui = *m_impl;
        if (!ui.panel || ui.menuParent) {
            return false;
        }
        const WidgetId popupId = Hash(ui.GetId(id), "#contexte");
        // Clic droit sur le dernier widget décrit : le menu s'ouvre sous la souris.
        if (ui.mouseEnabled && ui.input->WasMouseButtonPressed(MouseButton::Right) && ui.contextOpenedFrame != ui.frame
            && ui.IsMouseOver(ui.lastItemMin, ui.lastItemMax)) {
            ui.openMenu = popupId;
            ui.contextPosition = ui.mouse;
            ui.contextOpenedFrame = ui.frame;
        }
        if (ui.openMenu != popupId) {
            return false;
        }
        ui.BeginPopup(popupId, ui.contextPosition);
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

    bool Context::MenuBarButton(std::string_view label, bool highlighted) {
        Impl& ui = *m_impl;
        if (!ui.panel || !ui.panel->overlay || ui.menuParent) {
            return false;
        }
        const Style& style = ui.style;
        const std::string_view visible = VisibleLabel(label);
        const Vector2f size{ui.MeasureText(visible).x + 2.0f * style.framePadding.x, ui.FrameHeight()};
        const Vector2f min{ui.menuBarNextX, 0.0f};
        const Vector2f max = min + size;
        ui.menuBarNextX = max.x + style.itemSpacing;
        const Impl::Interaction interaction = ui.Behavior(ui.GetId(label), min, max);
        const bool pressed = interaction.held && interaction.hovered;
        if (highlighted || pressed || interaction.hovered) {
            ui.panel->content.AddRect(min, max, pressed      ? style.buttonActive
                                                : highlighted ? style.button
                                                              : style.frameHovered);
        }
        ui.panel->content.AddText(min + style.framePadding, visible, style.fontSize, style.text);
        return interaction.clicked;
    }

    void Context::SetMenuBarCursor(float x) {
        m_impl->menuBarNextX = x;
    }

    float Context::GetMenuBarHeight() const {
        return m_impl->menuBarHeight;
    }
}
