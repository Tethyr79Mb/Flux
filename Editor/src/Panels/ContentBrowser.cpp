#include "Panels/ContentBrowser.hpp"

#include "Document/SceneDocument.hpp"

#include <Flux/Core/Engine.hpp>
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
                m_selected.clear();
            }
            m_root = root;
            m_sinceScan = 0.0;
            Scan(root);
        }
        // Dossier affiché supprimé ou renommé hors de l'éditeur : retour à la racine.
        const Entry* folder = FindFolder(m_folder);
        if (!folder) {
            m_folder = m_root;
            folder = &m_tree;
        }

        UI::Context& ui = engine.GetUi();
        const Input& input = engine.GetWindow().GetInput();
        const Math::Vector2i screen = engine.GetWindow().GetSize();
        m_drag.target.clear();
        // Position sans ancrage (l'éditeur l'ancre en bas) : à droite, sous le panneau « Démo » du jeu.
        const float top = 150.0f + ui.GetMenuBarHeight();
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
                DrawFiles(ui, engine, document, *folder);
                ui.EndChild();
                // Clic droit dans le fond (hors vignette) : actions sur le dossier affiché.
                if (ui.BeginContextMenu("fond")) {
                    if (ui.MenuItem("Nouveau dossier...", {}, !document.IsLocked())) {
                        m_request = NameRequest::NewFolder;
                        m_requestPath = m_folder;
                        m_requestName = "Nouveau dossier";
                    }
                    if (ui.MenuItem("Afficher dans l'explorateur")) {
                        Platform::ShowInExplorer(m_folder);
                    }
                    ui.EndMenu();
                }
            }
        }
        ui.EndPanel();

        // Glisser-déposer : commencé en tenant une vignette, posé en relâchant sur un dossier.
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
        DrawNameWindow(engine, document);
        DrawRenameWindow(engine, document);
    }

    void ContentBrowser::Scan(const std::filesystem::path& root) {
        // Récursif ; erreurs ignorées (fichier supprimé pendant la lecture, droits) : l'entrée manque, c'est tout.
        // Les scènes vont dans m_scenes, pas dans l'arbre ; un dossier qui ne contenait que des scènes disparaît.
        m_scenes.clear();
        const auto read = [this](const auto& self, const std::filesystem::path& directory) -> std::vector<Entry> {
            std::vector<Entry> entries;
            std::error_code error;
            for (auto it = std::filesystem::directory_iterator(directory, error);
                 !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
                Entry entry;
                entry.path = it->path();
                entry.name = Platform::ToUtf8(entry.path.filename());
                entry.directory = it->is_directory(error);
                if (entry.directory) {
                    const std::size_t scenes = m_scenes.size();
                    entry.children = self(self, entry.path);
                    if (entry.children.empty() && m_scenes.size() > scenes) {
                        continue;
                    }
                } else if (IsScene(entry.path)) {
                    m_scenes.push_back(entry.path);
                    continue;
                }
                entries.push_back(std::move(entry));
            }
            // Dossiers d'abord, puis ordre alphabétique : le même d'une lecture à l'autre.
            std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
                return a.directory != b.directory ? a.directory : a.name < b.name;
            });
            return entries;
        };
        m_tree = {root, "assets", true, root.empty() ? std::vector<Entry>{} : read(read, root)};
        std::ranges::sort(m_scenes, [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.parent_path() != b.parent_path() ? a.parent_path() < b.parent_path() : a.filename() < b.filename();
        });
    }

    const ContentBrowser::Entry* ContentBrowser::FindFolder(const std::filesystem::path& path) const {
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
        return m_root.empty() ? nullptr : find(find, m_tree);
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

    // Contenu du dossier en vignettes : ses dossiers puis ses fichiers, en grille.
    void ContentBrowser::DrawFiles(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& folder) {
        if (folder.children.empty()) {
            ui.TextDisabled("Dossier vide. Clic droit : nouveau dossier.\nGlisser une vignette sur un dossier pour l'y ranger.");
            return;
        }
        constexpr float TileWidth = 84.0f;
        const float spacing = ui.GetStyle().itemSpacing;
        const int columns = std::max(1, static_cast<int>((ui.GetAvailableWidth() + spacing) / (TileWidth + spacing)));
        int column = 0;
        for (const Entry& entry : folder.children) {
            if (column++ % columns != 0) {
                ui.SameLine();
            }
            const auto [badge, color] = Badge(entry);
            ui.PushId(entry.name);
            const UI::Context::TileResult tile = ui.Tile(entry.name, entry.directory ? UI::Context::TileIcon::Folder : UI::Context::TileIcon::File,
                                                                  badge, color, entry.path == m_selected, TileWidth,
                                                                  GetIcon(engine, entry));
            if (tile.clicked) {
                m_selected = entry.path;
            }
            if (tile.doubleClicked && entry.directory) {
                m_folder = entry.path;
                m_selected.clear();
            }
            if (ui.IsItemActive() && m_drag.path.empty() && !document.IsLocked()) {
                m_drag = {};
                m_drag.path = entry.path;
                m_drag.pressMouse = engine.GetWindow().GetInput().GetMousePosition();
            }
            if (entry.directory) {
                DropTarget(ui, entry.path);
            }
            ShowContextMenu(ui, engine, document, entry);
            ui.PopId();
            if (tile.doubleClicked && entry.directory) {
                break; // le contenu affiché change : la suite de la liste n'est plus la bonne
            }
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

    void ContentBrowser::ShowContextMenu(UI::Context& ui, Engine& engine, SceneDocument& document, const Entry& entry) {
        if (!ui.BeginContextMenu()) {
            return;
        }
        if (entry.directory && ui.MenuItem("Ouvrir")) {
            m_folder = entry.path;
            m_selected.clear();
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
        const bool create = m_request == NameRequest::NewFolder;
        if (ui.BeginPanel(create ? "Nouveau dossier" : "Renommer", {(static_cast<float>(screen.x) - width) * 0.5f,
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
        m_sinceScan = ScanInterval; // relu à la prochaine frame
        if (!failures.empty()) {
            std::string list;
            for (const std::string& failure : failures) {
                list += "\n  " + failure;
            }
            Platform::ShowError(window, "Déplacer", "« " + name + " » a été déplacé, mais ces scènes n'ont pas pu être "
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
        const bool startup = IsStartupScene(engine, from);
        std::filesystem::rename(from, to, error);
        if (error) {
            Platform::ShowError(window, "Renommer", "Renommage impossible : " + error.message());
            return false;
        }
        document.FileRenamed(from, to);
        if (startup) {
            SetStartupScene(engine, to); // sinon Game.exe chercherait l'ancien nom
        }
        m_sinceScan = ScanInterval; // relu à la prochaine frame
        return true;
    }
}
