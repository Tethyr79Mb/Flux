// Éditeur d'interface : panneau « Interface » (ajouter des éléments, leur arbre) et panneau « Propriétés ».
#include "Interface/InterfaceEditor.hpp"

#include "Panels/AssetPicker.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/UI/DrawList.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace FluxEditor {
    using namespace Flux;
    using Interface::ElementType;
    using Math::LinearColor;
    using Math::Vector2f;

    namespace {
        constexpr float DragThreshold = 4.0f;

        // Actions du menu clic droit d'un élément de l'arbre.
        enum class Command { None, Duplicate, Forward, Backward, MoveOut, Remove };

        UI::Icon IconOf(ElementType type) {
            switch (type) {
                case ElementType::Window: return UI::Icon::Window;
                case ElementType::Frame: return UI::Icon::Frame;
                case ElementType::Text: return UI::Icon::Text;
                case ElementType::Image: return UI::Icon::Image;
                case ElementType::Button: return UI::Icon::Button;
                case ElementType::Checkbox: return UI::Icon::Checkbox;
                case ElementType::Slider: return UI::Icon::Slider;
                case ElementType::Count: break;
            }
            return UI::Icon::None;
        }

        std::string_view TypeName(ElementType type) {
            return Interface::GetTypeNames()[static_cast<std::size_t>(type)];
        }

        // L'élément `inner` est `outer` ou l'un de ses descendants.
        bool IsInside(const Interface::Element& outer, std::uint32_t inner) {
            if (outer.id == inner) {
                return true;
            }
            return std::ranges::any_of(outer.children, [inner](const Interface::Element& child) { return IsInside(child, inner); });
        }

        const Interface::Placed* FindPlaced(const std::vector<Interface::Placed>& placed, std::uint32_t id) {
            const auto found = std::ranges::find_if(placed, [id](const Interface::Placed& item) { return item.element->id == id; });
            return found == placed.end() ? nullptr : &*found;
        }
    }

    void InterfaceEditor::DrawElementsPanel(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        OpenInterface& open = *GetActive();
        const Input& input = engine.GetWindow().GetInput();
        // Position sans ancrage (l'éditeur l'ancre à gauche, en onglet avec Scènes).
        ui.SetNextPanelIcon(UI::Icon::Window);
        if (ui.BeginPanel("Interface", {8.0f, 8.0f + ui.GetMenuBarHeight()}, 300.0f)) {
            ui.Text(open.GetName() + (open.IsModified() ? " *" : ""));
            ui.Separator();
            ui.TextDisabled("Ajouter");
            // Trois par ligne : les plus courants d'abord.
            for (int type = 0; type < static_cast<int>(ElementType::Count); ++type) {
                if (type % 3 != 0) {
                    ui.SameLine();
                }
                if (ui.Button(TypeName(static_cast<ElementType>(type)))) {
                    Add(static_cast<ElementType>(type));
                }
            }
            ui.TextDisabled("Dans la fenêtre ou le cadre sélectionné.");
            ui.Separator();
            ui.TextDisabled("Éléments (le dernier passe devant)");

            m_treeDrag.target = 0;
            std::uint32_t action = 0;
            int command = static_cast<int>(Command::None);
            ui.BeginChild("elements", 0.0f, 0.0f);
            for (Interface::Element& element : open.document.roots) {
                DrawElementNode(engine, element, action, command);
            }
            if (open.document.roots.empty()) {
                ui.TextDisabled("Aucun élément.");
            }
            ui.EndChild();

            // Après l'arbre : les éléments peuvent changer de place.
            switch (static_cast<Command>(command)) {
                case Command::Duplicate:
                    open.selection = action;
                    Duplicate();
                    break;
                case Command::Forward: Reorder(action, 1); break;
                case Command::Backward: Reorder(action, -1); break;
                case Command::MoveOut:
                    if (const Interface::Element* parent = Interface::FindParent(open.document, action)) {
                        MoveTo(action, parent->id, 1);
                    }
                    break;
                case Command::Remove:
                    open.selection = action;
                    Delete();
                    break;
                case Command::None: break;
            }
        }
        ui.EndPanel();

        // Glisser-déposer dans l'arbre : lâché sur un élément, l'élément glissé se range avant, dedans ou après lui.
        if (m_treeDrag.id != 0) {
            const Vector2f moved = input.GetMousePosition() - m_treeDrag.pressMouse;
            m_treeDrag.dragging |= std::abs(moved.x) + std::abs(moved.y) > DragThreshold;
            if (!input.IsMouseButtonDown(MouseButton::Left)) {
                if (m_treeDrag.dragging && m_treeDrag.target != 0) {
                    MoveTo(m_treeDrag.id, m_treeDrag.target, m_treeDrag.zone);
                }
                m_treeDrag = {};
            }
        }
    }

    void InterfaceEditor::DrawElementNode(Engine& engine, Interface::Element& element, std::uint32_t& action, int& command) {
        UI::Context& ui = engine.GetUi();
        OpenInterface& open = *GetActive();
        ui.PushId(element.id);
        std::string label = element.name.empty() ? std::string(TypeName(element.type)) : element.name;
        if (!element.visible) {
            label += " (masqué)";
        }
        const bool leaf = element.children.empty();
        const UI::Context::TreeNodeResult node = ui.TreeNode(label, open.selection == element.id, leaf, true, IconOf(element.type));
        if (node.clicked) {
            open.selection = element.id;
        }
        if (ui.IsItemActive() && m_treeDrag.id == 0) {
            m_treeDrag = {.id = element.id, .pressMouse = engine.GetWindow().GetInput().GetMousePosition()};
        }
        if (m_treeDrag.dragging && m_treeDrag.id != element.id && ui.IsMouseOverItem()) {
            // Pas dans lui-même ni dans un de ses enfants.
            const Interface::Element* dragged = Interface::Find(std::as_const(open.document), m_treeDrag.id);
            if (dragged && !IsInside(*dragged, element.id)) {
                const UI::Rect rect = ui.GetItemRect();
                const float y = (engine.GetWindow().GetInput().GetMousePosition().y - rect.min.y) / std::max(1.0f, rect.max.y - rect.min.y);
                int zone = y < 0.5f ? -1 : 1;
                if (Interface::IsContainer(element.type)) {
                    zone = y < 0.25f ? -1 : y > 0.75f ? 1 : 0;
                }
                m_treeDrag.target = element.id;
                m_treeDrag.zone = zone;
                if (UI::DrawList* list = ui.GetPanelDrawList()) {
                    const LinearColor color = ui.GetStyle().checkMark;
                    if (zone == 0) {
                        list->AddRoundedRectOutline(rect.min, rect.max, color, 3.0f, 1.5f);
                    } else {
                        const float lineY = zone < 0 ? rect.min.y : rect.max.y;
                        list->AddLine({rect.min.x, lineY}, {rect.max.x, lineY}, color, 2.0f);
                    }
                }
            }
        }
        if (ui.BeginContextMenu("element")) {
            const auto item = [&](std::string_view text, Command value, bool enabled = true) {
                if (ui.MenuItem(text, {}, enabled)) {
                    action = element.id;
                    command = static_cast<int>(value);
                }
            };
            item("Dupliquer", Command::Duplicate);
            item("Vers l'avant", Command::Forward);
            item("Vers l'arrière", Command::Backward);
            item("Sortir du parent", Command::MoveOut, Interface::FindParent(open.document, element.id) != nullptr);
            ui.Separator();
            item("Supprimer", Command::Remove);
            ui.EndMenu();
        }
        if (node.open) {
            for (Interface::Element& child : element.children) {
                DrawElementNode(engine, child, action, command);
            }
            ui.TreePop();
        }
        ui.PopId();
    }

    void InterfaceEditor::MoveTo(std::uint32_t id, std::uint32_t target, int zone) {
        OpenInterface* open = GetActive();
        if (!open || id == target) {
            return;
        }
        Interface::Document& document = open->document;
        const Interface::Element* dragged = Interface::Find(std::as_const(document), id);
        if (!dragged || IsInside(*dragged, target) || !Interface::Find(std::as_const(document), target)) {
            return;
        }
        // Place à l'écran avant le déplacement : elle est gardée dans le nouveau parent.
        std::optional<Interface::Rect> rect;
        if (const Interface::Placed* placed = FindPlaced(Interface::Arrange(document, m_screen.min, m_screen.max), id)) {
            rect = placed->rect;
        }
        std::optional<Interface::Element> moving = Interface::Remove(document, id);
        if (!moving) {
            return;
        }
        if (zone == 0) {
            Interface::Find(document, target)->children.push_back(std::move(*moving));
        } else {
            std::size_t index = 0;
            std::vector<Interface::Element>* siblings = Interface::FindSiblings(document, target, index);
            siblings->insert(siblings->begin() + static_cast<std::ptrdiff_t>(index + (zone > 0 ? 1 : 0)), std::move(*moving));
        }
        if (rect && m_scale > 0.0f) {
            const std::vector<Interface::Placed> placed = Interface::Arrange(document, m_screen.min, m_screen.max);
            const Interface::Element* parent = Interface::FindParent(document, id);
            const Interface::Placed* parentPlaced = parent ? FindPlaced(placed, parent->id) : nullptr;
            if (!parent || parentPlaced) {
                Interface::Element& element = *Interface::Find(document, id);
                Interface::SetRect(element, parentPlaced ? parentPlaced->rect : m_screen, m_scale, *rect);
                element.position = {std::round(element.position.x), std::round(element.position.y)};
                element.size = {std::round(element.size.x), std::round(element.size.y)};
            }
        }
        open->selection = id;
        MarkChanged();
    }

    void InterfaceEditor::DrawPropertiesPanel(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        OpenInterface& open = *GetActive();
        // Position sans ancrage (l'éditeur l'ancre à droite, en onglet avec l'Inspecteur).
        const float width = 340.0f;
        ui.SetNextPanelIcon(UI::Icon::Inspector);
        if (ui.BeginPanel("Propriétés", {static_cast<float>(engine.GetWindow().GetSize().x) - width - 8.0f, 8.0f + ui.GetMenuBarHeight()},
                          width)) {
            // L'écran d'aperçu : la résolution change la largeur (les ancres font le reste), pas l'interface.
            ui.Choice("Aperçu", open.resolution, GetResolutionNames());
            if (ui.Button("Ajuster à la vue")) {
                open.zoom = 1.0f;
                open.pan = {};
            }
            ui.SameLine();
            ui.TextDisabled("Molette : zoom, clic droit : déplacer");
            ui.Separator();

            Interface::Element* element = GetSelected();
            if (!element) {
                ui.TextDisabled("Cliquez sur un élément de l'écran,\nou ajoutez-en un depuis le panneau Interface.");
            } else {
                ui.PushId(element->id);
                bool changed = false;
                ui.LabelText("Type", TypeName(element->type));
                changed |= ui.InputText("Nom", element->name);
                changed |= ui.Checkbox("Visible", element->visible);

                int anchor = static_cast<int>(element->anchor);
                if (ui.Choice("Ancre", anchor, Interface::GetAnchorNames())) {
                    // Changer d'ancre ne déplace pas l'élément : il garde sa place à l'écran.
                    const std::vector<Interface::Placed> placed = Interface::Arrange(open.document, m_screen.min, m_screen.max);
                    const Interface::Placed* item = FindPlaced(placed, element->id);
                    element->anchor = static_cast<Interface::Anchor>(anchor);
                    if (item && m_scale > 0.0f) {
                        Interface::SetRect(*element, item->parent, m_scale, item->rect);
                        element->position = {std::round(element->position.x), std::round(element->position.y)};
                        element->size = {std::round(element->size.x), std::round(element->size.y)};
                    }
                    changed = true;
                }
                if (element->anchor == Interface::Anchor::Stretch) {
                    changed |= ui.DragFloat2("Marges gauche, haut", element->position, 1.0f, UI::Context::NoMin, UI::Context::NoMax, 0);
                    changed |= ui.DragFloat2("Marges droite, bas", element->size, 1.0f, UI::Context::NoMin, UI::Context::NoMax, 0);
                } else {
                    changed |= ui.DragFloat2("Position", element->position, 1.0f, UI::Context::NoMin, UI::Context::NoMax, 0);
                    changed |= ui.DragFloat2("Taille", element->size, 1.0f, 0.0, 100000.0, 0);
                }
                ui.Separator();

                for (const Interface::Field& field : Interface::GetFields()) {
                    if (!field.AppliesTo(element->type)) {
                        continue;
                    }
                    std::visit(
                        [&](auto member) {
                            auto& value = element->*member;
                            using Value = std::remove_cvref_t<decltype(value)>;
                            if constexpr (std::is_same_v<Value, std::string>) {
                                if (field.image) {
                                    bool browse = false;
                                    changed |= ui.InputTextBrowse(field.label, value, browse);
                                    if (browse) {
                                        if (std::optional<std::string> chosen = ChooseAsset(engine, AssetType::Image, value)) {
                                            value = std::move(*chosen);
                                            changed = true;
                                        }
                                    }
                                } else {
                                    changed |= ui.InputText(field.label, value);
                                }
                            } else if constexpr (std::is_same_v<Value, LinearColor>) {
                                changed |= ui.ColorEdit4(field.label, value);
                            } else if constexpr (std::is_same_v<Value, float>) {
                                changed |= ui.DragFloat(field.label, value, field.speed, field.min, field.max, field.decimals);
                            } else if constexpr (std::is_same_v<Value, bool>) {
                                changed |= ui.Checkbox(field.label, value);
                            } else if constexpr (std::is_same_v<Value, Interface::TextAlign>) {
                                int index = static_cast<int>(value);
                                if (ui.Choice(field.label, index, Interface::GetAlignNames())) {
                                    value = static_cast<Interface::TextAlign>(index);
                                    changed = true;
                                }
                            }
                        },
                        field.member);
                }
                if (element->type == ElementType::Slider) {
                    const float low = std::min(element->minimum, element->maximum);
                    const float high = std::max(element->minimum, element->maximum);
                    element->value = std::clamp(element->value, low, high);
                }
                if (changed) {
                    MarkChanged();
                }
                ui.PopId();
            }
        }
        ui.EndPanel();
    }
}
