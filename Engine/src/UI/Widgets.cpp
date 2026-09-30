// Widgets simples : textes, boutons, onglets, cases à cocher, choix, sections repliables.
#include "ContextImpl.hpp"

namespace Flux::UI {
    void Context::Text(std::string_view text) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return;
        }
        const Vector2f min = ui.ItemRect(ui.MeasureText(text));
        ui.panel->content.AddText(min, text, ui.style.fontSize, ui.style.text);
    }

    void Context::BeginDisabled(bool disabled) {
        Impl& ui = *m_impl;
        ui.disabledStack.push_back(disabled);
        if (disabled && ui.disabledDepth++ == 0) {
            ui.enabledText = ui.style.text;
            ui.style.text = ui.style.textDisabled;
        }
    }

    void Context::EndDisabled() {
        Impl& ui = *m_impl;
        if (ui.disabledStack.empty()) {
            return;
        }
        const bool disabled = ui.disabledStack.back();
        ui.disabledStack.pop_back();
        if (disabled && --ui.disabledDepth == 0) {
            ui.style.text = ui.enabledText;
        }
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

    Context::TileResult Context::Tile(std::string_view label, TileIcon icon, std::string_view badge, LinearColor badgeColor,
                                      bool selected, float width) {
        Impl& ui = *m_impl;
        TileResult result;
        if (!ui.panel) {
            return result;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        const float iconHeight = std::floor(width * 0.7f);
        const float height = iconHeight + ui.LineHeight() + 8.0f;
        const Vector2f min = ui.ItemRect({width, height});
        const Vector2f max = min + Vector2f{width, height};
        const Impl::Interaction interaction = ui.Behavior(id, min, max);
        if (interaction.clicked) {
            result.clicked = true;
            result.doubleClicked = ui.lastClickId == id && ui.time - ui.lastClickTime < 0.35;
            ui.lastClickId = result.doubleClicked ? 0 : id;
            ui.lastClickTime = ui.time;
        }

        DrawList& list = ui.panel->content;
        if (selected) {
            list.AddRect(min, max, style.selection);
        } else if (interaction.hovered) {
            list.AddRect(min, max, style.frame);
        }
        // Icône centrée dans la partie haute, un peu plus large que haute.
        const float iconWidth = std::min(width - 16.0f, (iconHeight - 8.0f) * 1.25f);
        const float x0 = std::floor(min.x + (width - iconWidth) * 0.5f);
        const float x1 = x0 + iconWidth;
        const float y0 = min.y + 6.0f;
        const float y1 = min.y + iconHeight;
        const float h = y1 - y0;
        const auto shade = [](LinearColor color, float factor) {
            return LinearColor{color.r * factor, color.g * factor, color.b * factor, color.a};
        };
        if (icon == TileIcon::Folder) {
            // Onglet, dos du dossier, puis rabat avant plus clair.
            const LinearColor back = LinearColor::FromSrgb8(196, 150, 58);
            const float tab = std::floor(iconWidth * 0.42f);
            list.AddRect({x0, y0}, {x0 + tab, y0 + h * 0.2f}, back);
            list.AddTriangle({x0 + tab, y0}, {x0 + tab, y0 + h * 0.2f}, {x0 + tab + h * 0.14f, y0 + h * 0.2f}, back);
            list.AddRect({x0, y0 + h * 0.12f}, {x1, y1}, back);
            list.AddRect({x0, y0 + h * 0.3f}, {x1, y1}, LinearColor::FromSrgb8(232, 186, 82));
        } else {
            // Page au coin corné, bande colorée du type en bas.
            const float corner = std::floor(std::min(iconWidth, h) * 0.26f);
            const float pageX0 = x0 + iconWidth * 0.12f;
            const float pageX1 = x1 - iconWidth * 0.12f;
            const LinearColor paper = LinearColor::FromSrgb8(214, 218, 226);
            list.AddRect({pageX0, y0}, {pageX1 - corner, y0 + corner}, paper);
            list.AddRect({pageX0, y0 + corner}, {pageX1, y1}, paper);
            list.AddTriangle({pageX1 - corner, y0}, {pageX1 - corner, y0 + corner}, {pageX1, y0 + corner}, shade(paper, 0.65f));
            const float bandHeight = ui.LineHeight() + 2.0f;
            const Vector2f bandMin{pageX0, y1 - bandHeight - 4.0f};
            const Vector2f bandMax{pageX1, y1 - 4.0f};
            list.AddRect(bandMin, bandMax, badgeColor);
            const Vector2f badgeSize = ui.MeasureText(badge);
            ui.ClippedText({std::max(bandMin.x + 2.0f, (bandMin.x + bandMax.x - badgeSize.x) * 0.5f),
                            (bandMin.y + bandMax.y - badgeSize.y) * 0.5f},
                           bandMax.x - 2.0f, badge, style.text, bandMin.x);
        }

        // Nom centré, raccourci par « ... » s'il ne tient pas.
        std::string name(VisibleLabel(label));
        const float room = width - 4.0f;
        if (ui.MeasureText(name).x > room) {
            while (!name.empty() && ui.MeasureText(name + "...").x > room) {
                name.erase(PreviousBoundary(name, name.size()));
            }
            name += "...";
        }
        const float nameWidth = ui.MeasureText(name).x;
        ui.ClippedText({min.x + std::max(2.0f, (width - nameWidth) * 0.5f), min.y + iconHeight + 4.0f}, max.x - 2.0f, name,
                       style.text, min.x);
        return result;
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
            list.AddLine(at(0.22f, 0.52f), at(0.42f, 0.72f), style.checkMark, thickness);
            list.AddLine(at(0.40f, 0.72f), at(0.78f, 0.30f), style.checkMark, thickness);
        }
        if (!visible.empty()) {
            list.AddText({min.x + box + 2.0f * style.itemSpacing, min.y + style.framePadding.y}, visible, style.fontSize,
                         style.text);
        }
        return interaction.clicked;
    }

    bool Context::Choice(std::string_view label, int& index, std::span<const std::string_view> options) {
        Impl& ui = *m_impl;
        if (!ui.panel || options.empty() || ui.menuParent) {
            return false;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        const WidgetId popupId = Hash(id, "#liste");
        const auto [min, max] = ui.LabeledRow(label);
        const Impl::Interaction interaction = ui.Behavior(id, min, max);
        if (interaction.clicked) {
            ui.openMenu = popupId; // un clic sur le champ quand la liste est ouverte la referme (BeginFrame)
        }
        const bool open = ui.openMenu == popupId;
        const int count = static_cast<int>(options.size());

        DrawList& list = ui.panel->content;
        list.AddRect(min, max, open || (interaction.held && interaction.hovered) ? style.frameActive
                               : interaction.hovered                               ? style.frameHovered
                                                                                   : style.frame);
        const std::string_view text = index >= 0 && index < count ? options[static_cast<std::size_t>(index)] : "?";
        const float arrow = style.fontSize * 0.35f;
        ui.ClippedText(min + style.framePadding, max.x - arrow - 2.0f * style.framePadding.x, text, style.text);
        // Pointe vers le bas : une liste s'ouvre.
        const float x = max.x - style.framePadding.x - arrow;
        const float centerY = (min.y + max.y) * 0.5f;
        list.AddTriangle({x, centerY - arrow * 0.3f}, {x + arrow, centerY - arrow * 0.3f}, {x + arrow * 0.5f, centerY + arrow * 0.4f},
                         style.text);
        if (!open) {
            return false;
        }

        // La liste, sous le champ : la valeur en cours est surlignée.
        bool changed = false;
        ui.BeginPopup(popupId, {min.x, max.y}, max.x - min.x);
        for (int i = 0; i < count; ++i) {
            PushId(static_cast<std::uint64_t>(i)); // deux options de même nom restent distinctes
            if (i == index) {
                const Vector2f rowMin{ui.contentMin.x, ui.cursorY};
                ui.panel->content.AddRect(rowMin, {ui.contentMaxX, rowMin.y + ui.FrameHeight()}, style.frameActive);
            }
            if (MenuItem(options[static_cast<std::size_t>(i)]) && i != index) {
                index = i;
                changed = true;
            }
            PopId();
        }
        EndMenu();
        return changed;
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
}
