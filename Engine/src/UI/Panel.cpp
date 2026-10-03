// Panneaux : flottants (barre de titre, repli, croix, défilement, bords à glisser pour les redimensionner) ou ancrés
// (Docking.cpp décide de leur place).
#include "ContextImpl.hpp"

namespace Flux::UI {
    namespace {
        constexpr float MinPanelWidth = 120.0f;
        constexpr float CornerGrip = 12.0f; // près d'un coin, sur un bord : les deux bords à la fois

        enum Edge : int { EdgeLeft = 1, EdgeRight = 2, EdgeTop = 4, EdgeBottom = 8 };

        MouseCursor ResizeCursor(int edges) {
            const bool horizontal = (edges & (EdgeLeft | EdgeRight)) != 0;
            const bool vertical = (edges & (EdgeTop | EdgeBottom)) != 0;
            if (horizontal && vertical) {
                const bool nwse = ((edges & EdgeLeft) != 0) == ((edges & EdgeTop) != 0); // haut-gauche ou bas-droit
                return nwse ? MouseCursor::ResizeNwse : MouseCursor::ResizeNesw;
            }
            return horizontal ? MouseCursor::ResizeHorizontal : MouseCursor::ResizeVertical;
        }

        // Bords d'un panneau flottant sous la souris (à ResizeGrip près, dedans ou dehors), 0 si aucun.
        // Replié, seulement la gauche et la droite. Hauteur et largeur sont celles de la frame précédente.
        int ResizeEdgesAt(const Vector2f mouse, const Vector2f min, const Vector2f max, bool collapsed) {
            if (!Contains(min - Vector2f{ResizeGrip, ResizeGrip}, max + Vector2f{ResizeGrip, ResizeGrip}, mouse)) {
                return 0;
            }
            int edges = 0;
            if (std::abs(mouse.x - min.x) <= ResizeGrip) {
                edges |= EdgeLeft;
            } else if (std::abs(mouse.x - max.x) <= ResizeGrip) {
                edges |= EdgeRight;
            }
            if (!collapsed) {
                if (std::abs(mouse.y - min.y) <= ResizeGrip) {
                    edges |= EdgeTop;
                } else if (std::abs(mouse.y - max.y) <= ResizeGrip) {
                    edges |= EdgeBottom;
                }
                // Coins plus faciles à attraper : un bord près d'un coin prend aussi l'autre.
                if ((edges & (EdgeLeft | EdgeRight)) != 0 && (edges & (EdgeTop | EdgeBottom)) == 0) {
                    edges |= mouse.y - min.y <= CornerGrip ? EdgeTop : max.y - mouse.y <= CornerGrip ? EdgeBottom : 0;
                } else if ((edges & (EdgeTop | EdgeBottom)) != 0 && (edges & (EdgeLeft | EdgeRight)) == 0) {
                    edges |= mouse.x - min.x <= CornerGrip ? EdgeLeft : max.x - mouse.x <= CornerGrip ? EdgeRight : 0;
                }
            }
            return edges;
        }
    }

    // Bords et coins d'un panneau flottant : glisser pour changer sa taille (le bord opposé ne bouge pas).
    // Avant la barre de titre : le haut du panneau est un bord avant d'être la barre.
    void Context::Impl::ResizePanel(WidgetId id, Panel& resized, float titleHeight) {
        const WidgetId resizeId = Hash(id, "#taille");
        // hoveredPanel : panneau affiché à la frame précédente, dont la taille est donc connue.
        if (activeId == 0 && mouseEnabled && hoveredPanel == id && disabledDepth == 0) {
            const int edges = ResizeEdgesAt(mouse, resized.position, resized.position + Vector2f{resized.width, resized.height},
                                            resized.collapsed);
            if (edges != 0) {
                cursor = ResizeCursor(edges);
                if (mousePressed) {
                    activeId = resizeId;
                    pressPosition = mouse;
                    dragOrigin = resized.position;
                    resizeEdges = edges;
                    resizeStartSize = {resized.width, resized.height};
                }
            }
        }
        if (activeId != resizeId) {
            return;
        }
        activeSeen = true;
        cursor = ResizeCursor(resizeEdges);
        if (mouseReleased || !mouseDown) {
            activeId = 0;
            return;
        }
        // Bornes : la zone centrale avec un ancrage (les panneaux flottants y restent), sinon l'écran sous la barre.
        const bool docking = DockActive();
        const Vector2f areaMin = docking ? dockCentral.min : Vector2f{-1.0e6f, menuBarHeight};
        const Vector2f areaMax = docking ? dockCentral.max : Vector2f{1.0e6f, screenSize.y};
        const float maxWidth = docking ? std::max(MinPanelWidth, areaMax.x - areaMin.x) : std::max(MinPanelWidth, screenSize.x);
        const float minHeight = titleHeight + 2.0f * style.panelPadding + FrameHeight();
        const float maxHeight = std::max(minHeight, areaMax.y - areaMin.y);
        const Vector2f moved = mouse - pressPosition;
        const Vector2f start = resizeStartSize;
        const Vector2f startMax = dragOrigin + start; // coin opposé à la position

        if ((resizeEdges & EdgeRight) != 0) {
            resized.width = std::clamp(start.x + moved.x, MinPanelWidth, std::min(maxWidth, areaMax.x - dragOrigin.x));
        } else if ((resizeEdges & EdgeLeft) != 0) {
            resized.width = std::clamp(start.x - moved.x, MinPanelWidth, std::min(maxWidth, startMax.x - areaMin.x));
            resized.position.x = startMax.x - resized.width;
        }
        if ((resizeEdges & EdgeBottom) != 0) {
            resized.userHeight = std::clamp(start.y + moved.y, minHeight, std::max(minHeight, std::min(maxHeight, areaMax.y - dragOrigin.y)));
        } else if ((resizeEdges & EdgeTop) != 0) {
            resized.userHeight = std::clamp(start.y - moved.y, minHeight, std::max(minHeight, std::min(maxHeight, startMax.y - areaMin.y)));
            resized.position.y = startMax.y - resized.userHeight;
        }
        if ((resizeEdges & (EdgeLeft | EdgeRight)) != 0) {
            resized.userWidth = resized.width;
        }
    }

    bool Context::BeginPanel(std::string_view title, Math::Vector2f initialPosition, float width, float height, bool* open) {
        Impl& ui = *m_impl;
        const WidgetId id = ui.GetId(title);
        auto& slot = ui.panels[id];
        if (!slot) {
            slot = std::make_unique<Impl::Panel>(ui.font);
            slot->position = initialPosition;
            slot->width = width;
            // Flottant à la fermeture de l'éditeur : il reprend sa place et sa taille (LoadDockLayout).
            if (const auto saved = ui.savedFloating.find(id); saved != ui.savedFloating.end()) {
                slot->position = saved->second.position;
                if (saved->second.width > 0.0f) {
                    slot->width = slot->userWidth = std::max(MinPanelWidth, saved->second.width);
                }
                slot->userHeight = std::max(0.0f, saved->second.height);
                slot->collapsed = saved->second.collapsed;
                ui.savedFloating.erase(saved);
            }
            ui.order.push_back(id);
        }
        Impl::Panel& panel = *slot;
        panel.title = VisibleLabel(title);
        panel.fullTitle = title;
        panel.floatingWidth = width;
        panel.fixedHeight = height;
        panel.closable = open != nullptr;
        panel.icon = std::exchange(ui.nextPanelIcon, Icon::None);
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

        // Flottant : bords à glisser pour redimensionner, croix pour fermer, barre de titre à glisser pour déplacer
        // et à cliquer pour replier.
        if (!panel.docked) {
            ui.ResizePanel(id, panel, titleHeight);
            if (panel.userHeight > 0.0f) {
                panel.fixedHeight = panel.userHeight; // la hauteur choisie à la souris l'emporte (avec défilement)
            }
            const WidgetId titleId = Hash(id, "#titre");
            const bool wasCollapsed = panel.collapsed;
            panel.collapsed = false; // Behavior ignore les panneaux repliés ; la barre de titre, elle, reste active
            ui.clipMin = panel.position;
            ui.clipMax = panel.position + Vector2f{panel.width, titleHeight};
            panel.closeHovered = panel.closeHeld = false;
            if (panel.closable) {
                const Vector2f closeMax = ui.clipMax;
                const Impl::Interaction close = ui.Behavior(Hash(id, "#fermer"), {closeMax.x - titleHeight, ui.clipMin.y}, closeMax);
                panel.closeHovered = close.hovered;
                panel.closeHeld = close.held;
                if (close.clicked) {
                    *open = false;
                }
            }
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

    void Context::SetNextPanelIcon(Icon icon) {
        m_impl->nextPanelIcon = icon;
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

        ui.panel->content.AddRoundedRect(min, max, style.frame, style.frameRounding);
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
        if (std::exchange(scroll.toEnd, false)) {
            scroll.scrollY = maxScroll; // pris en compte dès la frame suivante (le contenu est déjà placé)
        }
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

    void Context::SetScrollToEnd() {
        Impl& ui = *m_impl;
        if (!ui.childStack.empty()) {
            ui.childScrolls[ui.childStack.back().id].toEnd = true;
        }
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
            // Opaque, et sans barre de titre : la barre d'onglets de la zone est dessinée par DockSpace. Coins du bas
            // arrondis (le haut touche les onglets) : chaque zone est une carte sur le fond de la fenêtre.
            LinearColor background = style.panelBackground;
            background.a = 1.0f;
            const float rounding = std::min(style.panelRounding, 0.5f * (max.y - viewTop));
            chrome.SetClipRect({min.x, viewTop}, max);
            chrome.AddRoundedRect({min.x, viewTop - rounding}, max, background, rounding);
            chrome.ResetClipRect();
        } else {
            // Ombre (ou halo), fond et barre de titre aux coins arrondis : la barre garde les coins du haut seulement,
            // le bas de son rectangle arrondi est coupé par la découpe.
            const float rounding = std::min(style.panelRounding, titleHeight * 0.5f);
            if (style.shadowSize > 0.0f) {
                chrome.AddShadow(min + Vector2f{0.0f, 3.0f}, max + Vector2f{0.0f, 3.0f}, style.shadow, rounding, style.shadowSize);
            }
            chrome.AddRoundedRect(min, max, style.panelBackground, rounding);
            const bool front = ui.order.back() == ui.panelId;
            const LinearColor titleColor = front ? style.titleBackgroundActive : style.titleBackground;
            if (panel.collapsed) {
                chrome.AddRoundedRect(min, max, titleColor, rounding);
            } else {
                chrome.SetClipRect(min, {max.x, viewTop});
                chrome.AddRoundedRect(min, {max.x, viewTop + rounding}, titleColor, rounding);
                chrome.ResetClipRect();
            }

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
            float titleX = arrowX + arrow + style.framePadding.x;
            if (panel.icon != Icon::None) {
                const float iconSize = std::round(style.fontSize);
                DrawIcon(chrome, panel.icon, {titleX, std::round(centerY - iconSize * 0.5f)}, iconSize);
                titleX += iconSize + style.framePadding.x * 0.75f;
            }
            const float titleMaxX = panel.closable ? max.x - titleHeight : max.x - style.framePadding.x;
            chrome.SetClipRect({titleX, min.y}, {titleMaxX, viewTop});
            chrome.AddText({titleX, min.y + style.framePadding.y}, panel.title, style.fontSize, style.text);
            chrome.ResetClipRect();

            // Croix de fermeture, sur un carré rouge au survol.
            if (panel.closable) {
                const Vector2f boxMin{max.x - titleHeight, min.y};
                const Vector2f boxMax{max.x, viewTop};
                if (panel.closeHovered || panel.closeHeld) {
                    const Vector2f inset{3.0f, 3.0f};
                    chrome.AddRoundedRect(boxMin + inset, boxMax - inset,
                                          panel.closeHeld ? LinearColor::FromSrgb8(150, 30, 30) : LinearColor::FromSrgb8(196, 43, 28),
                                          style.frameRounding);
                }
                const Vector2f center = (boxMin + boxMax) * 0.5f;
                const float half = style.fontSize * 0.3f;
                const LinearColor cross = panel.closeHovered || panel.closeHeld ? LinearColor::FromSrgb8(255, 255, 255) : style.text;
                chrome.AddLine(center - Vector2f{half, half}, center + Vector2f{half, half}, cross, 1.5f);
                chrome.AddLine(center + Vector2f{-half, half}, center + Vector2f{half, -half}, cross, 1.5f);
            }
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
            chrome.AddRoundedRectOutline(min, max, style.panelBorder, std::min(style.panelRounding, titleHeight * 0.5f));
        }

        panel.content.ResetClipRect();
        ui.idStack.pop_back();
        ui.panel = nullptr;
        ui.panelId = 0;
    }
}
