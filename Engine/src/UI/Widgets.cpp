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
}
