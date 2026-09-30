// Arbre (hiérarchie) : nœuds dépliables, sélection, double-clic.
#include "ContextImpl.hpp"

namespace Flux::UI {
    Context::TreeNodeResult Context::TreeNode(std::string_view label, bool selected, bool leaf, bool defaultOpen, Icon icon) {
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
        float textX = min.x + arrowWidth;
        if (icon != Icon::None) {
            const float iconSize = std::floor(rowHeight - 4.0f);
            DrawIcon(list, icon, {textX, min.y + 2.0f}, iconSize);
            textX += iconSize + 4.0f;
        }
        ui.ClippedText({textX, min.y + 2.0f}, max.x - 2.0f, VisibleLabel(label), style.text);

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
}
