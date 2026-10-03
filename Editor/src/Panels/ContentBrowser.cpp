#include "Panels/ContentBrowser.hpp"

#include "Document/SceneDocument.hpp"
#include "Interface/InterfaceEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Interface/Interface.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Icons.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/Context.hpp>

#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <system_error>
#include <utility>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        constexpr double ScanInterval = 2.0; // secondes : quelques dizaines de fichiers, lecture négligeable

        constexpr std::string_view SceneExtension = ".scene.json";

        bool IsScene(const std::filesystem::path& path) {
            return Platform::ToUtf8(path.filename()).ends_with(SceneExtension);
        }

        // Nom affiché d'une scène : son fichier sans « .scene.json ».
        std::string SceneName(const std::filesystem::path& path) {
            std::string name = Platform::ToUtf8(path.filename());
            if (name.ends_with(SceneExtension)) {
                name.resize(name.size() - SceneExtension.size());
            }
            return name;
        }

        constexpr std::string_view InterfaceExtension = ".fluxui";

        bool IsInterface(const std::filesystem::path& path) {
            return Platform::ToUtf8(path.filename()).ends_with(InterfaceExtension);
        }

        // Images désignées par les éléments d'une interface : réécrites comme ReplaceReferences pour les scènes.
        bool ReplaceImageReferences(std::vector<Interface::Element>& elements, const std::string& from, const std::string& to) {
            bool changed = false;
            for (Interface::Element& element : elements) {
                if (element.image == from) {
                    element.image = to;
                    changed = true;
                } else if (element.image.starts_with(from + "/")) {
                    element.image = to + element.image.substr(from.size());
                    changed = true;
                }
                changed |= ReplaceImageReferences(element.children, from, to);
            }
            return changed;
        }

        constexpr float DragThreshold = 4.0f; // pixels avant qu'une vignette tenue devienne un glisser-déposer

        // Caractères refusés par Windows dans un nom de fichier.
        bool IsValidName(std::string_view name) {
            return !name.empty() && name.find_first_of("\\/:*?\"<>|") == std::string_view::npos && name.back() != '.'
                && name.back() != ' ';
        }

        // Badge et couleur d'une vignette, d'après le type du fichier.
        std::pair<std::string, Math::LinearColor> Badge(const auto& entry) {
            if (entry.directory) {
                return {"Dossier", Math::LinearColor::FromSrgb8(150, 118, 52)};
            }
            std::string extension = Platform::ToUtf8(entry.path.extension());
            std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".glb" || extension == ".gltf") {
                return {"3D", Math::LinearColor::FromSrgb8(52, 92, 150)};
            }
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp") {
                return {"Image", Math::LinearColor::FromSrgb8(52, 128, 80)};
            }
            if (extension == InterfaceExtension) {
                return {"Interface", Math::LinearColor::FromSrgb8(40, 140, 160)};
            }
            if (extension == ".ttf" || extension == ".otf") {
                return {"Police", Math::LinearColor::FromSrgb8(120, 72, 140)};
            }
            if (extension == ".wav" || extension == ".ogg" || extension == ".mp3") {
                return {"Son", Math::LinearColor::FromSrgb8(150, 80, 60)};
            }
            std::string upper = extension.empty() ? std::string("?") : extension.substr(1);
            std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return {upper, Math::LinearColor::FromSrgb8(84, 84, 92)};
        }

        // Champs de fichiers (modèle, image, scène) qui désignent `from` ou un fichier sous lui (dossier déplacé) :
        // réécrits vers `to`. true si quelque chose a changé.
        bool ReplaceReferences(Scene& scene, const ComponentRegistry& components, const std::string& from, const std::string& to) {
            bool changed = false;
            entt::registry& registry = scene.GetRegistry();
            for (std::size_t c = 0; c < components.GetCount(); ++c) {
                const ComponentInfo& info = components.Get(c);
                for (const FieldInfo& field : info.fields) {
                    if (field.type != FieldType::String || field.asset == AssetType::None || field.asset == AssetType::Animation) {
                        continue;
                    }
                    for (const entt::entity entity : registry.view<IdComponent>()) {
                        void* component = info.get(registry, entity);
                        if (!component) {
                            continue;
                        }
                        std::string& value = *static_cast<std::string*>(field.access(component));
                        if (value == from) {
                            value = to;
                            changed = true;
                        } else if (value.starts_with(from + "/")) {
                            value = to + value.substr(from.size());
                            changed = true;
                        }
                    }
                }
            }
            return changed;
        }
    }

    void ContentBrowser::Update(Engine& engine, SceneDocument& document, double deltaSeconds) {
        const Project* project = engine.GetProject();
        const std::filesystem::path root = project ? project->assetsDirectory : std::filesystem::path{};
        m_sinceScan += deltaSeconds;
        if (root != m_root || m_sinceScan >= ScanInterval) {
            if (root != m_root) {
                m_folder = root;
                m_sceneFolder = root;
                m_selected.clear();
            }
            m_root = root;
            m_sinceScan = 0.0;
            Scan(root);
        }
        // Dossier affiché supprimé ou renommé hors de l'éditeur : retour à la racine.
        const Entry* folder = FindFolder(m_tree, m_folder);
        if (!folder) {
            m_folder = m_root;
            folder = &m_tree;
        }

        UI::Context& ui = engine.GetUi();
        const Input& input = engine.GetWindow().GetInput();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        // Position sans ancrage (l'éditeur l'ancre en bas) : à droite, sous le panneau « Démo » du jeu.
        const float top = 150.0f + ui.GetMenuBarHeight();
        ui.SetNextPanelIcon(UI::Icon::Content);
        if (ui.BeginPanel("Contenu", {std::max(316.0f, static_cast<float>(screen.x) - 368.0f), top}, 360.0f,
                          std::max(160.0f, static_cast<float>(screen.y) - top - 8.0f))) {
            if (!project) {
                ui.TextDisabled("Aucun projet ouvert.");
            } else if (root.empty() || !std::filesystem::is_directory(root)) {
                ui.TextDisabled("Dossier des assets introuvable :\n" + Platform::ToUtf8(root));
            } else {
                // Chemin du dossier affiché, et de quoi remonter d'un cran.
                ui.BeginDisabled(m_folder == m_root);
                if (ui.Button("Remonter")) {
                    m_folder = m_folder.parent_path();
                    m_selected.clear();
                }
                ui.EndDisabled();
                ui.SameLine();
                const std::filesystem::path relative = m_folder.lexically_relative(m_root);
                ui.TextDisabled(relative.empty() || relative == "." ? "assets" : "assets/" + Platform::ToGenericUtf8(relative));

                const float treeWidth = std::clamp(ui.GetAvailableWidth() * 0.3f, 120.0f, 260.0f);
                ui.BeginChild("dossiers", treeWidth, 0.0f);
                DrawFolder(ui, m_tree);
                ui.EndChild();
                ui.SameLine();
                ui.BeginChild("fichiers", 0.0f, 0.0f);
                DrawTiles(ui, engine, document, *folder, m_folder, false);
                ui.EndChild();
                // Clic droit dans le fond (hors vignette) : actions sur le dossier affiché.
                if (ui.BeginContextMenu("fond")) {
                    if (ui.MenuItem("Nouveau dossier...", {}, !document.IsLocked())) {
                        m_request = NameRequest::NewFolder;
                        m_requestPath = m_folder;
                        m_requestName = "Nouveau dossier";
                    }
                    if (m_interfaces && ui.MenuItem("Nouvelle interface...", {}, !document.IsLocked())) {
                        m_request = NameRequest::NewInterface;
                        m_requestPath = m_folder;
                        m_requestName = "Nouvelle interface";
                    }
                    if (ui.MenuItem("Afficher dans l'explorateur")) {
                        Platform::ShowInExplorer(m_folder);
                    }
                    ui.EndMenu();
                }
            }
        }
        ui.EndPanel();

        // Glisser-déposer : commencé en tenant une vignette (ici ou dans l'onglet Scènes, décrit plus tôt dans la
        // frame), posé en relâchant sur un dossier. La cible est effacée après : chaque frame la recalcule.
        if (!m_drag.path.empty()) {
            const Math::Vector2f moved = input.GetMousePosition() - m_drag.pressMouse;
            m_drag.dragging |= std::sqrt(moved.x * moved.x + moved.y * moved.y) > DragThreshold;
            if (!input.IsMouseButtonDown(MouseButton::Left)) {
                if (m_drag.dragging && !m_drag.target.empty()) {
                    const std::filesystem::path from = m_drag.path;
                    Move(engine, document, from, m_drag.target / from.filename());
                }
                m_drag = {};
            }
        }
        m_drag.target.clear();
        DrawNameWindow(engine, document);
        DrawRenameWindow(engine, document);
    }

    void ContentBrowser::Scan(const std::filesystem::path& root) {
        // Récursif ; erreurs ignorées (fichier supprimé pendant la lecture, droits) : l'entrée manque, c'est tout.
        // Un seul parcours pour deux arbres : les assets sans les scènes (un dossier qui ne contenait que des scènes
        // n'y apparaît pas), et les scènes avec les dossiers qui en contiennent ou qui sont vides (tout juste créés).
        m_scenes.clear();
        m_interfaceFiles.clear();
        struct Trees
        {
            std::vector<Entry> assets;
            std::vector<Entry> scenes;
        };
        const auto read = [this](const auto& self, const std::filesystem::path& directory) -> Trees {
            Trees trees;
            std::error_code error;
            for (auto it = std::filesystem::directory_iterator(directory, error);
                 !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
                Entry entry;
                entry.path = it->path();
                entry.name = Platform::ToUtf8(entry.path.filename());
                entry.directory = it->is_directory(error);
                if (entry.directory) {
                    Trees children = self(self, entry.path);
                    const bool empty = children.assets.empty() && children.scenes.empty();
                    if (!children.scenes.empty() || empty) {
                        trees.scenes.push_back({entry.path, entry.name, true, std::move(children.scenes)});
                    }
                    if (!children.assets.empty() || empty) {
                        entry.children = std::move(children.assets);
                        trees.assets.push_back(std::move(entry));
                    }
                } else if (IsScene(entry.path)) {
                    m_scenes.push_back(entry.path);
                    trees.scenes.push_back(std::move(entry));
                } else {
                    if (IsInterface(entry.path)) {
                        m_interfaceFiles.push_back(entry.path);
                    }
                    trees.assets.push_back(std::move(entry));
                }
            }
            // Dossiers d'abord, puis ordre alphabétique : le même d'une lecture à l'autre.
            const auto order = [](const Entry& a, const Entry& b) {
                return a.directory != b.directory ? a.directory : a.name < b.name;
            };
            std::ranges::sort(trees.assets, order);
            std::ranges::sort(trees.scenes, order);
            return trees;
        };
        Trees trees = root.empty() ? Trees{} : read(read, root);
        m_tree = {root, "assets", true, std::move(trees.assets)};
        m_sceneTree = {root, "assets", true, std::move(trees.scenes)};
        std::ranges::sort(m_scenes, [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.parent_path() != b.parent_path() ? a.parent_path() < b.parent_path() : a.filename() < b.filename();
        });
    }

    const ContentBrowser::Entry* ContentBrowser::FindFolder(const Entry& tree, const std::filesystem::path& path) const {
        const auto find = [&](const auto& self, const Entry& entry) -> const Entry* {
            if (entry.path == path) {
                return &entry;
            }
            for (const Entry& child : entry.children) {
                if (child.directory) {
                    if (const Entry* found = self(self, child)) {
                        return found;
                    }
                }
            }
            return nullptr;
        };
        return m_root.empty() ? nullptr : find(find, tree);
    }

    void ContentBrowser::DropTarget(UI::Context& ui, const std::filesystem::path& folder) {
        if (!m_drag.dragging || !ui.IsMouseOverItem() || folder == m_drag.path.parent_path()) {
            return;
        }
        // Pas dans lui-même ni dans un de ses sous-dossiers.
        const std::filesystem::path relative = folder.lexically_relative(m_drag.path);
        if (!relative.empty() && *relative.begin() != "..") {
            return;
        }
        m_drag.target = folder;
        if (UI::DrawList* list = ui.GetPanelDrawList()) {
            const UI::Rect rect = ui.GetItemRect();
            list->AddRectOutline(rect.min, rect.max, ui.GetStyle().checkMark, 2.0f);
        }
    }

    // Arbre des dossiers seulement (les fichiers sont à droite). Clic : affiche le dossier.
    void ContentBrowser::DrawFolder(UI::Context& ui, const Entry& folder) {
        const bool hasFolders = std::ranges::any_of(folder.children, &Entry::directory);
        ui.PushId(folder.name);
        // Le dossier affiché et ses parents se déplient d'eux-mêmes (après un double-clic à droite).
        const std::filesystem::path relative = m_folder.lexically_relative(folder.path);
        if (hasFolders && folder.path != m_folder && !relative.empty() && *relative.begin() != "..") {
            ui.SetNextTreeNodeOpen(true);
        }
        const UI::Context::TreeNodeResult node = ui.TreeNode(folder.name, folder.path == m_folder, !hasFolders,
                                                            folder.path == m_root);
        if (node.clicked) {
            m_folder = folder.path;
            m_selected.clear();
        }
        DropTarget(ui, folder.path);
        if (node.open) {
            for (const Entry& child : folder.children) {
                if (child.directory) {
                    DrawFolder(ui, child);
                }
            }
            ui.TreePop();
        }
        ui.PopId();
    }

    // Contenu d'un dossier en vignettes : ses dossiers puis ses fichiers, en grille. scenes : l'onglet Scènes
    // (vignettes de scènes, double-clic pour ouvrir, la scène ouverte en surbrillance) ; folder : le dossier affiché
    // du panneau, changé par un double-clic sur un dossier.
    void ContentBrowser::DrawTiles(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& shown,
                                   std::filesystem::path& folder, bool scenes) {
        if (shown.children.empty()) {
            ui.TextDisabled(scenes ? "Aucune scène ici. Clic droit : nouveau dossier." :
                                     "Dossier vide. Clic droit : nouveau dossier.\nGlisser une vignette sur un dossier pour l'y ranger.");
            return;
        }
        constexpr float TileWidth = 84.0f;
        const float spacing = ui.GetStyle().itemSpacing;
        const int columns = std::max(1, static_cast<int>((ui.GetAvailableWidth() + spacing) / (TileWidth + spacing)));
        const std::filesystem::path openScene = document.GetPath().lexically_normal();
        int column = 0;
        for (const Entry& entry : shown.children) {
            if (column++ % columns != 0) {
                ui.SameLine();
            }
            const bool scene = scenes && !entry.directory;
            const bool startup = scene && IsStartupScene(engine, entry.path);
            const auto [badge, color] = scene     ? std::pair<std::string, Math::LinearColor>{startup ? "Démarrage" : "",
                                                                                            Math::LinearColor::FromSrgb8(52, 128, 80)}
                                        : entry.directory ? std::pair<std::string, Math::LinearColor>{}
                                                          : Badge(entry);
            const bool interfaceFile = !entry.directory && IsInterface(entry.path);
            const UI::Icon icon = entry.directory ? UI::Icon::Folder
                                : scene           ? UI::Icon::Scene
                                : interfaceFile   ? UI::Icon::Window
                                                  : UI::Icon::File;
            const bool selected = scene ? entry.path.lexically_normal() == openScene : entry.path == m_selected;
            ui.PushId(entry.name);
            const UI::Context::TileResult tile = ui.Tile(scene ? SceneName(entry.path) : entry.name, icon, badge, color, selected,
                                                         TileWidth, scene || interfaceFile ? nullptr : GetIcon(engine, entry));
            if (tile.clicked && !scene) {
                m_selected = entry.path;
            }
            if (tile.doubleClicked && scene && !selected && !document.IsLocked()) {
                document.OpenFile(engine, entry.path); // propose d'enregistrer la scène en cours d'abord
            }
            if (tile.doubleClicked && entry.directory) {
                folder = entry.path;
                m_selected.clear();
            }
            if (tile.doubleClicked && interfaceFile && m_interfaces) {
                m_interfaces->Open(engine, entry.path);
            }
            if (ui.IsItemActive() && m_drag.path.empty() && !document.IsLocked()) {
                m_drag = {};
                m_drag.path = entry.path;
                m_drag.pressMouse = engine.GetWindow().GetInput().GetMousePosition();
            }
            if (entry.directory) {
                DropTarget(ui, entry.path);
            }
            if (scene) {
                if (!document.IsLocked()) {
                    ShowSceneMenu(ui, engine, document, entry.path);
                }
            } else {
                ShowContextMenu(ui, engine, document, entry, folder);
            }
            ui.PopId();
            if (tile.doubleClicked && entry.directory) {
                break; // le contenu affiché change : la suite de la liste n'est plus la bonne
            }
        }
    }

    void ContentBrowser::DrawSceneBrowser(UI::Context& ui, Engine& engine, SceneDocument& document, float height) {
        if (m_root.empty()) {
            return; // pas encore lu (première frame), ou pas de projet
        }
        // Une autre scène vient d'être ouverte : son dossier est affiché.
        const std::filesystem::path open = document.GetPath().lexically_normal();
        if (open != m_shownScene) {
            m_shownScene = open;
            if (!open.empty() && FindFolder(m_sceneTree, open.parent_path())) {
                m_sceneFolder = open.parent_path();
            }
        }
        const Entry* shown = FindFolder(m_sceneTree, m_sceneFolder);
        if (!shown) {
            m_sceneFolder = m_root;
            shown = &m_sceneTree;
        }
        ui.BeginDisabled(m_sceneFolder == m_root);
        if (ui.Button("Remonter")) {
            m_sceneFolder = m_sceneFolder.parent_path();
        }
        ui.EndDisabled();
        ui.SameLine();
        const std::filesystem::path relative = m_sceneFolder.lexically_relative(m_root);
        ui.TextDisabled(relative.empty() || relative == "." ? "assets" : "assets/" + Platform::ToGenericUtf8(relative));
        ui.BeginChild("scenes", 0.0f, height);
        DrawTiles(ui, engine, document, *shown, m_sceneFolder, true);
        ui.EndChild();
        if (ui.BeginContextMenu("fond")) {
            if (ui.MenuItem("Nouveau dossier...", {}, !document.IsLocked())) {
                m_request = NameRequest::NewFolder;
                m_requestPath = m_sceneFolder;
                m_requestName = "Nouveau dossier";
            }
            if (ui.MenuItem("Afficher dans l'explorateur")) {
                Platform::ShowInExplorer(m_sceneFolder);
            }
            ui.EndMenu();
        }
    }

    const UI::TextureInfo* ContentBrowser::GetIcon(Engine& engine, const Entry& entry) {
        std::string key = "/";
        if (!entry.directory) {
            key = Platform::ToUtf8(entry.path.extension());
            std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        auto [found, created] = m_icons.try_emplace(key);
        if (created) {
            if (const std::optional<ImageData> icon = Platform::GetShellIcon(entry.directory ? std::string_view{} : key)) {
                found->second = engine.GetRenderer().CreateTexture(*icon);
            }
        }
        return found->second ? &*found->second : nullptr;
    }

    void ContentBrowser::ShowContextMenu(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& entry,
                                         std::filesystem::path& folder) {
        if (!ui.BeginContextMenu()) {
            return;
        }
        if (entry.directory && ui.MenuItem("Ouvrir")) {
            folder = entry.path;
            m_selected.clear();
        }
        if (!entry.directory && IsInterface(entry.path) && m_interfaces && ui.MenuItem("Ouvrir dans l'éditeur d'interface")) {
            m_interfaces->Open(engine, entry.path);
        }
        if (ui.MenuItem("Renommer...", {}, !document.IsLocked())) {
            m_request = NameRequest::Rename;
            m_requestPath = entry.path;
            m_requestName = entry.name;
        }
        if (ui.MenuItem("Afficher dans l'explorateur")) {
            Platform::ShowInExplorer(entry.path);
        }
        // Les dossiers se suppriment dans l'explorateur : trop facile de perdre beaucoup d'un coup ici.
        if (!entry.directory && ui.MenuItem("Supprimer")) {
            Delete(engine, document, entry.path);
        }
        ui.EndMenu();
    }

    void ContentBrowser::DrawNameWindow(Engine& engine, SceneDocument& document) {
        if (m_request == NameRequest::None) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        const float width = 400.0f;
        const bool create = m_request == NameRequest::NewFolder || m_request == NameRequest::NewInterface;
        const std::string_view title = m_request == NameRequest::NewFolder      ? "Nouveau dossier"
                                       : m_request == NameRequest::NewInterface ? "Nouvelle interface"
                                                                                : "Renommer";
        if (ui.BeginPanel(title, {(static_cast<float>(screen.x) - width) * 0.5f,
                                                                    static_cast<float>(screen.y) * 0.3f},
                          width)) {
            if (!create) {
                ui.TextDisabled(Platform::ToUtf8(m_requestPath.filename()));
            }
            const bool entered = ui.InputText("Nom", m_requestName);
            ui.Separator();
            if (ui.Button(create ? "Créer" : "Renommer") || entered) {
                const Window& window = engine.GetWindow();
                if (!IsValidName(m_requestName)) {
                    Platform::ShowError(window, "Nom", "Nom invalide : il ne doit pas être vide ni contenir \\ / : * ? \" < > |");
                } else if (m_request == NameRequest::NewInterface) {
                    // Extension ajoutée d'office ; une interface vide, ouverte aussitôt dans l'éditeur.
                    std::string name = m_requestName;
                    if (!name.ends_with(InterfaceExtension)) {
                        name += InterfaceExtension;
                    }
                    const std::filesystem::path file = m_requestPath / Platform::PathFromUtf8(name);
                    std::error_code error;
                    if (std::filesystem::exists(file, error)) {
                        Platform::ShowError(window, "Nouvelle interface", "« " + name + " » existe déjà.");
                    } else if (auto saved = Interface::SaveFile({}, file); !saved) {
                        Platform::ShowError(window, "Nouvelle interface", saved.error());
                    } else {
                        m_request = NameRequest::None;
                        m_sinceScan = ScanInterval;
                        m_selected = file;
                        if (m_interfaces) {
                            m_interfaces->Open(engine, file);
                        }
                    }
                } else if (create) {
                    const std::filesystem::path folder = m_requestPath / Platform::PathFromUtf8(m_requestName);
                    std::error_code error;
                    if (std::filesystem::exists(folder, error)) {
                        Platform::ShowError(window, "Nouveau dossier", "« " + m_requestName + " » existe déjà.");
                    } else if (!std::filesystem::create_directory(folder, error)) {
                        Platform::ShowError(window, "Nouveau dossier", "Création impossible : " + error.message());
                    } else {
                        m_request = NameRequest::None;
                        m_sinceScan = ScanInterval; // relu à la prochaine frame
                    }
                } else if (Move(engine, document, m_requestPath, m_requestPath.parent_path() / Platform::PathFromUtf8(m_requestName))) {
                    m_request = NameRequest::None;
                }
            }
            ui.SameLine();
            if (ui.Button("Annuler")) {
                m_request = NameRequest::None;
            }
        }
        ui.EndPanel();
    }

    bool ContentBrowser::Move(Engine& engine, SceneDocument& document, const std::filesystem::path& from,
                              const std::filesystem::path& to) {
        const Window& window = engine.GetWindow();
        const std::string name = Platform::ToUtf8(from.filename());
        if (to == from) {
            return true;
        }
        if (document.IsLocked()) {
            Platform::ShowError(window, "Déplacer", "Arrêter le mode Jouer avant de ranger les fichiers.");
            return false;
        }
        // Un .gltf désigne ses .bin et ses images à côté de lui : seul, il les perdrait.
        if (Platform::ToUtf8(from.extension()) == ".gltf") {
            Platform::ShowError(window, "Déplacer", "« " + name + " » utilise des fichiers à côté de lui (.bin, images) : "
                                                    "déplacer ou renommer plutôt le dossier qui les contient.");
            return false;
        }
        // Un dossier dans lui-même ou dans un de ses sous-dossiers.
        if (const std::filesystem::path inside = to.lexically_normal().lexically_relative(from.lexically_normal());
            !inside.empty() && *inside.begin() != "..") {
            Platform::ShowError(window, "Déplacer", "« " + name + " » ne peut pas aller dans lui-même.");
            return false;
        }
        std::error_code error;
        if (std::filesystem::exists(to, error)) {
            Platform::ShowError(window, "Déplacer", "« " + Platform::ToUtf8(to.filename()) + " » existe déjà à cet endroit.");
            return false;
        }
        std::filesystem::rename(from, to, error);
        if (error) {
            Platform::ShowError(window, "Déplacer", "Déplacement impossible : " + error.message());
            return false;
        }

        // Nouveau chemin de tout ce qui était sous `from` (lui compris) ; std::nullopt pour le reste.
        const auto moved = [&](const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
            const std::filesystem::path relative = path.lexically_normal().lexically_relative(from.lexically_normal());
            if (relative.empty() || *relative.begin() == "..") {
                return std::nullopt;
            }
            return relative == "." ? to : to / relative;
        };
        const std::string oldReference = Platform::ToGenericUtf8(from.lexically_relative(m_root));
        const std::string newReference = Platform::ToGenericUtf8(to.lexically_relative(m_root));
        const ComponentRegistry& components = engine.GetComponents();

        // Scènes du projet (à leur nouvelle place si elles ont bougé aussi), puis la scène ouverte, en mémoire.
        std::vector<std::string> failures;
        for (const std::filesystem::path& scenePath : m_scenes) {
            const std::filesystem::path path = moved(scenePath).value_or(scenePath);
            Scene scene;
            if (!SceneSerializer::Load(scene, components, path)) {
                failures.push_back(Platform::ToUtf8(path.filename()));
                continue;
            }
            if (ReplaceReferences(scene, components, oldReference, newReference)
                && !SceneSerializer::Save(scene, components, path)) {
                failures.push_back(Platform::ToUtf8(path.filename()));
            }
        }
        for (const std::filesystem::path& interfacePath : m_interfaceFiles) {
            const std::filesystem::path path = moved(interfacePath).value_or(interfacePath);
            auto loaded = Interface::LoadFile(path);
            if (!loaded) {
                failures.push_back(Platform::ToUtf8(path.filename()));
                continue;
            }
            if (ReplaceImageReferences(loaded->roots, oldReference, newReference) && !Interface::SaveFile(*loaded, path)) {
                failures.push_back(Platform::ToUtf8(path.filename()));
            }
        }
        if (m_interfaces) {
            m_interfaces->FileMoved(from, to, oldReference, newReference);
        }
        ReplaceReferences(engine.GetScene(), components, oldReference, newReference);
        if (const std::optional<std::filesystem::path> openScene = moved(document.GetPath())) {
            document.FileRenamed(document.GetPath(), *openScene);
        }
        if (Project* project = engine.GetProject()) {
            if (const std::optional<std::filesystem::path> startup = moved(project->GetStartupScenePath())) {
                SetStartupScene(engine, *startup);
            }
        }
        if (m_selected == from) {
            m_selected = to;
        }
        if (const std::optional<std::filesystem::path> folder = moved(m_folder)) {
            m_folder = *folder;
        }
        if (const std::optional<std::filesystem::path> folder = moved(m_sceneFolder)) {
            m_sceneFolder = *folder;
        }
        m_sinceScan = ScanInterval; // relu à la prochaine frame
        if (!failures.empty()) {
            std::string list;
            for (const std::string& failure : failures) {
                list += "\n  " + failure;
            }
            Platform::ShowError(window, "Déplacer", "« " + name + " » a été déplacé, mais ces fichiers n'ont pas pu être "
                                                    "mises à jour (à corriger à la main) :" + list);
        }
        return true;
    }

    void ContentBrowser::ShowSceneMenu(UI::Context& ui, Engine& engine, SceneDocument& document,
                                       const std::filesystem::path& path) {
        if (!ui.BeginContextMenu()) {
            return;
        }
        if (ui.MenuItem("Ouvrir", {}, document.GetPath().lexically_normal() != path.lexically_normal())) {
            document.OpenFile(engine, path);
        }
        if (ui.MenuItem("Renommer...")) {
            const std::string name = Platform::ToUtf8(path.filename());
            m_renaming = path;
            m_newName = name.substr(0, name.size() - SceneExtension.size());
        }
        if (ui.MenuItem("Définir comme scène de démarrage", {}, !IsStartupScene(engine, path))) {
            SetStartupScene(engine, path);
        }
        if (ui.MenuItem("Afficher dans l'explorateur")) {
            Platform::ShowInExplorer(path);
        }
        if (ui.MenuItem("Supprimer")) {
            Delete(engine, document, path);
        }
        ui.EndMenu();
    }

    void ContentBrowser::Delete(Engine& engine, SceneDocument& document, const std::filesystem::path& path) {
        const Window& window = engine.GetWindow();
        const std::string name = Platform::ToUtf8(path.filename());
        if (IsStartupScene(engine, path)) {
            Platform::ShowError(window, "Supprimer", "« " + name + " » est la scène de démarrage du projet : "
                                                     "Game.exe ne pourrait plus démarrer.");
            return;
        }
        if (!Platform::AskYesNo(window, "Supprimer", "Envoyer « " + name + " » à la corbeille ?")) {
            return;
        }
        if (auto deleted = Platform::MoveToRecycleBin(path); !deleted) {
            Platform::ShowError(window, "Supprimer", deleted.error());
            return;
        }
        document.ForgetFile(path);
        if (m_interfaces) {
            m_interfaces->FileDeleted(path);
        }
        m_sinceScan = ScanInterval; // relu à la prochaine frame
    }

    bool ContentBrowser::IsStartupScene(const Engine& engine, const std::filesystem::path& path) {
        const Project* project = engine.GetProject();
        return project && project->GetStartupScenePath().lexically_normal() == path.lexically_normal();
    }

    void ContentBrowser::SetStartupScene(Engine& engine, const std::filesystem::path& path) {
        Project* project = engine.GetProject();
        if (!project) {
            return;
        }
        // Chemin relatif au dossier des assets, comme dans le fichier : le projet sert aussi depuis bin/.
        const std::filesystem::path relative = path.lexically_normal().lexically_relative(project->assetsDirectory);
        if (relative.empty() || *relative.begin() == "..") {
            Platform::ShowError(engine.GetWindow(), "Scène de démarrage", "La scène doit être dans le dossier des assets.");
            return;
        }
        const std::filesystem::path previous = std::exchange(project->startupScene, relative);
        if (auto saved = ProjectFile::Save(*project); !saved) {
            project->startupScene = previous; // le fichier n'a pas changé : le projet en mémoire non plus
            Platform::ShowError(engine.GetWindow(), "Scène de démarrage", "Projet non enregistré : " + saved.error());
        }
    }

    void ContentBrowser::DrawRenameWindow(Engine& engine, SceneDocument& document) {
        if (m_renaming.empty()) {
            return;
        }
        UI::Context& ui = engine.GetUi();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        const float width = 400.0f;
        if (ui.BeginPanel("Renommer la scène", {(static_cast<float>(screen.x) - width) * 0.5f, static_cast<float>(screen.y) * 0.3f},
                          width)) {
            ui.TextDisabled(Platform::ToUtf8(m_renaming.filename()));
            // Entrée dans le champ valide aussi ; l'extension .scene.json est ajoutée d'office.
            const bool entered = ui.InputText("Nouveau nom", m_newName);
            ui.Separator();
            if ((ui.Button("Renommer") || entered) && Rename(engine, document, m_newName)) {
                m_renaming.clear();
            }
            ui.SameLine();
            if (ui.Button("Annuler")) {
                m_renaming.clear();
            }
        }
        ui.EndPanel();
    }

    bool ContentBrowser::Rename(Engine& engine, SceneDocument& document, const std::string& name) {
        const Window& window = engine.GetWindow();
        std::string fileName = name;
        if (fileName.ends_with(SceneExtension)) {
            fileName.resize(fileName.size() - SceneExtension.size());
        }
        // Caractères refusés par Windows dans un nom de fichier.
        if (!IsValidName(fileName)) {
            Platform::ShowError(window, "Renommer", "Nom invalide : il ne doit pas être vide ni contenir \\ / : * ? \" < > |");
            return false;
        }
        fileName += SceneExtension;
        const std::filesystem::path from = m_renaming;
        const std::filesystem::path to = from.parent_path() / Platform::PathFromUtf8(fileName);
        if (to.filename() == from.filename()) {
            return true; // rien à faire
        }
        std::error_code error;
        if (std::filesystem::exists(to, error)) {
            Platform::ShowError(window, "Renommer", "« " + fileName + " » existe déjà.");
            return false;
        }
        // Move met aussi à jour les changements de scène qui la désignent, la scène ouverte et celle de démarrage.
        return Move(engine, document, from, to);
    }
}
