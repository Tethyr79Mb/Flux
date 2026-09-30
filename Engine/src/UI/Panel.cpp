// Panneaux : flottants (barre de titre, repli, défilement) ou ancrés (Docking.cpp décide de leur place).
#include "ContextImpl.hpp"

namespace Flux::UI {
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
        panel.fullTitle = title;
        panel.floatingWidth = width;
        panel.fixedHeight = height;
        panel.lastFrame = ui.frame;
        panel.chrome.Clear();
        panel.content.Clear();
        ui.panel = &panel;
        ui.panelId = id;
        ui.idStack.push_back(id);
        const float titleHeight = ui.FrameHeight();

        // Ancré (DockSpace décrit dans cette frame) : place et taille de la zone, la barre d'onglets en guise de titre.
        const auto dock = ui.DockActive() ? ui.dockedPanels.find(id) : ui.dockedPanels.end();
        panel.docked = dock != ui.dockedPanels.end();
        panel.hiddenTab = false;
        if (panel.docked) {
            DockNode& zone = ui.dockNodes[dock->second];
            if (zone.selected == 0) {
                zone.selected = id; // premier panneau apparu dans une zone vide : affiché tout de suite
            }
            panel.hiddenTab = zone.selected != id;
            panel.collapsed = false;
            panel.position = zone.min;
            panel.width = zone.max.x - zone.min.x;
            panel.fixedHeight = std::max(titleHeight, zone.max.y - zone.min.y);
            if (panel.hiddenTab) {
                ui.clipMin = ui.clipMax = panel.position; // contenu décrit quand même : rien n'apparaît
                ui.contentMin = panel.position;
                ui.contentMaxX = panel.position.x;
                ui.cursorY = panel.position.y;
                ui.RestorePanelClip();
                return false;
            }
        }

        // Flottant : barre de titre à glisser pour déplacer, à cliquer pour replier.
        if (!panel.docked) {
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
                    ui.draggedPanel = id; // DockSpace montre où il peut s'ancrer
                    ui.draggedFrame = ui.frame;
                }
            }
            if (titleBar.clicked && !ui.dragging) {
                panel.collapsed = !panel.collapsed;
            }
            if (ui.DockActive() && !(titleBar.held && ui.dragging)) {
                // Avec un ancrage, les panneaux flottants (ceux du jeu) restent dans la zone centrale, la vue du jeu.
                const Rect& area = ui.dockCentral;
                panel.position.x = std::clamp(panel.position.x, area.min.x, std::max(area.min.x, area.max.x - panel.width));
                panel.position.y = std::clamp(panel.position.y, area.min.y, std::max(area.min.y, area.max.y - titleHeight));
            } else {
                // La barre de titre reste toujours attrapable, même si la fenêtre a rétréci.
                panel.position.x = std::clamp(panel.position.x, 40.0f - panel.width, std::max(0.0f, ui.screenSize.x - 40.0f));
                // Jamais sous la barre de menus.
                panel.position.y = std::clamp(panel.position.y, ui.menuBarHeight,
                                              std::max(ui.menuBarHeight, ui.screenSize.y - titleHeight));
            }
        }

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

    void Context::BeginChild(std::string_view label, float width, float height) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        // Sur la même ligne qu'un widget, la zone commence en haut de celui-ci, pas sous lui (cursorY y est déjà).
        const float left = ui.sameLine ? ui.lastItemMax.x + style.itemSpacing : ui.contentMin.x + ui.indent;
        const float top = ui.sameLine ? ui.lastItemMin.y : ui.cursorY;
        const float bottom = ui.panel->fixedHeight > 0.0f ? ui.panel->position.y + ui.panel->fixedHeight - style.panelPadding
                                                         : top + 200.0f;
        const Vector2f size{width > 0.0f ? width : std::max(1.0f, ui.contentMaxX - left),
                            height > 0.0f ? height : std::max(1.0f, bottom - top)};
        const Vector2f min = ui.ItemRect(size);
        const Vector2f max = min + size;

        Impl::ChildScroll& scroll = ui.childScrolls[id];
        const float padding = 4.0f;
        const float viewHeight = size.y - 2.0f * padding;
        const float maxScroll = std::max(0.0f, scroll.contentHeight - viewHeight);
        if (ui.wheel != 0.0f && ui.IsMouseOver(min, max)) {
            scroll.scrollY -= ui.wheel * 3.0f * (ui.TreeRowHeight() + 1.0f);
            ui.wheel = 0.0f; // la zone la plus proche défile, pas celles qui l'entourent
        }
        scroll.scrollY = std::clamp(scroll.scrollY, 0.0f, maxScroll);

        ui.panel->content.AddRect(min, max, style.frame);
        ui.childStack.push_back({ui.SaveLayout(), id, min, max, min.y + padding});
        ui.idStack.push_back(id);
        const float scrollbarWidth = maxScroll > 0.0f ? 8.0f : 0.0f;
        ui.contentMin = {min.x + padding, min.y + padding - scroll.scrollY};
        ui.contentMaxX = max.x - padding - scrollbarWidth;
        ui.cursorY = ui.contentMin.y;
        ui.indent = 0.0f;
        ui.sameLine = false;
        ui.clipMin = {std::max(min.x, ui.clipMin.x), std::max(min.y, ui.clipMin.y)};
        ui.clipMax = {std::min(max.x, ui.clipMax.x), std::min(max.y, ui.clipMax.y)};
        ui.RestorePanelClip();
    }

    void Context::EndChild() {
        Impl& ui = *m_impl;
        if (!ui.panel || ui.childStack.empty()) {
            return;
        }
        const Impl::ChildFrame child = ui.childStack.back();
        ui.childStack.pop_back();
        Impl::ChildScroll& scroll = ui.childScrolls[child.id];
        const bool empty = ui.cursorY == ui.contentMin.y;
        scroll.contentHeight = empty ? 0.0f : ui.cursorY - ui.lastSpacing - (child.contentTop - scroll.scrollY);

        ui.RestoreLayout(child.parent);
        ui.idStack.pop_back();
        ui.RestorePanelClip();
        // Barre de défilement (indicateur) ; la molette fait défiler.
        const float viewHeight = child.max.y - child.min.y - 8.0f;
        const float maxScroll = std::max(0.0f, scroll.contentHeight - viewHeight);
        if (maxScroll > 0.0f) {
            const float trackTop = child.min.y + 3.0f;
            const float trackHeight = child.max.y - child.min.y - 6.0f;
            const float thumbHeight = std::max(20.0f, trackHeight * viewHeight / scroll.contentHeight);
            const float thumbY = trackTop + (trackHeight - thumbHeight) * scroll.scrollY / maxScroll;
            ui.panel->content.AddRect({child.max.x - 7.0f, thumbY}, {child.max.x - 3.0f, thumbY + thumbHeight},
                                      ui.style.frameActive);
        }
        // La zone entière est le dernier widget : SameLine, clic droit dans son fond.
        ui.lastItemMin = child.min;
        ui.lastItemMax = child.max;
    }

    float Context::GetAvailableWidth() const {
        const Impl& ui = *m_impl;
        if (!ui.panel) {
            return 0.0f;
        }
        const float left = ui.sameLine ? ui.lastItemMax.x + ui.style.itemSpacing : ui.contentMin.x + ui.indent;
        return std::max(0.0f, ui.contentMaxX - left);
    }

    void Context::EndPanel() {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        Impl::Panel& panel = *ui.panel;
        if (panel.hiddenTab) {
            panel.content.Clear(); // décrit sans être affiché
            ui.idStack.pop_back();
            ui.panel = nullptr;
            ui.panelId = 0;
            return;
        }
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
        if (panel.docked) {
            // Opaque, et sans barre de titre : la barre d'onglets de la zone est dessinée par DockSpace.
            LinearColor background = style.panelBackground;
            background.a = 1.0f;
            chrome.AddRect({min.x, viewTop}, max, background);
        } else {
            chrome.AddRect(min, max, style.panelBackground);
            const bool front = ui.order.back() == ui.panelId;
            chrome.AddRect(min, {max.x, viewTop}, front ? style.titleBackgroundActive : style.titleBackground);

            // Flèche : vers le bas si déplié, vers la droite si replié.
            const float arrow = style.fontSize * 0.5f;
            const float arrowX = min.x + style.panelPadding;
            const float centerY = min.y + titleHeight * 0.5f;
            if (panel.collapsed) {
                chrome.AddTriangle({arrowX + arrow * 0.15f, centerY - arrow * 0.5f},
                                   {arrowX + arrow * 0.15f, centerY + arrow * 0.5f}, {arrowX + arrow * 0.95f, centerY},
                                   style.text);
            } else {
                chrome.AddTriangle({arrowX, centerY - arrow * 0.35f}, {arrowX + arrow, centerY - arrow * 0.35f},
                                   {arrowX + arrow * 0.5f, centerY + arrow * 0.45f}, style.text);
            }
            const float titleX = arrowX + arrow + style.framePadding.x;
            chrome.SetClipRect({titleX, min.y}, {max.x - style.framePadding.x, viewTop});
            chrome.AddText({titleX, min.y + style.framePadding.y}, panel.title, style.fontSize, style.text);
            chrome.ResetClipRect();
        }

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
        if (!panel.docked) {
            chrome.AddRectOutline(min, max, style.panelBorder);
        }

        panel.content.ResetClipRect();
        ui.idStack.pop_back();
        ui.panel = nullptr;
        ui.panelId = 0;
    }
}
