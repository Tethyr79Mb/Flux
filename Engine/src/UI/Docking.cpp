// Ancrage des panneaux : un arbre binaire de zones. Chaque zone découpée partage son rectangle entre ses deux
// parties (séparateur à glisser) ; chaque feuille accueille des panneaux en onglets, ou reste libre (centrale).
#include "ContextImpl.hpp"

#include <nlohmann/json.hpp>

namespace Flux::UI {
    namespace {
        constexpr float SplitterSize = 4.0f;  // espace entre deux zones, où l'on attrape le séparateur
        constexpr float MinZoneSize = 60.0f;  // une zone ne descend pas en dessous en glissant un séparateur
        constexpr int MaxDockDepth = 32;      // fichier de disposition : garde-fou contre un arbre absurde

        using Json = nlohmann::json;

        LinearColor Opaque(LinearColor color) {
            color.a = 1.0f;
            return color;
        }

        WidgetId PanelId(std::string_view title) {
            return Hash(RootId, title); // même identifiant que BeginPanel hors de toute portée PushId
        }
    }

    void Context::Impl::RebuildDockIndex() {
        dockedPanels.clear();
        for (std::size_t i = 0; i < dockNodes.size(); ++i) {
            for (const std::string& title : dockNodes[i].panels) {
                dockedPanels[PanelId(title)] = static_cast<DockId>(i);
            }
        }
    }

    void Context::Impl::LayoutDock(DockId id, Vector2f min, Vector2f max) {
        DockNode& node = dockNodes[id];
        node.min = min;
        node.max = max;
        if (node.IsLeaf()) {
            return;
        }
        const DockId first = node.children[0];
        const DockId second = node.children[1];
        if (node.horizontal) {
            const float available = std::max(0.0f, max.x - min.x - SplitterSize);
            const float split = std::floor(min.x + available * node.ratio);
            LayoutDock(first, min, {split, max.y});
            LayoutDock(second, {split + SplitterSize, min.y}, max);
        } else {
            const float available = std::max(0.0f, max.y - min.y - SplitterSize);
            const float split = std::floor(min.y + available * node.ratio);
            LayoutDock(first, min, {max.x, split});
            LayoutDock(second, {min.x, split + SplitterSize}, max);
        }
    }

    // La souris peut agir sur l'ancrage : ni panneau flottant, ni menu au-dessus d'elle.
    bool Context::Impl::DockMouseFree() const {
        if (!mouseEnabled) {
            return false;
        }
        if (hoveredPanel == 0) {
            return true;
        }
        const Panel& hovered = *panels.at(hoveredPanel);
        return hovered.docked && !hovered.overlay;
    }

    // Séparateurs : glisser pour changer la part de chaque côté.
    void Context::Impl::DockSplitters() {
        for (std::size_t i = 0; i < dockNodes.size(); ++i) {
            DockNode& node = dockNodes[i];
            if (node.IsLeaf()) {
                continue;
            }
            const DockNode& first = dockNodes[node.children[0]];
            const Vector2f min = node.horizontal ? Vector2f{first.max.x, node.min.y} : Vector2f{node.min.x, first.max.y};
            const Vector2f max = node.horizontal ? Vector2f{first.max.x + SplitterSize, node.max.y}
                                                 : Vector2f{node.max.x, first.max.y + SplitterSize};
            // Zone d'accroche un peu plus large que le trait visible.
            const Vector2f grip = node.horizontal ? Vector2f{2.0f, 0.0f} : Vector2f{0.0f, 2.0f};
            const WidgetId id = Hash(Hash(RootId, "#ancrage"), &i, sizeof(i));
            const bool hovered = DockMouseFree() && (activeId == 0 || activeId == id)
                              && Contains(min - grip, max + grip, mouse);
            if (hovered && mousePressed) {
                activeId = id;
                pressPosition = mouse;
            }
            bool held = false;
            if (activeId == id) {
                activeSeen = true;
                if (mouseReleased || !mouseDown) {
                    activeId = 0;
                } else {
                    held = true;
                    const float start = node.horizontal ? node.min.x : node.min.y;
                    const float length = node.horizontal ? node.max.x - node.min.x : node.max.y - node.min.y;
                    const float available = length - SplitterSize;
                    if (available > 0.0f) {
                        const float position = (node.horizontal ? mouse.x : mouse.y) - start - SplitterSize * 0.5f;
                        const float margin = std::min(MinZoneSize, available * 0.5f);
                        node.ratio = std::clamp(position, margin, available - margin) / available;
                    }
                }
            }
            // Toujours peint : sinon la couleur de fond de la scène apparaît entre deux zones.
            dockChrome.AddRect(min, max, held ? style.checkMark : hovered ? style.frameHovered : style.titleBackground);
        }
    }

    // Barre d'onglets d'une zone : les panneaux décrits à la frame précédente (ou déjà dans celle-ci).
    void Context::Impl::DockTabs(DockNode& node) {
        const float barHeight = FrameHeight();
        std::vector<WidgetId> visible;
        for (const std::string& title : node.panels) {
            const WidgetId id = PanelId(title);
            const auto found = panels.find(id);
            if (found != panels.end() && found->second->lastFrame + 1 >= frame) {
                visible.push_back(id);
            }
        }
        if (std::ranges::find(visible, node.selected) == visible.end()) {
            node.selected = visible.empty() ? 0 : visible.front();
        }

        dockChrome.AddRect(node.min, {node.max.x, std::min(node.max.y, node.min.y + barHeight)}, style.titleBackground);
        if (visible.empty()) {
            // Zone sans panneau (le jeu ne tourne pas, par exemple) : un fond, pas un trou vers la vue 3D.
            dockChrome.AddRect({node.min.x, node.min.y + barHeight}, node.max, Opaque(style.panelBackground));
        }

        dockChrome.SetClipRect(node.min, {node.max.x, node.min.y + barHeight});
        float x = node.min.x + 2.0f;
        for (const WidgetId id : visible) {
            const std::string& title = panels.at(id)->title;
            const float width = measure.MeasureText(title, style.fontSize).x + 2.0f * style.framePadding.x + 8.0f;
            const Vector2f min{x, node.min.y + 3.0f};
            const Vector2f max{x + width, node.min.y + barHeight};
            const bool hovered = DockMouseFree() && activeId == 0 && Contains(min, max, mouse)
                              && Contains(node.min, node.max, mouse);
            if (hovered && mousePressed) {
                node.selected = id;
            }
            const bool selected = node.selected == id;
            if (selected) {
                dockChrome.AddRect(min, max, Opaque(style.panelBackground));
                dockChrome.AddRect(min, {max.x, min.y + 2.0f}, style.checkMark);
            } else if (hovered) {
                dockChrome.AddRect(min, max, style.frameHovered);
            }
            dockChrome.AddText({min.x + style.framePadding.x + 4.0f, node.min.y + style.framePadding.y + 1.0f}, title,
                               style.fontSize, selected ? style.text : style.textDisabled);
            x = max.x + 2.0f;
        }
        dockChrome.ResetClipRect();
    }

    DockId Context::ClearDockLayout() {
        Impl& ui = *m_impl;
        ui.dockNodes.assign(1, {});
        ui.dockNodes[0].central = true;
        ui.RebuildDockIndex();
        return 0;
    }

    Context::DockSplit Context::SplitDock(DockId zone, DockSide side, float fraction) {
        Impl& ui = *m_impl;
        if (zone < 0 || zone >= static_cast<DockId>(ui.dockNodes.size()) || !ui.dockNodes[zone].IsLeaf()) {
            return {-1, -1};
        }
        const DockId sideId = static_cast<DockId>(ui.dockNodes.size());
        const DockId restId = sideId + 1;
        DockNode rest = std::move(ui.dockNodes[zone]); // panneaux, onglet affiché, rôle central
        ui.dockNodes.push_back({});
        ui.dockNodes.push_back(std::move(rest));

        const bool sideFirst = side == DockSide::Left || side == DockSide::Top;
        fraction = std::clamp(fraction, 0.05f, 0.95f);
        DockNode& split = ui.dockNodes[zone];
        split = {};
        split.horizontal = side == DockSide::Left || side == DockSide::Right;
        split.children[0] = sideFirst ? sideId : restId;
        split.children[1] = sideFirst ? restId : sideId;
        split.ratio = sideFirst ? fraction : 1.0f - fraction;
        ui.RebuildDockIndex();
        return {sideId, restId};
    }

    void Context::DockPanel(std::string_view title, DockId zone) {
        Impl& ui = *m_impl;
        if (zone < 0 || zone >= static_cast<DockId>(ui.dockNodes.size()) || !ui.dockNodes[zone].IsLeaf()
            || ui.dockNodes[zone].central) {
            return;
        }
        for (DockNode& node : ui.dockNodes) {
            std::erase(node.panels, title); // un panneau n'est que dans une zone
        }
        ui.dockNodes[zone].panels.emplace_back(title);
        ui.RebuildDockIndex();
    }

    Rect Context::DockSpace(Math::Vector2f min, Math::Vector2f max) {
        Impl& ui = *m_impl;
        if (ui.dockNodes.empty()) {
            ClearDockLayout();
        }
        max = {std::max(min.x, max.x), std::max(min.y, max.y)};
        ui.dockFrame = ui.frame;
        ui.dockArea = {min, max};
        ui.dockChrome.Clear();

        // Séparateurs d'après la disposition de cette frame, puis disposition recalculée avec leurs nouvelles parts.
        ui.LayoutDock(0, min, max);
        ui.DockSplitters();
        ui.LayoutDock(0, min, max);

        ui.dockCentral = {};
        for (DockNode& node : ui.dockNodes) {
            if (!node.IsLeaf()) {
                continue;
            }
            if (node.central) {
                ui.dockCentral = {node.min, node.max};
            } else {
                ui.DockTabs(node);
            }
        }
        return ui.dockCentral;
    }

    namespace {
        Json SaveNode(const std::vector<DockNode>& nodes, DockId id) {
            const auto& node = nodes[id];
            if (!node.IsLeaf()) {
                return {{"split", node.horizontal ? "horizontal" : "vertical"},
                        {"ratio", node.ratio},
                        {"children", {SaveNode(nodes, node.children[0]), SaveNode(nodes, node.children[1])}}};
            }
            Json json{{"panels", node.panels}};
            if (node.central) {
                json["central"] = true;
            }
            for (const std::string& title : node.panels) {
                if (PanelId(title) == node.selected) {
                    json["selected"] = title;
                }
            }
            return json;
        }

        // Ajoute la zone décrite par json (et ses descendantes) à nodes ; false si le texte est invalide.
        bool LoadNode(const Json& json, std::vector<DockNode>& nodes, DockId id, int depth,
                      int& centralCount) {
            if (!json.is_object() || depth > MaxDockDepth) {
                return false;
            }
            if (json.contains("split")) {
                const Json& children = json.at("children");
                const std::string split = json.at("split").get<std::string>();
                const double ratio = json.at("ratio").get<double>();
                if (!children.is_array() || children.size() != 2 || (split != "horizontal" && split != "vertical")
                    || !std::isfinite(ratio)) {
                    return false;
                }
                const DockId first = static_cast<DockId>(nodes.size());
                nodes.resize(nodes.size() + 2);
                nodes[id].horizontal = split == "horizontal";
                nodes[id].ratio = std::clamp(static_cast<float>(ratio), 0.05f, 0.95f);
                nodes[id].children[0] = first;
                nodes[id].children[1] = first + 1;
                return LoadNode(children[0], nodes, first, depth + 1, centralCount)
                    && LoadNode(children[1], nodes, first + 1, depth + 1, centralCount);
            }
            auto& node = nodes[id];
            node.central = json.value("central", false);
            node.panels = json.value("panels", std::vector<std::string>{});
            if (node.central) {
                ++centralCount;
                node.panels.clear(); // la zone centrale reste libre
            }
            if (const auto selected = json.find("selected"); selected != json.end() && selected->is_string()) {
                node.selected = PanelId(selected->get<std::string>());
            }
            return true;
        }
    }

    std::string Context::SaveDockLayout() const {
        const Impl& ui = *m_impl;
        if (ui.dockNodes.empty()) {
            return {};
        }
        const Json json{{"format", "flux.dock"}, {"version", 1}, {"root", SaveNode(ui.dockNodes, 0)}};
        return json.dump(2);
    }

    bool Context::LoadDockLayout(std::string_view text) {
        Impl& ui = *m_impl;
        const Json json = Json::parse(text, nullptr, false);
        if (json.is_discarded() || !json.is_object() || json.value("format", "") != "flux.dock"
            || json.value("version", 0) != 1 || !json.contains("root")) {
            return false;
        }
        std::vector<DockNode> nodes(1);
        int centralCount = 0;
        try {
            if (!LoadNode(json.at("root"), nodes, 0, 0, centralCount) || centralCount != 1) {
                return false;
            }
        } catch (const Json::exception&) {
            return false; // champ absent ou de mauvais type
        }
        ui.dockNodes = std::move(nodes);
        ui.RebuildDockIndex();
        return true;
    }
}
