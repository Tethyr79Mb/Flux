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

    bool Context::Tab(std::string_view label, bool selected) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const Style& style = ui.style;
        const std::string_view visible = VisibleLabel(label);
        const Vector2f size{ui.MeasureText(visible).x + 2.0f * style.framePadding.x + 8.0f, ui.FrameHeight()};
        const Vector2f min = ui.ItemRect(size, 0.0f);
        const Vector2f max = min + size;
        const Impl::Interaction interaction = ui.Behavior(ui.GetId(label), min, max);

        // Même allure que les onglets des zones ancrées : fond du contenu et trait d'accent pour l'onglet affiché.
        if (selected) {
            ui.panel->content.AddRect(min, max, style.frameActive);
            ui.panel->content.AddRect(min, {max.x, min.y + 2.0f}, style.checkMark);
        } else if (interaction.hovered) {
            ui.panel->content.AddRect(min, max, style.frameHovered);
        }
        ui.panel->content.AddText({min.x + style.framePadding.x + 4.0f, min.y + style.framePadding.y}, visible,
                                  style.fontSize, selected ? style.text : style.textDisabled);
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
        if (ui.mouseEnabled && ui.input->WasMouseButtonPressed(MouseButton::Right)
            && ui.IsMouseOver(ui.lastItemMin, ui.lastItemMax)) {
            ui.openMenu = popupId;
            ui.contextPosition = ui.mouse;
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

    Math::Vector2f Context::MeasureText(std::string_view text) const {
        const Impl& ui = *m_impl;
        return ui.measure.MeasureText(text, ui.style.fontSize);
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
