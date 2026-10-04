#include "Interface/InterfaceEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/DrawList.hpp>
#include <Flux/UI/Icons.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace FluxEditor {
    using namespace Flux;
    using Math::LinearColor;
    using Math::Vector2f;

    namespace {
        constexpr std::size_t MaxUndo = 200;
        constexpr std::string_view Extension = ".fluxui";
        const Platform::FileType InterfaceFileType{"Interfaces Flux", ".fluxui"};

        bool Contains(Vector2f min, Vector2f max, Vector2f point) {
            return point.x >= min.x && point.y >= min.y && point.x < max.x && point.y < max.y;
        }
    }

    std::string InterfaceEditor::OpenInterface::GetName() const {
        if (path.empty()) {
            return "Sans fichier";
        }
        std::string name = Platform::ToUtf8(path.filename());
        if (name.ends_with(Extension)) {
            name.resize(name.size() - Extension.size());
        }
        return name;
    }

    void InterfaceEditor::Open(Engine& engine, const std::filesystem::path& path) {
        const std::filesystem::path normal = path.lexically_normal();
        for (std::size_t i = 0; i < m_open.size(); ++i) {
            if (m_open[i].path.lexically_normal() == normal) {
                m_active = static_cast<int>(i);
                return;
            }
        }
        auto loaded = Interface::LoadFile(path);
        if (!loaded) {
            Platform::ShowError(engine.GetWindow(), "Ouvrir une interface", loaded.error());
            return;
        }
        OpenInterface open;
        open.path = path;
        open.document = std::move(*loaded);
        open.savedJson = Interface::SaveToString(open.document);
        open.committedJson = open.savedJson;
        m_open.push_back(std::move(open));
        m_active = static_cast<int>(m_open.size()) - 1;
        m_drag = {};
    }

    Interface::Element* InterfaceEditor::GetSelected() {
        OpenInterface* open = GetActive();
        return open && open->selection != 0 ? Interface::Find(open->document, open->selection) : nullptr;
    }

    void InterfaceEditor::Select(std::uint32_t id) {
        if (OpenInterface* open = GetActive()) {
            open->selection = id;
        }
    }

    void InterfaceEditor::MarkChanged() {
        if (OpenInterface* open = GetActive()) {
            open->changed = true;
        }
    }

    void InterfaceEditor::CommitHistory(bool interacting) {
        OpenInterface* open = GetActive();
        if (!open || !open->changed || interacting) {
            return;
        }
        open->changed = false;
        std::string json = Interface::SaveToString(open->document);
        if (json == open->committedJson) {
            return; // valeur retapée à l'identique
        }
        open->undo.push_back({std::move(open->committedJson), open->selection});
        if (open->undo.size() > MaxUndo) {
            open->undo.erase(open->undo.begin());
        }
        open->redo.clear();
        open->committedJson = std::move(json);
    }

    void InterfaceEditor::Undo(bool redo) {
        OpenInterface* open = GetActive();
        if (!open) {
            return;
        }
        CommitHistory(false); // une modification pas encore dans l'historique est annulée en premier
        std::vector<Snapshot>& from = redo ? open->redo : open->undo;
        std::vector<Snapshot>& to = redo ? open->undo : open->redo;
        if (from.empty()) {
            return;
        }
        Snapshot snapshot = std::move(from.back());
        from.pop_back();
        auto loaded = Interface::LoadFromString(snapshot.json);
        if (!loaded) {
            return; // ne peut pas arriver : l'état vient de SaveToString
        }
        to.push_back({std::move(open->committedJson), open->selection});
        open->document = std::move(*loaded);
        open->committedJson = std::move(snapshot.json);
        open->selection = Interface::Find(open->document, snapshot.selection) ? snapshot.selection : 0;
        m_drag = {};
    }

    bool InterfaceEditor::Save(Engine& engine, OpenInterface& open) {
        if (open.path.empty()) {
            const Project* project = engine.GetProject();
            const auto path = Platform::ShowSaveFileDialog(engine.GetWindow(), "Enregistrer l'interface", InterfaceFileType,
                                                           project ? project->assetsDirectory : std::filesystem::path{},
                                                           open.GetName() + std::string(Extension));
            if (!path) {
                return false;
            }
            open.path = *path;
        }
        if (auto saved = Interface::SaveFile(open.document, open.path); !saved) {
            Platform::ShowError(engine.GetWindow(), "Enregistrer l'interface", saved.error());
            return false;
        }
        open.changed = false;
        open.committedJson = Interface::SaveToString(open.document);
        open.savedJson = open.committedJson;
        return true;
    }

    bool InterfaceEditor::Close(Engine& engine, std::size_t index) {
        OpenInterface& open = m_open[index];
        CommitHistory(false);
        if (open.IsModified()) {
            const Platform::Answer answer = Platform::AskYesNoCancel(
                engine.GetWindow(), "Interface modifiée", "Enregistrer les modifications de « " + open.GetName() + " » ?");
            if (answer == Platform::Answer::Cancel || (answer == Platform::Answer::Yes && !Save(engine, open))) {
                return false;
            }
        }
        m_open.erase(m_open.begin() + static_cast<std::ptrdiff_t>(index));
        const int closed = static_cast<int>(index);
        if (m_active == closed) {
            // L'onglet voisin prend la place ; plus aucun : retour à la scène.
            m_active = m_open.empty() ? -1 : std::min(closed, static_cast<int>(m_open.size()) - 1);
        } else if (m_active > closed) {
            --m_active;
        }
        m_drag = {};
        return true;
    }

    bool InterfaceEditor::ConfirmClose(Engine& engine) {
        for (OpenInterface& open : m_open) {
            if (open.changed) {
                open.changed = false;
                open.committedJson = Interface::SaveToString(open.document);
            }
            if (!open.IsModified()) {
                continue;
            }
            const Platform::Answer answer = Platform::AskYesNoCancel(
                engine.GetWindow(), "Interface modifiée", "Enregistrer les modifications de « " + open.GetName() + " » ?");
            if (answer == Platform::Answer::Cancel || (answer == Platform::Answer::Yes && !Save(engine, open))) {
                return false;
            }
        }
        return true;
    }

    void InterfaceEditor::FileMoved(const std::filesystem::path& from, const std::filesystem::path& to,
                                    const std::string& oldReference, const std::string& newReference) {
        for (OpenInterface& open : m_open) {
            // Onglet du fichier déplacé (ou d'un fichier du dossier déplacé).
            const std::filesystem::path relative = open.path.lexically_normal().lexically_relative(from.lexically_normal());
            if (!open.path.empty() && !relative.empty() && *relative.begin() != "..") {
                open.path = relative == "." ? to : to / relative;
            }
            // Images désignées par les éléments : le fichier sur le disque a été mis à jour par Contenu, la copie
            // ouverte suit sans devenir « modifiée » si elle ne l'était pas.
            const bool wasModified = open.IsModified();
            bool changed = false;
            const auto replace = [&](auto&& self, std::vector<Interface::Element>& elements) -> void {
                for (Interface::Element& element : elements) {
                    if (element.image == oldReference) {
                        element.image = newReference;
                        changed = true;
                    } else if (element.image.starts_with(oldReference + "/")) {
                        element.image = newReference + element.image.substr(oldReference.size());
                        changed = true;
                    }
                    self(self, element.children);
                }
            };
            replace(replace, open.document.roots);
            if (changed) {
                open.committedJson = Interface::SaveToString(open.document);
                if (!wasModified) {
                    open.savedJson = open.committedJson;
                }
                open.undo.clear(); // les anciens états désignent les anciens chemins
                open.redo.clear();
            }
        }
    }

    void InterfaceEditor::FileDeleted(const std::filesystem::path& path) {
        for (OpenInterface& open : m_open) {
            if (!open.path.empty() && open.path.lexically_normal() == path.lexically_normal()) {
                open.path.clear();
                open.savedJson.clear(); // à enregistrer sous un autre nom
            }
        }
    }

    UI::Rect InterfaceEditor::DrawTabs(Engine& engine, const UI::Rect& view) {
        m_hoveredTab = -1;
        if (m_open.empty()) {
            m_active = -1;
            return view;
        }
        UI::Context& ui = engine.GetUi();
        const UI::Style& style = ui.GetStyle();
        const Input& input = engine.GetWindow().GetInput();
        UI::DrawList& list = engine.GetRenderer().GetOverlay();
        const float barHeight = std::round(style.fontSize + 2.0f * style.framePadding.y + 10.0f);
        const Vector2f barMax{view.max.x, std::min(view.max.y, view.min.y + barHeight)};
        const UI::Rect rest{{view.min.x, barMax.y}, view.max};
        engine.GetRenderer().SetSceneViewport(rest.min, rest.max);

        list.SetClipRect(view.min, barMax);
        list.AddRect(view.min, barMax, style.windowBackground);
        const Vector2f mouse = input.GetMousePosition();
        const bool mouseFree = !ui.WantsMouse() && Contains(view.min, barMax, mouse);
        const float iconSize = std::round(style.fontSize);
        const float closeSize = std::round(style.fontSize * 0.9f);

        // L'onglet de la scène, puis une interface par onglet.
        std::optional<int> clicked;
        std::optional<int> closed;
        float x = view.min.x + 6.0f;
        for (int tab = -1; tab < static_cast<int>(m_open.size()); ++tab) {
            const bool scene = tab < 0;
            const OpenInterface* open = scene ? nullptr : &m_open[static_cast<std::size_t>(tab)];
            const std::string label = scene ? "Scène" : open->GetName() + (open->IsModified() ? " *" : "");
            const float textWidth = ui.MeasureText(label).x;
            const float width = 10.0f + iconSize + 6.0f + textWidth + (scene ? 12.0f : 8.0f + closeSize + 6.0f);
            const Vector2f min{x, view.min.y + 5.0f};
            const Vector2f max{x + width, barMax.y};
            x += width + 3.0f;

            const bool selected = m_active == tab;
            const bool hovered = mouseFree && Contains(min, max, mouse);
            const Vector2f closeMin{max.x - 6.0f - closeSize, 0.5f * (min.y + max.y - closeSize)};
            const Vector2f closeMax = closeMin + Vector2f{closeSize, closeSize};
            const bool closeHovered = !scene && hovered && Contains(closeMin, closeMax, mouse);
            if (hovered) {
                m_hoveredTab = tab;
                if (input.WasMouseButtonPressed(MouseButton::Left)) {
                    (closeHovered ? closed : clicked) = tab;
                } else if (!scene && input.WasMouseButtonPressed(MouseButton::Middle)) {
                    closed = tab;
                }
            }

            // Coins arrondis en haut seulement : la forme déborde sous la barre, découpée.
            const float radius = std::max(style.frameRounding, 4.0f);
            if (selected || hovered) {
                list.SetClipRect(min, max);
                list.AddRoundedRect(min, max + Vector2f{0.0f, radius}, selected ? style.panelBackground : style.frameHovered,
                                    radius);
                if (selected) {
                    list.AddRoundedRect(min, {max.x, min.y + 2.0f * radius}, style.checkMark, radius);
                    list.AddRect({min.x, min.y + 2.0f}, {max.x, min.y + 2.0f * radius}, style.panelBackground);
                }
                list.SetClipRect(view.min, barMax);
            }
            UI::DrawIcon(list, scene ? UI::Icon::Scene : UI::Icon::Window, {min.x + 10.0f, 0.5f * (min.y + max.y - iconSize)},
                         iconSize);
            list.AddText({min.x + 10.0f + iconSize + 6.0f, 0.5f * (min.y + max.y - style.fontSize) - 1.0f}, label,
                         style.fontSize, selected ? style.text : style.textDisabled);
            if (!scene && (selected || hovered)) {
                if (closeHovered) {
                    list.AddRoundedRect(closeMin, closeMax, style.buttonHovered, 3.0f);
                }
                const float inset = closeSize * 0.3f;
                const LinearColor cross = closeHovered ? style.text : style.textDisabled;
                list.AddLine(closeMin + Vector2f{inset, inset}, closeMax - Vector2f{inset, inset}, cross, 1.5f);
                list.AddLine({closeMax.x - inset, closeMin.y + inset}, {closeMin.x + inset, closeMax.y - inset}, cross, 1.5f);
            }
        }
        list.ResetClipRect();

        if (clicked) {
            m_active = *clicked;
            m_drag = {};
        }
        if (closed) {
            Close(engine, static_cast<std::size_t>(*closed));
        }
        return rest;
    }

    void InterfaceEditor::Update(Engine& engine, const UI::Rect& view) {
        const bool active = IsActive();
        // L'éditeur couvre la vue : la scène 3D n'est pas dessinée et la caméra ne bouge pas.
        engine.SetCameraInputEnabled(!active);
        engine.GetRenderer().SetSceneVisible(!active);
        UI::Context& ui = engine.GetUi();
        if (active && !m_wasActive) {
            // Les panneaux de l'éditeur d'interface se rangent à côté de Scènes et de l'Inspecteur (en onglets), et
            // passent devant à chaque retour sur une interface.
            if (!m_panelsPlaced) {
                ui.DockPanelNextTo("Interface", "Scènes");
                ui.DockPanelNextTo("Propriétés", "Inspecteur");
                m_panelsPlaced = true;
            }
            ui.FocusPanel("Interface");
            ui.FocusPanel("Propriétés");
        }
        m_wasActive = active;
        if (!active) {
            m_drag = {};
            return;
        }
        UpdateCanvas(engine, view);
        DrawElementsPanel(engine);
        DrawPropertiesPanel(engine);
        CommitHistory(ui.IsInteracting() || m_drag.kind != Drag::Kind::None);
    }

    void InterfaceEditor::HandleShortcuts(Engine& engine) {
        OpenInterface* open = GetActive();
        UI::Context& ui = engine.GetUi();
        if (!open || ui.WantsKeyboard()) {
            return;
        }
        const Input& input = engine.GetWindow().GetInput();
        if (input.IsControlDown()) {
            if (input.WasLetterPressed('S')) {
                CommitHistory(false);
                Save(engine, *open);
            } else if (input.WasLetterPressed('Z')) {
                Undo(input.IsShiftDown());
            } else if (input.WasLetterPressed('Y')) {
                Undo(true);
            } else if (input.WasLetterPressed('D')) {
                Duplicate();
            }
            return;
        }
        if (m_drag.kind == Drag::Kind::Move || m_drag.kind == Drag::Kind::Resize) {
            return; // geste en cours : il finit d'abord
        }
        if (input.WasKeyPressed(Key::Delete)) {
            Delete();
        } else if (input.WasKeyPressed(Key::Escape)) {
            Select(0);
        }
        // Flèches : un pixel de référence, dix avec Maj (répétées tant que la touche est tenue).
        const float step = input.IsShiftDown() ? 10.0f : 1.0f;
        Vector2f delta;
        delta.x = (input.WasKeyTyped(Key::Right) ? step : 0.0f) - (input.WasKeyTyped(Key::Left) ? step : 0.0f);
        delta.y = (input.WasKeyTyped(Key::Down) ? step : 0.0f) - (input.WasKeyTyped(Key::Up) ? step : 0.0f);
        if (delta.x != 0.0f || delta.y != 0.0f) {
            Nudge(delta);
        }
    }

    void InterfaceEditor::DrawEditMenuItems(UI::Context& ui) {
        OpenInterface* open = GetActive();
        if (!open) {
            return;
        }
        const bool canUndo = !open->undo.empty() || open->changed;
        if (ui.MenuItem("Annuler##interface", "Ctrl+Z", canUndo)) {
            Undo(false);
        }
        if (ui.MenuItem("Rétablir##interface", "Ctrl+Y", !open->redo.empty())) {
            Undo(true);
        }
        ui.Separator();
        if (ui.MenuItem("Dupliquer", "Ctrl+D", open->selection != 0)) {
            Duplicate();
        }
        if (ui.MenuItem("Supprimer##interface", "Suppr", open->selection != 0)) {
            Delete();
        }
    }

    void InterfaceEditor::DrawFileMenuItems(UI::Context& ui, Engine& engine) {
        OpenInterface* open = GetActive();
        if (!open) {
            return;
        }
        if (ui.MenuItem("Enregistrer l'interface", "Ctrl+S")) {
            CommitHistory(false);
            Save(engine, *open);
        }
        if (ui.MenuItem("Fermer l'interface")) {
            Close(engine, static_cast<std::size_t>(m_active));
        }
    }

    namespace {
        // Page d'onglets : un cadre transparent qui remplit la zone sous les onglets.
        Interface::Element MakePage(Interface::Document& document, bool shown) {
            Interface::Element page = Interface::MakeElement(document, Interface::ElementType::Frame);
            page.name = "Onglet " + std::to_string(page.id);
            page.anchor = Interface::Anchor::Stretch;
            page.position = {};
            page.size = {};
            page.background = {0.0f, 0.0f, 0.0f, 0.0f}; // transparente : le fond des onglets se voit
            page.radius = 0.0f;
            page.visible = shown;
            return page;
        }
    }

    void InterfaceEditor::Add(Interface::ElementType type) {
        OpenInterface* open = GetActive();
        if (!open) {
            return;
        }
        Interface::Document& document = open->document;
        Interface::Element element = Interface::MakeElement(document, type);
        const std::uint32_t id = element.id;
        if (type == Interface::ElementType::Tabs) {
            // Deux pages pour commencer.
            for (int page = 0; page < 2; ++page) {
                element.children.push_back(MakePage(document, page == 0));
            }
        }
        Interface::Element* selected = GetSelected();
        if (!selected && type != Interface::ElementType::Window) {
            // Rien de sélectionné : dans la dernière fenêtre (celle de devant), pas à côté d'elle, où l'élément
            // resterait affiché la fenêtre fermée.
            const auto window = std::ranges::find_if(document.roots | std::views::reverse, [](const Interface::Element& root) {
                return root.type == Interface::ElementType::Window;
            });
            if (window != (document.roots | std::views::reverse).end()) {
                selected = &*window;
            }
        }
        if (selected && selected->type == Interface::ElementType::Tabs) {
            if (type == Interface::ElementType::Frame) {
                // Un cadre ajouté aux onglets est une nouvelle page, montrée tant qu'elle est sélectionnée.
                Interface::Element page = MakePage(document, selected->children.empty());
                const std::uint32_t pageId = page.id;
                selected->children.push_back(std::move(page));
                open->selection = pageId;
                MarkChanged();
                return;
            }
            // Le reste va dans la page affichée (une page d'abord s'il n'y en a pas).
            if (selected->children.empty()) {
                selected->children.push_back(MakePage(document, true));
            }
            selected = &selected->children[static_cast<std::size_t>(Interface::GetShownTab(*selected))];
        }
        if (selected && Interface::IsContainer(selected->type)) {
            // Dans la fenêtre ou le cadre sélectionné, au centre ; jamais plus grand que lui.
            element.size = {std::min(element.size.x, std::max(20.0f, selected->size.x - 40.0f)),
                            std::min(element.size.y, std::max(20.0f, selected->size.y - 40.0f))};
            if (selected->anchor == Interface::Anchor::Stretch) {
                element.size = Interface::MakeDefault(type).size;
            }
            selected->children.push_back(std::move(element));
        } else if (selected) {
            // À côté de l'élément sélectionné, juste devant lui.
            std::size_t index = 0;
            std::vector<Interface::Element>* siblings = Interface::FindSiblings(document, selected->id, index);
            siblings->insert(siblings->begin() + static_cast<std::ptrdiff_t>(index + 1), std::move(element));
        } else {
            document.roots.push_back(std::move(element));
        }
        open->selection = id;
        MarkChanged();
    }

    void InterfaceEditor::Duplicate() {
        OpenInterface* open = GetActive();
        Interface::Element* selected = GetSelected();
        if (!open || !selected) {
            return;
        }
        Interface::Element copy = Interface::CloneWithNewIds(open->document, *selected);
        copy.name += " (copie)";
        if (copy.anchor != Interface::Anchor::Stretch) {
            copy.position += Vector2f{20.0f, 20.0f}; // décalée : on voit qu'il y en a deux
        }
        const std::uint32_t id = copy.id;
        std::size_t index = 0;
        std::vector<Interface::Element>* siblings = Interface::FindSiblings(open->document, selected->id, index);
        siblings->insert(siblings->begin() + static_cast<std::ptrdiff_t>(index + 1), std::move(copy));
        open->selection = id;
        MarkChanged();
    }

    void InterfaceEditor::Delete() {
        OpenInterface* open = GetActive();
        if (!open || open->selection == 0) {
            return;
        }
        const Interface::Element* parent = Interface::FindParent(open->document, open->selection);
        const std::uint32_t parentId = parent ? parent->id : 0;
        if (Interface::Remove(open->document, open->selection)) {
            open->selection = parentId;
            MarkChanged();
        }
    }

    void InterfaceEditor::Reorder(std::uint32_t id, int direction) {
        OpenInterface* open = GetActive();
        if (!open) {
            return;
        }
        std::size_t index = 0;
        std::vector<Interface::Element>* siblings = Interface::FindSiblings(open->document, id, index);
        const auto target = static_cast<std::ptrdiff_t>(index) + direction;
        if (!siblings || target < 0 || target >= static_cast<std::ptrdiff_t>(siblings->size())) {
            return;
        }
        std::swap((*siblings)[index], (*siblings)[static_cast<std::size_t>(target)]);
        MarkChanged();
    }

    void InterfaceEditor::Nudge(Vector2f delta) {
        Interface::Element* selected = GetSelected();
        if (!selected) {
            return;
        }
        selected->position += delta;
        if (selected->anchor == Interface::Anchor::Stretch) {
            selected->size -= delta; // marges droite et bas : l'élément garde sa taille
        }
        MarkChanged();
    }
}
