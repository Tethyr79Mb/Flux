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
        ui.FrameRect(min, max, color);
        ui.panel->content.AddText(min + style.framePadding, visible, style.fontSize, style.text);
        return interaction.clicked;
    }

    bool Context::Button(std::string_view label, Icon icon, bool fullWidth) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const Style& style = ui.style;
        const std::string_view visible = VisibleLabel(label);
        const float iconSize = icon != Icon::None ? std::round(style.fontSize * 0.9f) : 0.0f;
        const float textWidth = ui.MeasureText(visible).x;
        const float gap = iconSize > 0.0f && !visible.empty() ? style.framePadding.x * 0.75f : 0.0f;
        const float contentWidth = iconSize + gap + textWidth;
        const Vector2f size{fullWidth ? ui.RowWidth() : contentWidth + 2.0f * style.framePadding.x, ui.FrameHeight()};
        const Vector2f min = ui.ItemRect(size);
        const Vector2f max = min + size;
        const Impl::Interaction interaction = ui.Behavior(ui.GetId(label), min, max);

        const LinearColor color = interaction.held && interaction.hovered ? style.buttonActive
                                : interaction.hovered                     ? style.buttonHovered
                                                                          : style.button;
        ui.FrameRect(min, max, color);
        const float left = fullWidth ? std::round(0.5f * (min.x + max.x - contentWidth)) : min.x + style.framePadding.x;
        if (iconSize > 0.0f) {
            DrawIcon(ui.panel->content, icon, {left, std::round(0.5f * (min.y + max.y - iconSize))}, iconSize);
        }
        ui.panel->content.AddText({left + iconSize + gap, min.y + style.framePadding.y}, visible, style.fontSize, style.text);
        return interaction.clicked;
    }

    bool Context::ToggleButton(std::string_view label, Icon icon, bool& value) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const Style& style = ui.style;
        const std::string_view visible = VisibleLabel(label);
        const float iconSize = icon != Icon::None ? std::round(style.fontSize * 0.9f) : 0.0f;
        const float gap = iconSize > 0.0f && !visible.empty() ? style.framePadding.x * 0.75f : 0.0f;
        const Vector2f size{iconSize + gap + ui.MeasureText(visible).x + 2.0f * style.framePadding.x, ui.FrameHeight()};
        const Vector2f min = ui.ItemRect(size);
        const Vector2f max = min + size;
        const Impl::Interaction interaction = ui.Behavior(ui.GetId(label), min, max);
        if (interaction.clicked) {
            value = !value;
        }
        // Enfoncé : couleur des boutons et liseré d'accent ; relâché : fond des champs, texte grisé.
        const LinearColor color = interaction.held && interaction.hovered ? style.buttonActive
                                : interaction.hovered                     ? style.buttonHovered
                                : value                                   ? style.button
                                                                          : style.frame;
        ui.FrameRect(min, max, color);
        if (value) {
            ui.panel->content.AddRoundedRectOutline(min, max, style.checkMark, style.frameRounding);
        }
        if (iconSize > 0.0f) {
            DrawIcon(ui.panel->content, icon, {min.x + style.framePadding.x, std::round(0.5f * (min.y + max.y - iconSize))},
                     iconSize);
        }
        ui.panel->content.AddText({min.x + style.framePadding.x + iconSize + gap, min.y + style.framePadding.y}, visible,
                                  style.fontSize, value ? style.text : style.textDisabled);
        return interaction.clicked;
    }

    void Context::SetItemTooltip(std::string_view text) {
        Impl& ui = *m_impl;
        if (!ui.panel || text.empty() || ui.activeId != 0 || ui.openMenu != 0
            || !ui.IsMouseOver(ui.lastBehaviorMin, ui.lastBehaviorMax)) {
            return;
        }
        // Widget sans identifiant (texte d'une barre) : reconnu à son texte.
        const WidgetId id = ui.lastBehaviorId != 0 ? ui.lastBehaviorId : Hash(ui.idStack.back(), text);
        if (id != ui.tooltipId) {
            ui.tooltipId = id;
            ui.tooltipSince = ui.time;
        }
        ui.tooltipSeen = true;
        constexpr double Delay = 0.5; // secondes immobile sur le widget avant l'info-bulle
        if (ui.time - ui.tooltipSince >= Delay) {
            ui.tooltipText = text;
        }
    }

    Context::TileResult Context::Tile(std::string_view label, Icon icon, std::string_view badge, LinearColor badgeColor,
                                      bool selected, float width, const TextureInfo* image) {
        Impl& ui = *m_impl;
        TileResult result;
        if (!ui.panel) {
            return result;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        // De haut en bas : l'icône (carré de 48 pixels au plus), la bande du type, le nom.
        const float side = std::min(width - 16.0f, 48.0f);
        const float bandHeight = ui.LineHeight() + 2.0f;
        const float nameTop = 4.0f + side + 3.0f + bandHeight + 5.0f;
        const float height = nameTop + ui.LineHeight() + 4.0f;
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
        const float centerX = std::floor(min.x + width * 0.5f);
        const float x0 = centerX - side * 0.5f;
        const float y0 = min.y + 4.0f;
        const float y1 = y0 + side;
        if (image && image->size.x > 0 && image->size.y > 0) {
            // Icône fournie (celle du système) : dans le carré, proportions gardées, jamais agrandie au-delà de 1:1
            // tant qu'elle tient (une icône de 48 reste nette).
            const float scale = std::min(side / static_cast<float>(std::max(image->size.x, image->size.y)), 1.0f);
            const Vector2f size{std::floor(static_cast<float>(image->size.x) * scale), std::floor(static_cast<float>(image->size.y) * scale)};
            const Vector2f at{std::floor(centerX - size.x * 0.5f), std::floor(y0 + (side - size.y) * 0.5f)};
            list.AddImage(at, at + size, image->id);
        } else {
            DrawIcon(list, icon, {x0, y0}, side);
        }
        // Bande du type : « 3D », « Image »..., à la largeur du texte.
        if (!badge.empty()) {
            const Vector2f badgeSize = ui.MeasureText(badge);
            const float bandWidth = std::min(width - 2.0f, std::max(badgeSize.x + 10.0f, 30.0f));
            const Vector2f bandMin{std::floor(centerX - bandWidth * 0.5f), y1 + 3.0f};
            const Vector2f bandMax{bandMin.x + bandWidth, bandMin.y + bandHeight};
            list.AddRect(bandMin, bandMax, badgeColor);
            // Texte clair : les bandes sont foncées, quel que soit le thème.
            ui.ClippedText({std::max(bandMin.x + 2.0f, centerX - badgeSize.x * 0.5f), (bandMin.y + bandMax.y - badgeSize.y) * 0.5f},
                           bandMax.x - 2.0f, badge, LinearColor::FromSrgb8(240, 240, 242), bandMin.x);
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
        ui.ClippedText({min.x + std::max(2.0f, (width - nameWidth) * 0.5f), min.y + nameTop}, max.x - 2.0f, name, style.text,
                       min.x);
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
        // Cochée : remplie de la couleur d'accent, coche claire.
        const float inset = std::round(box * 0.1f);
        const Vector2f boxMin = min + Vector2f{inset, inset};
        const Vector2f boxMax = min + Vector2f{box - inset, box - inset};
        if (value) {
            const LinearColor accent = interaction.hovered ? LinearColor{style.checkMark.r * 1.15f, style.checkMark.g * 1.15f,
                                                                         style.checkMark.b * 1.15f, style.checkMark.a}
                                                           : style.checkMark;
            list.AddRoundedRect(boxMin, boxMax, accent, style.frameRounding);
            const float side = boxMax.x - boxMin.x;
            const float thickness = std::max(2.0f, side * 0.13f);
            const auto at = [&](float x, float y) { return boxMin + Vector2f{side * x, side * y}; };
            const LinearColor mark = LinearColor::FromSrgb8(255, 255, 255);
            list.AddLine(at(0.2f, 0.52f), at(0.42f, 0.74f), mark, thickness);
            list.AddLine(at(0.40f, 0.74f), at(0.8f, 0.28f), mark, thickness);
        } else {
            // Fond des boutons : la case se voit sur le fond des panneaux, plus foncé que les champs.
            list.AddRoundedRect(boxMin, boxMax, interaction.hovered ? style.buttonHovered : style.button, style.frameRounding);
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
        list.AddRoundedRect(min, max, open || (interaction.held && interaction.hovered) ? style.frameActive
                               : interaction.hovered                               ? style.frameHovered
                                                                                   : style.frame,
                            style.frameRounding);
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

    bool Context::CollapsingHeader(std::string_view label, bool defaultOpen, Icon icon, bool* remove) {
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

        // Un peu d'air au-dessus d'une section qui suit d'autres widgets.
        if (!ui.sameLine && ui.cursorY > ui.contentMin.y) {
            ui.cursorY += style.itemSpacing;
        }
        const float height = ui.FrameHeight() + 2.0f;
        const Vector2f min = ui.ItemRect({ui.RowWidth(), height});
        const Vector2f max = min + Vector2f{ui.RowWidth(), height};
        // Croix de retrait au bout du titre : décrite avant le titre, elle prend le clic.
        const float removeSize = remove ? height : 0.0f;
        bool removeHovered = false;
        if (remove) {
            const Vector2f boxMin{max.x - removeSize, min.y};
            const Impl::Interaction close = ui.Behavior(Hash(id, "#retirer"), boxMin, max);
            removeHovered = close.hovered;
            if (close.clicked) {
                *remove = true;
            }
        }
        const Impl::Interaction interaction = ui.Behavior(id, min, {max.x - removeSize, max.y});
        if (interaction.clicked) {
            open = !open;
        }

        DrawList& list = ui.panel->content;
        list.AddRoundedRect(min, max, interaction.hovered ? style.headerHovered : style.header, style.frameRounding);
        const float arrowWidth = style.fontSize;
        const float size = style.fontSize * 0.42f;
        const float x = min.x + style.framePadding.x * 0.5f + (arrowWidth - size) * 0.5f;
        const float centerY = min.y + height * 0.5f;
        const LinearColor arrow = interaction.hovered ? style.text : style.textDisabled;
        if (open) {
            list.AddTriangle({x, centerY - size * 0.35f}, {x + size, centerY - size * 0.35f},
                             {x + size * 0.5f, centerY + size * 0.45f}, arrow);
        } else {
            list.AddTriangle({x + size * 0.15f, centerY - size * 0.5f}, {x + size * 0.15f, centerY + size * 0.5f},
                             {x + size * 0.95f, centerY}, arrow);
        }
        float textX = min.x + style.framePadding.x * 0.5f + arrowWidth + 2.0f;
        if (icon != Icon::None) {
            const float iconSize = std::round(style.fontSize);
            DrawIcon(list, icon, {textX, std::round(centerY - iconSize * 0.5f)}, iconSize);
            textX += iconSize + style.framePadding.x * 0.75f;
        }
        ui.ClippedText({textX, std::round(centerY - ui.LineHeight() * 0.5f)}, max.x - removeSize - 2.0f, VisibleLabel(label),
                       style.text);
        if (remove) {
            const Vector2f center{max.x - removeSize * 0.5f, centerY};
            if (removeHovered) {
                list.AddRoundedRect(center - Vector2f{removeSize * 0.36f, removeSize * 0.36f},
                                    center + Vector2f{removeSize * 0.36f, removeSize * 0.36f},
                                    LinearColor::FromSrgb8(196, 43, 28), style.frameRounding);
            }
            const float half = style.fontSize * 0.26f;
            const LinearColor cross = removeHovered ? LinearColor::FromSrgb8(255, 255, 255) : style.textDisabled;
            list.AddLine(center - Vector2f{half, half}, center + Vector2f{half, half}, cross, 1.5f);
            list.AddLine(center + Vector2f{-half, half}, center + Vector2f{half, -half}, cross, 1.5f);
        }
        return open;
    }
}
