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

    Vector2f Context::Impl::SplitterMin(const DockNode& node) const {
        const DockNode& first = dockNodes[node.children[0]];
        return node.horizontal ? Vector2f{first.max.x, node.min.y} : Vector2f{node.min.x, first.max.y};
    }

    Vector2f Context::Impl::SplitterMax(const DockNode& node) const {
        const DockNode& first = dockNodes[node.children[0]];
        return node.horizontal ? Vector2f{first.max.x + SplitterSize, node.max.y}
                               : Vector2f{node.max.x, first.max.y + SplitterSize};
    }

    WidgetId Context::Impl::SplitterId(std::size_t index) const {
        return Hash(Hash(RootId, "#ancrage"), &index, sizeof(index));
    }

    // Séparateurs : glisser pour changer la part de chaque côté.
    void Context::Impl::DockSplitterInput() {
        for (std::size_t i = 0; i < dockNodes.size(); ++i) {
            DockNode& node = dockNodes[i];
            if (node.IsLeaf()) {
                continue;
            }
            // Zone d'accroche un peu plus large que le trait visible.
            const Vector2f grip = node.horizontal ? Vector2f{2.0f, 0.0f} : Vector2f{0.0f, 2.0f};
            const WidgetId id = SplitterId(i);
            if (activeId == 0 && mousePressed && DockMouseFree()
                && Contains(SplitterMin(node) - grip, SplitterMax(node) + grip, mouse)) {
                activeId = id;
                pressPosition = mouse;
            }
            if (activeId != id) {
                continue;
            }
            activeSeen = true;
            if (mouseReleased || !mouseDown) {
                activeId = 0;
                continue;
            }
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

    void Context::Impl::DrawDockSplitters() {
        for (std::size_t i = 0; i < dockNodes.size(); ++i) {
            const DockNode& node = dockNodes[i];
            if (node.IsLeaf()) {
                continue;
            }
            const Vector2f min = SplitterMin(node);
            const Vector2f max = SplitterMax(node);
            const Vector2f grip = node.horizontal ? Vector2f{2.0f, 0.0f} : Vector2f{0.0f, 2.0f};
            const bool held = activeId == SplitterId(i);
            const bool hovered = activeId == 0 && DockMouseFree() && Contains(min - grip, max + grip, mouse);
            if (held || hovered) {
                cursor = node.horizontal ? MouseCursor::ResizeHorizontal : MouseCursor::ResizeVertical;
            }
            // Toujours peint : sinon la couleur de fond de la scène apparaît entre deux zones.
            dockChrome.AddRect(min, max, held ? style.checkMark : hovered ? style.frameHovered : style.titleBackground);
        }
    }

    // Onglets d'une zone : les panneaux décrits à la frame précédente (ou déjà dans celle-ci).
    std::vector<WidgetId> Context::Impl::VisibleTabs(const DockNode& node) const {
        std::vector<WidgetId> visible;
        for (const std::string& title : node.panels) {
            const WidgetId id = PanelId(title);
            const auto found = panels.find(id);
            if (found != panels.end() && found->second->lastFrame + 1 >= frame) {
                visible.push_back(id);
            }
        }
        return visible;
    }

    std::vector<Context::Impl::TabRect> Context::Impl::TabRects(const DockNode& node) const {
        std::vector<TabRect> tabs;
        float x = node.min.x + 2.0f;
        for (const WidgetId id : VisibleTabs(node)) {
            const std::string& title = panels.at(id)->title;
            const float width = measure.MeasureText(title, style.fontSize).x + 2.0f * style.framePadding.x + 8.0f;
            tabs.push_back({id, {x, node.min.y + 3.0f}, {x + width, node.min.y + FrameHeight()}});
            x += width + 2.0f;
        }
        return tabs;
    }

    // Clic : affiche l'onglet ; glissé de quelques pixels : le panneau sort de la zone (retourné pour UndockPanel).
    WidgetId Context::Impl::DockTabInput(DockNode& node) {
        const std::vector<TabRect> tabs = TabRects(node);
        if (std::ranges::find(tabs, node.selected, &TabRect::id) == tabs.end()) {
            node.selected = tabs.empty() ? 0 : tabs.front().id;
        }
        WidgetId undock = 0;
        for (const TabRect& tab : tabs) {
            const WidgetId tabId = Hash(tab.id, "#onglet");
            if (activeId == 0 && mousePressed && DockMouseFree() && Contains(tab.min, tab.max, mouse)
                && Contains(node.min, node.max, mouse)) {
                activeId = tabId;
                pressPosition = mouse;
                node.selected = tab.id;
            }
            if (activeId != tabId) {
                continue;
            }
            activeSeen = true;
            const Vector2f moved = mouse - pressPosition;
            if (mouseReleased || !mouseDown) {
                activeId = 0;
            } else if (std::abs(moved.x) + std::abs(moved.y) > 6.0f) {
                undock = tab.id;
            }
        }
        return undock;
    }

    void Context::Impl::DrawDockTabs(const DockNode& node) {
        const float barHeight = FrameHeight();
        const std::vector<TabRect> tabs = TabRects(node);
        dockChrome.AddRect(node.min, {node.max.x, std::min(node.max.y, node.min.y + barHeight)}, style.titleBackground);
        if (tabs.empty()) {
            // Zone sans panneau (le jeu ne tourne pas, par exemple) : un fond, pas un trou vers la vue 3D.
            dockChrome.AddRect({node.min.x, node.min.y + barHeight}, node.max, Opaque(style.panelBackground));
        }
        dockChrome.SetClipRect(node.min, {node.max.x, node.min.y + barHeight});
        for (const TabRect& tab : tabs) {
            const Vector2f min = tab.min;
            const Vector2f max = tab.max;
            const bool hovered = activeId == 0 && DockMouseFree() && Contains(min, max, mouse);
            const bool selected = node.selected == tab.id;
            if (selected) {
                dockChrome.AddRect(min, max, Opaque(style.panelBackground));
                dockChrome.AddRect(min, {max.x, min.y + 2.0f}, style.checkMark);
            } else if (hovered) {
                dockChrome.AddRect(min, max, style.frameHovered);
            }
            dockChrome.AddText({min.x + style.framePadding.x + 4.0f, node.min.y + style.framePadding.y + 1.0f},
                               panels.at(tab.id)->title, style.fontSize, selected ? style.text : style.textDisabled);
        }
        dockChrome.ResetClipRect();
    }

    // Le panneau devient flottant sous la souris, et continue d'être glissé par sa barre de titre.
    void Context::Impl::UndockPanel(WidgetId id) {
        Panel& undocked = *panels.at(id);
        for (DockNode& node : dockNodes) {
            std::erase(node.panels, undocked.fullTitle);
        }
        undocked.docked = false;
        undocked.hiddenTab = false;
        undocked.collapsed = false;
        undocked.width = undocked.userWidth > 0.0f ? undocked.userWidth : undocked.floatingWidth > 0.0f ? undocked.floatingWidth : 320.0f;
        undocked.position = mouse - Vector2f{std::min(60.0f, undocked.width * 0.3f), FrameHeight() * 0.5f};
        std::erase(order, id);
        order.push_back(id); // au premier plan

        activeId = Hash(id, "#titre");
        activeSeen = true;
        pressPosition = mouse;
        dragOrigin = undocked.position;
        dragging = true;
        RemoveEmptyZones();
    }

    // Une zone vidée de ses panneaux disparaît : sa voisine prend toute la place.
    void Context::Impl::RemoveEmptyZones() {
        bool removed = true;
        while (removed) {
            removed = false;
            for (std::size_t parent = 0; parent < dockNodes.size() && !removed; ++parent) {
                if (dockNodes[parent].IsLeaf()) {
                    continue;
                }
                for (int side = 0; side < 2; ++side) {
                    const DockNode& child = dockNodes[dockNodes[parent].children[side]];
                    if (child.IsLeaf() && !child.central && child.panels.empty()) {
                        DockNode sibling = dockNodes[dockNodes[parent].children[1 - side]];
                        dockNodes[parent] = std::move(sibling);
                        removed = true;
                        break;
                    }
                }
            }
        }
        CompactDock();
    }

    // Renumérote les zones depuis la racine : celles retirées par RemoveEmptyZones ne restent pas dans le tableau.
    void Context::Impl::CompactDock() {
        std::vector<DockNode> compact;
        compact.reserve(dockNodes.size());
        const auto copy = [&](auto&& self, DockId id) -> DockId {
            const DockId index = static_cast<DockId>(compact.size());
            compact.push_back(dockNodes[id]);
            if (!dockNodes[id].IsLeaf()) {
                const DockId first = self(self, dockNodes[id].children[0]);
                const DockId second = self(self, dockNodes[id].children[1]);
                compact[index].children[0] = first;
                compact[index].children[1] = second;
            }
            return index;
        };
        if (!dockNodes.empty()) {
            copy(copy, 0);
        }
        dockNodes = std::move(compact);
        RebuildDockIndex();
    }

    DockId Context::Impl::DockLeafAt(Vector2f point) const {
        for (std::size_t i = 0; i < dockNodes.size(); ++i) {
            const DockNode& node = dockNodes[i];
            if (node.IsLeaf() && Contains(node.min, node.max, point)) {
                return static_cast<DockId>(i);
            }
        }
        return -1;
    }

    // Croix de cibles au milieu de la zone : centre (onglet, sauf zone centrale) et quatre côtés.
    std::vector<Context::Impl::DropTarget> Context::Impl::DropTargets(DockId zone) const {
        const DockNode& node = dockNodes[zone];
        const Vector2f center = (node.min + node.max) * 0.5f;
        const float size = 30.0f;
        const float step = size + 6.0f;
        // La zone centrale (la vue) ne cède qu'un quart de sa place ; une autre zone, la moitié.
        const float fraction = node.central ? 0.25f : 0.5f;
        const Vector2f extent = node.max - node.min;
        std::vector<DropTarget> targets;
        const auto add = [&](bool isCenter, DockSide side, Vector2f offset, Rect preview) {
            DropTarget target;
            target.zone = zone;
            target.center = isCenter;
            target.side = side;
            target.min = center + offset - Vector2f{size * 0.5f, size * 0.5f};
            target.max = target.min + Vector2f{size, size};
            target.preview = preview;
            targets.push_back(target);
        };
        if (!node.central) {
            add(true, DockSide::Left, {0.0f, 0.0f}, {node.min, node.max});
        }
        add(false, DockSide::Left, {-step, 0.0f}, {node.min, {node.min.x + extent.x * fraction, node.max.y}});
        add(false, DockSide::Right, {step, 0.0f}, {{node.max.x - extent.x * fraction, node.min.y}, node.max});
        add(false, DockSide::Top, {0.0f, -step}, {node.min, {node.max.x, node.min.y + extent.y * fraction}});
        add(false, DockSide::Bottom, {0.0f, step}, {{node.min.x, node.max.y - extent.y * fraction}, node.max});
        return targets;
    }

    // Panneau flottant glissé au-dessus d'une zone : cibles affichées ; relâché sur l'une d'elles, il s'y ancre.
    void Context::Impl::DockDropInput() {
        dockTargets.Clear();
        const bool dragged = draggedPanel != 0 && draggedFrame + 1 >= frame && panels.contains(draggedPanel);
        if (!dragged) {
            draggedPanel = 0;
            return;
        }
        const DockId zone = DockLeafAt(mouse);
        if (zone < 0) {
            if (!mouseDown) {
                draggedPanel = 0;
            }
            return;
        }
        const std::vector<DropTarget> targets = DropTargets(zone);
        const DropTarget* hovered = nullptr;
        for (const DropTarget& target : targets) {
            if (Contains(target.min, target.max, mouse)) {
                hovered = &target;
            }
        }

        if (mouseDown) {
            if (hovered) {
                LinearColor preview = style.selection;
                preview.a = 0.35f;
                dockTargets.AddRect(hovered->preview.min, hovered->preview.max, preview);
            }
            for (const DropTarget& target : targets) {
                dockTargets.AddRect(target.min, target.max, &target == hovered ? style.buttonHovered : style.button);
                dockTargets.AddRectOutline(target.min, target.max, style.checkMark);
            }
            return;
        }

        // Relâché.
        const WidgetId id = draggedPanel;
        draggedPanel = 0;
        if (!hovered) {
            return;
        }
        const std::string title = panels.at(id)->fullTitle;
        DockId destination = hovered->zone;
        if (!hovered->center) {
            // Même règle que DropTargets : un quart de la vue, la moitié d'une autre zone.
            destination = SplitZone(hovered->zone, hovered->side, dockNodes[hovered->zone].central ? 0.25f : 0.5f);
        }
        for (DockNode& node : dockNodes) {
            std::erase(node.panels, title);
        }
        dockNodes[destination].panels.push_back(title);
        dockNodes[destination].selected = id;
        RebuildDockIndex();
        activeId = 0;
        dragging = false;
    }

    // Découpe une feuille : la nouvelle zone (retournée) du côté demandé, la suivante garde le contenu d'origine.
    DockId Context::Impl::SplitZone(DockId zone, DockSide side, float fraction) {
        const DockId sideId = static_cast<DockId>(dockNodes.size());
        const DockId restId = sideId + 1;
        DockNode rest = std::move(dockNodes[zone]); // panneaux, onglet affiché, rôle central
        dockNodes.push_back({});
        dockNodes.push_back(std::move(rest));

        const bool sideFirst = side == DockSide::Left || side == DockSide::Top;
        fraction = std::clamp(fraction, 0.05f, 0.95f);
        DockNode& split = dockNodes[zone];
        split = {};
        split.horizontal = side == DockSide::Left || side == DockSide::Right;
        split.children[0] = sideFirst ? sideId : restId;
        split.children[1] = sideFirst ? restId : sideId;
        split.ratio = sideFirst ? fraction : 1.0f - fraction;
        RebuildDockIndex();
        return sideId;
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
        const DockId sideId = ui.SplitZone(zone, side, fraction);
        return {sideId, sideId + 1};
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

        // Entrées d'abord (elles peuvent changer l'arbre), sur la disposition de la frame ; dessin ensuite.
        ui.LayoutDock(0, min, max);
        ui.DockSplitterInput();
        ui.DockDropInput();
        WidgetId undock = 0;
        for (DockNode& node : ui.dockNodes) {
            if (node.IsLeaf() && !node.central) {
                if (const WidgetId id = ui.DockTabInput(node); id != 0) {
                    undock = id;
                }
            }
        }
        if (undock != 0) {
            ui.UndockPanel(undock);
        }
        ui.LayoutDock(0, min, max);

        ui.DrawDockSplitters();
        ui.dockCentral = {};
        for (const DockNode& node : ui.dockNodes) {
            if (!node.IsLeaf()) {
                continue;
            }
            if (node.central) {
                ui.dockCentral = {node.min, node.max};
            } else {
                ui.DrawDockTabs(node);
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
        Json json{{"format", "flux.dock"}, {"version", 1}, {"root", SaveNode(ui.dockNodes, 0)}};
        // Panneaux flottants (hors zone), dans leur ordre d'affichage ; ceux qui ne sont pas encore réapparus
        // depuis le chargement gardent leur place lue.
        Json floating = Json::array();
        const auto saveFloating = [&](std::string_view title, const Impl::FloatingState& state) {
            floating.push_back({{"title", title}, {"x", state.position.x}, {"y", state.position.y}, {"width", state.width},
                                {"height", state.height}, {"collapsed", state.collapsed}});
        };
        for (const WidgetId id : ui.order) {
            const Impl::Panel& panel = *ui.panels.at(id);
            if (!panel.docked && !ui.dockedPanels.contains(id) && !panel.fullTitle.empty() && PanelId(panel.fullTitle) == id) {
                saveFloating(panel.fullTitle, {panel.position, panel.userWidth, panel.userHeight, panel.collapsed});
            }
        }
        for (const auto& [id, state] : ui.savedFloatingTitles) {
            if (const auto pending = ui.savedFloating.find(id); pending != ui.savedFloating.end()) {
                saveFloating(state, pending->second);
            }
        }
        json["floating"] = std::move(floating);
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
        // Facultatif (absent des fichiers plus anciens) ; une entrée abîmée est ignorée, pas la disposition.
        ui.savedFloating.clear();
        ui.savedFloatingTitles.clear();
        if (const auto floating = json.find("floating"); floating != json.end() && floating->is_array()) {
            for (const Json& entry : *floating) {
                try {
                    const std::string title = entry.at("title").get<std::string>();
                    Impl::FloatingState state;
                    state.position = {entry.at("x").get<float>(), entry.at("y").get<float>()};
                    state.width = entry.value("width", 0.0f);
                    state.height = entry.value("height", 0.0f);
                    state.collapsed = entry.value("collapsed", false);
                    if (std::isfinite(state.position.x) && std::isfinite(state.position.y) && std::isfinite(state.width)
                        && std::isfinite(state.height)) {
                        ui.savedFloating[PanelId(title)] = state;
                        ui.savedFloatingTitles[PanelId(title)] = title;
                    }
                } catch (const Json::exception&) {
                }
            }
        }
        return true;
    }
}
