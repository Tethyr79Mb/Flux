// Champs éditables : nombres (glisser ou taper), texte, couleur ; saisie au clavier commune.
#include "ContextImpl.hpp"

namespace Flux::UI {
    void Context::Impl::BeginEdit(WidgetId id, std::string text) {
        editingId = id;
        editingSeen = true;
        editBuffer = std::move(text);
        caret = editBuffer.size();
        selectAll = true; // la première frappe remplace tout, comme dans la plupart des éditeurs
        caretTime = time;
    }

    Context::Impl::EditResult Context::Impl::ProcessTextEdit(Vector2f min, Vector2f max, bool numeric) {
        editingSeen = true;
        bool typed = false;
        const auto clearIfSelected = [&] {
            if (selectAll) {
                editBuffer.clear();
                caret = 0;
                selectAll = false;
            }
        };

        const std::string_view text = input->GetTextInput();
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t next = NextBoundary(text, i);
            const std::string_view character = text.substr(i, next - i);
            i = next;
            if (numeric && (character.size() != 1 || !IsNumberCharacter(character[0]))) {
                continue;
            }
            clearIfSelected();
            editBuffer.insert(caret, character);
            caret += character.size();
            typed = true;
        }
        if (input->WasKeyTyped(Key::Backspace)) {
            if (selectAll) {
                clearIfSelected();
            } else if (caret > 0) {
                const std::size_t previous = PreviousBoundary(editBuffer, caret);
                editBuffer.erase(previous, caret - previous);
                caret = previous;
            }
            typed = true;
        }
        if (input->WasKeyTyped(Key::Delete)) {
            if (selectAll) {
                clearIfSelected();
            } else if (caret < editBuffer.size()) {
                editBuffer.erase(caret, NextBoundary(editBuffer, caret) - caret);
            }
            typed = true;
        }
        if (input->WasKeyTyped(Key::Left)) {
            caret = selectAll ? 0 : PreviousBoundary(editBuffer, caret);
            selectAll = false;
            typed = true;
        }
        if (input->WasKeyTyped(Key::Right)) {
            caret = selectAll ? editBuffer.size() : NextBoundary(editBuffer, caret);
            selectAll = false;
            typed = true;
        }
        if (input->WasKeyPressed(Key::Home)) {
            caret = 0;
            selectAll = false;
            typed = true;
        }
        if (input->WasKeyPressed(Key::End)) {
            caret = editBuffer.size();
            selectAll = false;
            typed = true;
        }
        if (typed) {
            caretTime = time; // le curseur reste visible pendant la frappe
        }

        if (input->WasKeyPressed(Key::Escape)) {
            editingId = 0;
            return EditResult::Cancel;
        }
        if (input->WasKeyPressed(Key::Enter) || input->WasKeyPressed(Key::NumpadEnter) || input->WasKeyPressed(Key::Tab)
            || (mousePressed && !Contains(min, max, mouse))) { // clic ailleurs : on garde ce qui a été tapé
            editingId = 0;
            return EditResult::Commit;
        }
        return EditResult::Editing;
    }

    void Context::Impl::DrawTextEdit(Vector2f min, Vector2f max) {
        DrawList& list = panel->content;
        FrameRect(min, max, style.frameActive);
        // Texte décalé vers la gauche quand le curseur sortirait du champ.
        const float visibleWidth = max.x - min.x - 2.0f * style.framePadding.x;
        const float caretOffset = MeasureText(std::string_view(editBuffer).substr(0, caret)).x;
        const float scroll = std::max(0.0f, caretOffset - visibleWidth + 1.0f);
        const Vector2f textPosition{min.x + style.framePadding.x - scroll, min.y + style.framePadding.y};
        const float clipLeft = min.x + 1.0f;
        if (selectAll && !editBuffer.empty()) {
            const float textWidth = MeasureText(editBuffer).x;
            list.AddRect({std::max(textPosition.x - 1.0f, clipLeft), min.y + 2.0f},
                         {std::min(textPosition.x + textWidth + 1.0f, max.x - 1.0f), max.y - 2.0f}, style.selection);
        }
        ClippedText(textPosition, max.x - 1.0f, editBuffer, style.text, clipLeft);
        if (std::fmod(time - caretTime, 1.0) < 0.55) {
            const float caretX = textPosition.x + caretOffset;
            list.AddRect({caretX, min.y + 3.0f}, {caretX + 1.0f, max.y - 3.0f}, style.text);
        }
    }

    bool Context::Impl::DragScalar(WidgetId id, Vector2f min, Vector2f max, double& value, double speed, double low,
                                   double high, int decimals, const LinearColor* axisColor) {
        bool changed = false;
        if (editingId == id) {
            const EditResult result = ProcessTextEdit(min, max, true);
            if (result == EditResult::Editing) {
                DrawTextEdit(min, max);
                return false;
            }
            if (result == EditResult::Commit) {
                if (const std::optional<double> parsed = ParseNumber(editBuffer)) {
                    const double clamped = std::clamp(*parsed, low, high);
                    changed = clamped != value;
                    value = clamped;
                }
            }
            // Saisie terminée à cette frame : le champ est dessiné normalement.
        } else {
            const Interaction interaction = Behavior(id, min, max);
            if (interaction.held) {
                if (!dragging && std::abs(mouse.x - pressPosition.x) > 2.0f) {
                    dragging = true; // quelques pixels de tolérance pour qu'un clic reste un clic
                }
                if (dragging && mouseDelta.x != 0.0f) {
                    double step = speed;
                    if (input->IsShiftDown()) {
                        step *= 10.0;
                    } else if (input->IsControlDown()) {
                        step *= 0.1;
                    }
                    const double dragged = std::clamp(value + static_cast<double>(mouseDelta.x) * step, low, high);
                    changed = dragged != value;
                    value = dragged;
                }
            }
            if (interaction.clicked && !dragging) {
                BeginEdit(id, FormatEditable(value, decimals));
            }
        }

        const bool active = activeId == id || editingId == id;
        const bool hovered = activeId == 0 && IsMouseOver(min, max);
        DrawList& list = panel->content;
        FrameRect(min, max, active ? style.frameActive : (hovered ? style.frameHovered : style.frame));
        if (axisColor) {
            list.AddRoundedRect(min, {min.x + 3.0f, max.y}, *axisColor, std::min(style.frameRounding, 1.5f));
        }
        const std::string text = FormatNumber(value, decimals);
        const float textWidth = MeasureText(text).x;
        // Centré ; aligné à gauche quand il déborde, pour garder le signe et les premiers chiffres visibles.
        const float x = std::max(min.x + style.framePadding.x, (min.x + max.x - textWidth) * 0.5f);
        ClippedText({x, min.y + style.framePadding.y}, max.x - 2.0f, text, style.text);
        return changed;
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

    bool Context::DragFloat2(std::string_view label, Math::Vector2f& value, float speed, double min, double max, int decimals) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const WidgetId id = ui.GetId(label);
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        const float width = (fieldMax.x - fieldMin.x - ui.style.itemSpacing) / 2.0f;
        float* components[2] = {&value.x, &value.y};
        const LinearColor* colors[2] = {&ui.style.axisX, &ui.style.axisY};
        bool changed = false;
        for (int axis = 0; axis < 2; ++axis) {
            const float x = fieldMin.x + static_cast<float>(axis) * (width + ui.style.itemSpacing);
            double component = *components[axis];
            if (ui.DragScalar(Hash(id, &axis, sizeof(axis)), {x, fieldMin.y}, {x + width, fieldMax.y}, component, speed, min,
                              max, decimals, colors[axis])) {
                *components[axis] = static_cast<float>(component);
                changed = true;
            }
        }
        return changed;
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
        return ui.TextField(id, min, max, value);
    }

    bool Context::InputTextBrowse(std::string_view label, std::string& value, bool& browse) {
        Impl& ui = *m_impl;
        browse = false;
        if (!ui.panel) {
            return false;
        }
        const Style& style = ui.style;
        const WidgetId id = ui.GetId(label);
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        const float buttonWidth = ui.MeasureText("...").x + 2.0f * style.framePadding.x;
        const Vector2f buttonMin{std::max(fieldMin.x, fieldMax.x - buttonWidth), fieldMin.y};
        const Impl::Interaction interaction = ui.Behavior(Hash(id, "#parcourir"), buttonMin, fieldMax);
        const LinearColor color = interaction.held && interaction.hovered ? style.buttonActive
                                : interaction.hovered                     ? style.buttonHovered
                                                                          : style.button;
        ui.FrameRect(buttonMin, fieldMax, color);
        ui.ClippedText(buttonMin + style.framePadding, fieldMax.x, "...", style.text);
        browse = interaction.clicked;
        return ui.TextField(id, fieldMin, {buttonMin.x - style.itemSpacing, fieldMax.y}, value);
    }

    bool Context::Impl::TextField(WidgetId id, Vector2f min, Vector2f max, std::string& value) {
        Impl& ui = *this;
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
            ui.FrameRect(min, max, ui.style.frame);
            ui.ClippedText(min + ui.style.framePadding, max.x - 2.0f, value, ui.style.text);
            return changed;
        }

        const Impl::Interaction interaction = ui.Behavior(id, min, max);
        if (interaction.clicked) {
            ui.BeginEdit(id, value);
        }
        ui.FrameRect(min, max, interaction.hovered ? ui.style.frameHovered : ui.style.frame);
        ui.ClippedText(min + ui.style.framePadding, max.x - 2.0f, value, ui.style.text);
        return false;
    }

    bool Context::ColorEdit3(std::string_view label, Math::LinearColor& color) {
        return ColorEdit(label, color, false);
    }

    bool Context::ColorEdit4(std::string_view label, Math::LinearColor& color) {
        return ColorEdit(label, color, true);
    }

    bool Context::ColorEdit(std::string_view label, Math::LinearColor& color, bool alpha) {
        Impl& ui = *m_impl;
        if (!ui.panel) {
            return false;
        }
        const WidgetId id = ui.GetId(label);
        const auto [fieldMin, fieldMax] = ui.LabeledRow(label);
        const Style& style = ui.style;

        // Aperçu (couleur opaque), puis rouge, vert, bleu. Un clic sur l'aperçu ouvre le sélecteur de couleur :
        // fenêtre modale, la frame attend la réponse.
        const float swatch = fieldMax.y - fieldMin.y;
        const Vector2f swatchMax{fieldMin.x + swatch, fieldMax.y};
        bool changed = false;
        if (ui.colorPicker) {
            const Impl::Interaction interaction = ui.Behavior(Hash(id, "#nuancier"), fieldMin, swatchMax);
            if (interaction.clicked) {
                LinearColor picked = color;
                if (ui.colorPicker(picked) && !(picked == color)) {
                    color = picked;
                    changed = true;
                }
            }
            if (interaction.hovered) {
                ui.panel->content.AddRectOutline(fieldMin - Vector2f{1.0f, 1.0f}, swatchMax + Vector2f{1.0f, 1.0f},
                                                 style.checkMark);
            }
        }
        ui.panel->content.AddRect(fieldMin, swatchMax, {color.r, color.g, color.b, 1.0f});
        ui.panel->content.AddRectOutline(fieldMin, swatchMax, style.panelBorder);
        const float start = fieldMin.x + swatch + style.itemSpacing;
        const int count = alpha ? 4 : 3;
        const float width = (fieldMax.x - start - static_cast<float>(count - 1) * style.itemSpacing) / static_cast<float>(count);
        float* channels[3] = {&color.r, &color.g, &color.b};
        const LinearColor* colors[3] = {&style.axisX, &style.axisY, &style.axisZ};
        for (int channel = 0; channel < count; ++channel) {
            const float x = start + static_cast<float>(channel) * (width + style.itemSpacing);
            if (channel == 3) {
                // Opacité : linéaire (pas de sRGB), 255 = opaque.
                double value = static_cast<double>(std::clamp(color.a, 0.0f, 1.0f)) * 255.0;
                if (ui.DragScalar(Hash(id, &channel, sizeof(channel)), {x, fieldMin.y}, {x + width, fieldMax.y}, value, 1.0,
                                  0.0, 255.0, 0, nullptr)) {
                    color.a = static_cast<float>(value / 255.0);
                    changed = true;
                }
                continue;
            }
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
}
